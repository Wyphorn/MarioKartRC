#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <Adafruit_NeoPixel.h>
#include <DFRobotDFPlayerMini.h>
#include <ArduinoOTA.h>
#include "mk_protocol.h"
#include "mk_clock_guard.h"
#include "mk_watchdog.h"
#include "star_envelope.h"   // erzeugt von tools/make_star_envelope.py
#if __has_include("mk_secrets.h")
#include "mk_secrets.h"   // WLAN für den Update-Modus, Vorlage: mk_secrets.example.h
#endif

// ── Sound tables ─────────────────────────────────────────────────────────────
// Track numbers match DFPlayer filenames: track 2 → 002.mp3.
// Folder order matches gCharFolder (1=Mario … 8=Rosalina).
// Add/remove track numbers as you place files on the SD card.
// Empty array = nothing plays for that mood/character.

struct CharSounds {
    const uint8_t* joy;   uint8_t joyCount;
    const uint8_t* sad;   uint8_t sadCount;
};

// Mario (folder 01)
static const uint8_t kJoy_Mario[] = {1,2,4};
static const uint8_t kSad_Mario[] = {3,5};

// Yoshi (folder 03)
static const uint8_t kJoy_Yoshi[] = {1,2,4};
static const uint8_t kSad_Yoshi[] = {3,5};

// Bowser (folder 04)
static const uint8_t kJoy_Bowser[] = {1,4};
static const uint8_t kSad_Bowser[] = {2,3,5};

// DK (folder 05)
static const uint8_t kJoy_DK[] = {1,3,5};
static const uint8_t kSad_DK[] = {2,4};

// Luigi (folder 02)
static const uint8_t kJoy_Luigi[] = {1,2,4};
static const uint8_t kSad_Luigi[] = {3,5};

// Peach (folder 06)
static const uint8_t kJoy_Peach[] = {1,2,4};
static const uint8_t kSad_Peach[] = {3,5};

// Toad (folder 07)
static const uint8_t kJoy_Toad[] = {1,2,4};
static const uint8_t kSad_Toad[] = {4,5};

// Rosalina (folder 08)
static const uint8_t kJoy_Rosalina[] = {1,2,4};
static const uint8_t kSad_Rosalina[] = {4,5};

// ── Game sounds (folder 09) ───────────────────────────────────────────────────
// Charakter-unabhängige Sounds, ausgelöst durch Spielereignisse (Item etc.).
// Tracknummer = Dateiname: SND_BANANA 1 → 001.mp3, SND_STAR 7 → 007.wav, …
// 0 = nicht belegt / noch keine Datei vorhanden.

static constexpr uint8_t FOLDER_GAME  = 9;

static constexpr uint8_t SND_STAR     = 7;   // 007.wav – Stern (geloopt, 12.4 s)
static constexpr uint8_t SND_BANANA   = 1;   // 001.mp3 – Banane ablegen
static constexpr uint8_t SND_SHELL    = 0;   // noch nicht belegt
static constexpr uint8_t SND_BOOST    = 0;   // noch nicht belegt
static constexpr uint8_t SND_FINISH   = 0;   // noch nicht belegt

#define SOUNDS(chr) { kJoy_##chr, sizeof(kJoy_##chr), kSad_##chr, sizeof(kSad_##chr) }
static const CharSounds kSounds[8] = {
    SOUNDS(Mario),
    SOUNDS(Luigi),
    SOUNDS(Yoshi),
    SOUNDS(Bowser),
    SOUNDS(DK),
    SOUNDS(Peach),
    SOUNDS(Toad),
    SOUNDS(Rosalina),
};
#undef SOUNDS

// ── Pins ─────────────────────────────────────────────────────────────────────
static constexpr int PIN_ADC_CHAR = 2;
static constexpr int PIN_DF_TX    = 18;  // ESP32 TX → DFPlayer RX
static constexpr int PIN_DF_RX    = 19;  // ESP32 RX ← DFPlayer TX
static constexpr int PIN_SCL      = 0;
static constexpr int PIN_SDA      = 1;
// Kart-Board V2 hat fürs Routing eine andere Pinbelegung (env car_v2,
// -DKART_REV=2), siehe CLAUDE.md. Falsche Firmware aufs falsche Board: Motor
// steht oder läuft falsch, LEDs dunkel — und der Servo bekommt ggf. das
// Motor-PWM und fährt an den Anschlag. Der Bootlog nennt die Board-Version.
#ifndef KART_REV
#define KART_REV 1
#endif
#if KART_REV == 2
// V2 (Layout 2026-10-04 gedreht): LED-Daten gehen über J3 Pin 4 zum
// Schalter-Platinchen (Status-LED, dahinter die Figur).
static constexpr int PIN_ADC_BAT  = 3;
static constexpr int PIN_LPWM     = 4;
static constexpr int PIN_RPWM     = 5;
static constexpr int PIN_SERVO    = 6;
static constexpr int PIN_LED      = 7;
static constexpr int PIN_IR       = 20;
#else
static constexpr int PIN_ADC_BAT  = 4;
static constexpr int PIN_RPWM     = 5;
static constexpr int PIN_LPWM     = 6;
static constexpr int PIN_SERVO    = 7;
static constexpr int PIN_LED      = 20;
static constexpr int PIN_IR       = 3;
#endif
static constexpr int PIN_LIGHT_MAIN = 15;  // Front weiß + Heck rot (ein NPN)
// Hauptlicht über PWM statt nur an/aus — im Stern-Modus pulsiert es zur
// Lautstärke des Lieds. Der NPN schaltet 1 kHz problemlos, das Auge sieht kein
// Flackern.
static constexpr uint32_t LIGHT_PWM_HZ   = 1000;
static constexpr uint8_t  LIGHT_PWM_BITS = 8;
static void lightMainWrite(uint8_t duty) {
    static int last = -1;
    if (duty == last) return;
    last = duty;
    ledcWrite(PIN_LIGHT_MAIN, duty);
}
static constexpr int PIN_LIGHT_REV  = 14;  // Heck weiß, Rückwärtsgang

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr int LED_COUNT    = 11;
static constexpr uint8_t LSM_ADDR = 0x6A;

// Servo limits with 5° mechanical buffer (1000–2000 → 1050–1950)
static constexpr int SERVO_MIN_US = 1050;
static constexpr int SERVO_MAX_US = 1950;
static constexpr int SERVO_MID_US = 1500;
// Servo direkt über LEDC statt ESP32Servo — die Bibliothek bindet den Pin unter
// Core 3.x doppelt an und meldet bei jedem Start einen falschen Fehler.
// 50 Hz = 20000 µs Periode, 14 bit → 16384 Stufen, ~1.2 µs Auflösung.
static constexpr uint32_t SERVO_FREQ_HZ   = 50;
static constexpr uint8_t  SERVO_RES_BITS  = 14;
static constexpr uint32_t SERVO_PERIOD_US = 1000000 / SERVO_FREQ_HZ;

static void servoWriteUs(int us) {
    // Nur bei Änderung schreiben: jedes ledcWrite stößt eine Aktualisierung
    // der PWM an, bei mehreren Schreibvorgängen pro Periode kann der Servo
    // einen Zwischenwert als Puls bekommen und zucken.
    static uint32_t lastDuty = UINT32_MAX;
    uint32_t duty = (uint32_t)us * ((1u << SERVO_RES_BITS) - 1) / SERVO_PERIOD_US;
    if (duty == lastDuty) return;
    lastDuty = duty;
    ledcWrite(PIN_SERVO, duty);
}

// ── Battery thresholds (2S LiPo / 2× 18650) ──────────────────────────────────
// Spannungsteiler: R1 (VBat→ADC) + R2 (ADC→GND) — Werte anpassen wenn verdrahtet!
static constexpr float BAT_R1          = 100000.0f;
static constexpr float BAT_R2          =  47000.0f;
static constexpr float BAT_DIVIDER_INV = (BAT_R1 + BAT_R2) / BAT_R2;
// V2: Der Teiler misst hinter der Puffer-Diode (Schottky SS34) auf dem
// Schalter-Platinchen, also um deren Durchlassspannung zu niedrig. Typisch
// 0.3 V bei den 0.3–1 A der Elektronik — ohne Ausgleich griffe der
// Tiefentladeschutz schon bei echten ~6.7 V statt 6.4 V.
// Nachbau: D2 durch Drahtbrücke ersetzt → hier 0.0f eintragen, sonst greift
// der Tiefentladeschutz erst bei echten ~6.1 V.
#if KART_REV == 2
static constexpr float BAT_DIODE_DROP_V = 0.30f;
#else
static constexpr float BAT_DIODE_DROP_V = 0.0f;
#endif
static constexpr float BAT_WARN_V      = 6.6f;  // 3.3V/Zelle
// Tiefentladeschutz für ungeschützte 18650: liegt der Akku BAT_OFF_SAMPLES
// Messungen in Folge (1 Hz = 30 s) unter BAT_OFF_V, geht das Auto in Deep Sleep
// ohne Weckquelle. Jede Messung zählt, auch unter Last — im Rennen steht das
// Auto nie lange genug. Solange der Akku Reserve hat, erholt sich die Spannung
// beim Gaswegnehmen über die Schwelle und setzt den Zähler zurück; erst wenn
// auch die entlastete Spannung darunter bleibt, wird abgeschaltet. 30 s
// Volllast am Stück können früher auslösen — kostet Laufzeit, nicht die Zelle.
static constexpr float   BAT_OFF_V       = 6.4f;  // 3.2V/Zelle
static constexpr uint8_t BAT_OFF_SAMPLES = 30;

// ── EEPROM layout ─────────────────────────────────────────────────────────────
static constexpr int EEPROM_SIZE       = 32;
static constexpr int EE_ADDR_CHANNEL   = 0;   // uint8_t
static constexpr int EE_ADDR_TRIM      = 1;   // int8_t
static constexpr int EE_ADDR_BASE_MAC  = 2;   // 6 bytes
static constexpr uint8_t EE_MAGIC      = 0xAB;
static constexpr int EE_ADDR_MAGIC     = 8;
// Lenkungs-Kalibrierung: eigenes Magic, damit eepromClear() (Pairing vergessen)
// sie nicht mitnimmt. 3× uint16 links/mitte/rechts.
static constexpr int EE_ADDR_SERVO_MAGIC = 9;
static constexpr int EE_ADDR_SERVO       = 10;   // 6 bytes
static constexpr uint8_t EE_SERVO_MAGIC  = 0xC5;

// ── Globals ───────────────────────────────────────────────────────────────────
static Adafruit_NeoPixel   gLeds(LED_COUNT, PIN_LED, NEO_GRB + NEO_KHZ800);
static HardwareSerial      gDfSerial(1);
static DFRobotDFPlayerMini gDf;

static void playGameSound(uint8_t track) {
    if (track > 0) gDf.playFolder(FOLDER_GAME, track);
}

static uint8_t  gMyMac[6];
static uint8_t  gBaseMac[6];
static bool     gPaired       = false;
static uint8_t  gSlot         = 0;
static uint8_t  gChannel      = MK_ESPNOW_CHANNEL;
static int8_t   gTrim         = 0;
// Kalibrierte Lenkung (µs), Standard = bisherige feste Grenzen. Links kann auch
// der größere Wert sein, je nach Einbaulage des Servos.
static int16_t  gServoLeft    = SERVO_MIN_US;
static int16_t  gServoCenter  = SERVO_MID_US;
static int16_t  gServoRight   = SERVO_MAX_US;
// Live-Vorschau beim Kalibrieren (aus dem Callback, Loop wertet aus)
static volatile uint16_t gServoPreviewUs = 0;
static volatile uint32_t gServoPreviewMs = 0;
static volatile bool     gServoSavePending = false;
static uint16_t          gServoSaveUs[3];
static uint8_t  gIrId         = 0;       // 0 = IR off
static uint32_t gRumbleUntilMs   = 0;
static uint32_t gLastCollisionMs = 0;
static bool     gImuOk        = false;
static bool     gDfOk         = false;

// Character info
static uint8_t  gCharFolder   = 1;       // DFPlayer folder = character index 1-8
static const char* gCharName  = "?";

// Control state
static int8_t   gThrottle     = 0;
static int8_t   gSteering     = 0;
static uint8_t  gButtons      = 0;
static uint8_t  gPrevButtons  = 0;
static bool     gLightsOn     = false;

// Stern (Grün): Regenbogen auf LED 1–10, solange das Stern-Lied läuft. Die
// Datei ist schon geloopt (4 Durchläufe, 12.4 s) — ein DFPlayer-Neustart pro
// Durchlauf hätte hörbare Lücken. Ende über die Track-Ende-Meldung, STAR_MAX_MS
// als Obergrenze, falls sie verloren geht oder kein DFPlayer da ist. Meldungen
// in den ersten STAR_MIN_MS zählen nicht — die könnten vom vorigen Sound sein.
static constexpr uint32_t STAR_MAX_MS = 20000;
static constexpr uint32_t STAR_MIN_MS = 500;
static bool     gStarActive   = false;
static uint32_t gStarStartMs  = 0;
static volatile uint32_t gLastPacketMs = 0;   // wird im ESP-NOW-Callback gesetzt

// Wie lange ist das letzte Steuerpaket her? Nie direkt "nowMs() - gLastPacketMs"
// rechnen: der Callback läuft in einem eigenen Task und kann gLastPacketMs genau
// zwischen dem nowMs()-Aufruf und dem Lesen neu setzen — dann ist der Wert
// jünger als "jetzt", die Subtraktion läuft unter 0 und ergibt ~4 Mrd. ms.
// Die Neustarts im Leerlauf ("30s nichts von der FB" bei 50 Paketen/s,
// 2026-09-25) kamen allerdings vom Rücksprung der Systemuhr, siehe
// mk_clock_guard.h — seit nowMs() ist das hier nur noch die zweite Absicherung.
static uint32_t msSinceLastPacket() {
    uint32_t last = gLastPacketMs;   // erst den Zeitstempel ...
    uint32_t now  = nowMs();        // ... dann die Uhr
    return (int32_t)(now - last) < 0 ? 0 : now - last;
}
static constexpr uint32_t CONTROL_TIMEOUT_MS = 200;
// Verbindungsverlust auf dem Betriebskanal: das Auto bleibt gepairt und wartet
// auf seine FB — die ist Master und meldet sich auf demselben Kanal mit
// derselben MAC zurück (z.B. nach einem FB-Neustart). Erst nach so langer
// Funkstille gilt die FB als weg und das Auto fällt auf Kanal 1 zurück.
// Absichtlich lang: jeder Rückfall auf Kanal 1 kann die Zuordnung FB↔Auto
// ändern, weil dort die erste FB gewinnt.
static constexpr uint32_t LINK_LOST_MS  = 30000;
static constexpr uint32_t LINK_SHOWN_MS = 1000;   // LED: ab hier "Verbindung weg"
// true = Pairing nach dem Einschalten aus dem EEPROM übernommen, aber noch kein
// Paket gesehen. Dann gilt das kurze PAIR_TIMEOUT_MS statt LINK_LOST_MS.
static bool gRestored = false;

// Pairing state machine
enum class PairState { BEACONING, PAIRED };
static PairState gPairState = PairState::BEACONING;
static uint32_t  gLastBeaconMs   = 0;
static uint32_t  gPairingStartMs = 0;
static constexpr uint32_t BEACON_INTERVAL_MS = 500;
static constexpr uint32_t PAIR_TIMEOUT_MS    = 5000;

// LED state
static uint32_t gLedPhaseMs = 0;
static int8_t   gMappingSlot = 0;  // 0 = normal, 1-8 = show slot color

// Battery state — gBatWired auf true setzen sobald Spannungsteiler verdrahtet
static bool    gBatWired   = true;
static float   gBatVoltage = 8.4f;
static uint8_t gBatLevel   = 5;     // 0–5; 0=kritisch/Cutoff, 5=voll
static bool    gBatCutoff  = false; // true = Abschaltung läuft, nichts mehr ansteuern

// ── EEPROM helpers ────────────────────────────────────────────────────────────
static void eepromLoad() {
    EEPROM.begin(EEPROM_SIZE);
    if (EEPROM.read(EE_ADDR_SERVO_MAGIC) == EE_SERVO_MAGIC) {
        uint16_t v[3];
        EEPROM.get(EE_ADDR_SERVO, v);
        gServoLeft = v[0]; gServoCenter = v[1]; gServoRight = v[2];
        Serial.printf("[SERVO] kalibriert: links %u  mitte %u  rechts %u µs\n", v[0], v[1], v[2]);
    }
    if (EEPROM.read(EE_ADDR_MAGIC) != EE_MAGIC) return;
    gChannel = EEPROM.read(EE_ADDR_CHANNEL);
    gTrim    = (int8_t)EEPROM.read(EE_ADDR_TRIM);
    for (int i = 0; i < 6; i++) gBaseMac[i] = EEPROM.read(EE_ADDR_BASE_MAC + i);
}

static void eepromSaveChannel(uint8_t ch) {
    gChannel = ch;
    EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.write(EE_ADDR_CHANNEL, ch);
    { MkWdtPause wdtPause; EEPROM.commit(); }   // Flash-Schreiben haelt auch die Loop an
}

// Prüft und speichert die Lenkungs-Kalibrierung. false = unplausibel, verworfen.
static bool eepromSaveServo(const uint16_t* v) {
    uint16_t lo = min(v[0], v[2]), hi = max(v[0], v[2]);
    bool ok = lo >= MK_SERVO_HARD_MIN && hi <= MK_SERVO_HARD_MAX
           && hi - lo >= MK_SERVO_MIN_SPAN && v[1] > lo && v[1] < hi;
    if (!ok) {
        Serial.printf("[SERVO] Kalibrierung verworfen: %u / %u / %u µs\n", v[0], v[1], v[2]);
        return false;
    }
    gServoLeft = v[0]; gServoCenter = v[1]; gServoRight = v[2];
    gTrim = 0;   // die Mitte ist frisch eingestellt, alter Trim gilt nicht mehr
    EEPROM.write(EE_ADDR_SERVO_MAGIC, EE_SERVO_MAGIC);
    uint16_t c[3] = {v[0], v[1], v[2]};
    EEPROM.put(EE_ADDR_SERVO, c);
    EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.write(EE_ADDR_TRIM, 0);
    { MkWdtPause wdtPause; EEPROM.commit(); }
    Serial.printf("[SERVO] gespeichert: links %u  mitte %u  rechts %u µs\n", v[0], v[1], v[2]);
    return true;
}

static void eepromSaveTrim(int8_t trim) {
    gTrim = trim;
    EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.write(EE_ADDR_TRIM, (uint8_t)trim);
    { MkWdtPause wdtPause; EEPROM.commit(); }   // Flash-Schreiben haelt auch die Loop an
}

static void eepromSaveBaseMac(const uint8_t* mac) {
    memcpy(gBaseMac, mac, 6);
    EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC);
    for (int i = 0; i < 6; i++) EEPROM.write(EE_ADDR_BASE_MAC + i, mac[i]);
    { MkWdtPause wdtPause; EEPROM.commit(); }   // Flash-Schreiben haelt auch die Loop an
}

// Nur das Pairing vergessen (Kanal → 1, FB-MAC → leer). Magic und Trim bleiben
// stehen — früher wurde hier das Magic-Byte gelöscht, eepromLoad() verwarf
// danach den ganzen Block und der Servo-Trim war bei jedem Rückfall auf
// Kanal 1 weg. Der Trim gehört zur Mechanik des Autos, nicht zum Pairing.
static void eepromClear() {
    EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.write(EE_ADDR_CHANNEL, MK_ESPNOW_CHANNEL);
    for (int i = 0; i < 6; i++) EEPROM.write(EE_ADDR_BASE_MAC + i, 0x00);
    { MkWdtPause wdtPause; EEPROM.commit(); }   // Flash-Schreiben haelt auch die Loop an
}

// ── Motor ─────────────────────────────────────────────────────────────────────
static void motorSet(int8_t throttle) {
    if (throttle > 0) {
        ledcWrite(PIN_RPWM, map(throttle, 1, 100, 0, 255));
        ledcWrite(PIN_LPWM, 0);
    } else if (throttle < 0) {
        ledcWrite(PIN_RPWM, 0);
        ledcWrite(PIN_LPWM, map(-throttle, 1, 100, 0, 255));
    } else {
        ledcWrite(PIN_RPWM, 0);
        ledcWrite(PIN_LPWM, 0);
    }
}

// ── Servo ─────────────────────────────────────────────────────────────────────
static void servoSet(int8_t steering) {
    // Stick -100..0..100 → kalibrierter linker Anschlag..Mitte..rechter
    // Anschlag (MSG_SERVO_CAL). Jede Seite getrennt, so bleibt der volle Weg
    // auf beiden Seiten nutzbar. Der Trim (MK_TRIM_STEP_US pro Stufe) verschiebt
    // nur die Mitte, in Richtung "rechts" positiv, und bleibt innerhalb der
    // Anschläge. Links kann der größere µs-Wert sein (Einbaulage).
    int lo = min(gServoLeft, gServoRight), hi = max(gServoLeft, gServoRight);
    int dir = (gServoRight >= gServoLeft) ? 1 : -1;
    int center = constrain(gServoCenter + dir * gTrim * MK_TRIM_STEP_US, lo, hi);
    int us = (steering < 0) ? map(steering, -100, 0, gServoLeft, center)
                            : map(steering, 0, 100, center, gServoRight);
    us = constrain(us, lo, hi);
    servoWriteUs(constrain(us, MK_SERVO_HARD_MIN, MK_SERVO_HARD_MAX));
}

// ── I2C helpers ───────────────────────────────────────────────────────────────
static uint8_t i2cRead(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(addr, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

static void i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

// ── Character ID ──────────────────────────────────────────────────────────────
static const char* const kCharNames[8] = {
    "Mario", "Luigi", "Yoshi", "Bowser", "DK", "Peach", "Toad", "Rosalina"
};

// Liefert den DFPlayer-Ordner (1–8) zur aktuellen DIP-Stellung, Mittel aus 4 Messungen.
static uint8_t readCharacterFolder() {
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += analogRead(PIN_ADC_CHAR);
    int raw = sum / 4;
    if (raw > 3940) return 8;  // Rosalina: 3.3V liegt über der ADC-Skala
    float v = raw * 2.985f / 4095.0f;
    // Schwellen = Mitte zwischen den Nachbarspannungen der DIP-Leiter
    // (R8 4.7k / R10 10k / R13 20k / R15 51k gegen 10k nach 3V3).
    if (v < 0.884f) return 1;  // Mario
    if (v < 1.188f) return 2;  // Luigi
    if (v < 1.485f) return 3;  // Yoshi
    if (v < 1.798f) return 4;  // Bowser
    if (v < 2.073f) return 5;  // DK
    if (v < 2.480f) return 6;  // Peach
    return 7;                  // Toad
}

static void readCharacter() {
    gCharFolder = readCharacterFolder();
    gCharName   = kCharNames[gCharFolder - 1];
}

// Erkennungssound: immer der erste Joy-Track, damit jeder Charakter gleich klingt.
static void playCharIntro() {
    if (!gDfOk) return;
    const CharSounds& s = kSounds[gCharFolder - 1];
    if (s.joyCount > 0) gDf.playFolder(gCharFolder, s.joy[0]);
}

// Nur im Pairing: DIP-Wechsel erkennen. Ein neuer Stand gilt erst, wenn er
// CHAR_STABLE_MS unverändert ansteht — sonst wären beim Umlegen mehrerer
// Schalter die Zwischenstellungen als eigene Charaktere zu hören.
static constexpr uint32_t CHAR_POLL_MS   = 100;
static constexpr uint32_t CHAR_STABLE_MS = 1000;

static void charPoll(uint32_t now) {
    static uint32_t lastPoll    = 0;
    static uint8_t  candidate   = 0;
    static uint32_t candidateMs = 0;
    if (now - lastPoll < CHAR_POLL_MS) return;
    lastPoll = now;

    uint8_t f = readCharacterFolder();
    if (f != candidate) { candidate = f; candidateMs = now; return; }
    if (f == gCharFolder || now - candidateMs < CHAR_STABLE_MS) return;

    readCharacter();
    Serial.printf("[CHAR] Wechsel → %s (folder %d)\n", gCharName, gCharFolder);
    playCharIntro();
}

// ── Battery ───────────────────────────────────────────────────────────────────
static uint8_t gBatLowSamples = 0;

// Tiefentladeschutz: Fahrt unterbinden, alles Abschaltbare aus, trauriger
// Sound, dann Deep Sleep ohne Weckquelle. Nur Aus-/Einschalten holt das Auto
// zurück. Buck, BTS7960-Modul und Servo ziehen weiter ein paar mA — bewusst
// hingenommen, entscheidend ist, dass nicht mehr gefahren wird.
static void carShutdown() {
    mkWatchdogStop();   // 3 s Sound, dann Deep Sleep — RTC-WDT darf nicht weiterlaufen
    gBatCutoff = true;
    Serial.printf("[BAT] %.2fV < %.2fV — Abschaltung (Deep Sleep)\n", gBatVoltage, BAT_OFF_V);
    motorSet(0);
    servoWriteUs(SERVO_MID_US);
    lightMainWrite(0);
    digitalWrite(PIN_LIGHT_REV, LOW);
    digitalWrite(PIN_IR, LOW);
    gLeds.clear();
    gLeds.show();
    if (gDfOk && gCharFolder >= 1 && gCharFolder <= 8) {
        const CharSounds& snd = kSounds[gCharFolder - 1];
        if (snd.sadCount > 0) gDf.playFolder(gCharFolder, snd.sad[esp_random() % snd.sadCount]);
        delay(3000);   // Sound ausspielen lassen
        gDf.sleep();
    }
    if (gImuOk) {
        i2cWrite(LSM_ADDR, 0x10, 0x00);   // Accel power-down
        i2cWrite(LSM_ADDR, 0x11, 0x00);   // Gyro power-down
    }
    // Ausgänge im Deep Sleep auf LOW festhalten: kein Servo-Puls, Motor-PWM,
    // Licht, IR und LED-Daten aus.
    ledcDetach(PIN_SERVO);
    ledcDetach(PIN_RPWM);
    ledcDetach(PIN_LPWM);
    ledcDetach(PIN_LIGHT_MAIN);
    const uint8_t lowPins[] = { PIN_SERVO, PIN_RPWM, PIN_LPWM, PIN_LIGHT_MAIN,
                                PIN_LIGHT_REV, PIN_IR, PIN_LED };
    for (uint8_t pin : lowPins) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        gpio_hold_en((gpio_num_t)pin);
    }
    Serial.flush();
    esp_deep_sleep_start();   // keine Weckquelle konfiguriert
}

static void batUpdate() {
    if (!gBatWired) return;
    // analogReadMilliVolts() nutzt die Werkskalibrierung des ADC — der rohe
    // analogRead()-Wert ist beim C6 nicht linear zur Spannung.
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += analogReadMilliVolts(PIN_ADC_BAT);
    float vAdc = (sum / 4.0f) / 1000.0f;
    gBatVoltage = vAdc * BAT_DIVIDER_INV + BAT_DIODE_DROP_V;
    static uint32_t lastLog = 0;
    if (lastLog == 0 || nowMs() - lastLog >= 30000) {
        lastLog = nowMs();
        Serial.printf("[BAT] %.2fV (%.2fV/Zelle)\n", gBatVoltage, gBatVoltage / 2);
    }
    if (gBatVoltage < BAT_OFF_V) {
        gBatLevel = 0;   // Status-LED blinkt rot, bis abgeschaltet wird
        if (++gBatLowSamples >= BAT_OFF_SAMPLES) carShutdown();
        return;
    }
    gBatLowSamples = 0;
    if      (gBatVoltage < BAT_WARN_V) gBatLevel = 1;
    else if (gBatVoltage < 7.0f)       gBatLevel = 2;
    else if (gBatVoltage < 7.4f)       gBatLevel = 3;
    else if (gBatVoltage < 7.9f)       gBatLevel = 4;
    else                               gBatLevel = 5;
}

static uint8_t readBattery() { return gBatLevel; }

// ── IR LED ────────────────────────────────────────────────────────────────────
static void irSetActive(bool active) {
    // Simple on/off carrier; RMT-based modulation can be added later if needed.
    // For MVP: drive GPIO9 HIGH when active. The TSOP38238 expects 38kHz
    // but basic detection tests with DC drive first.
    digitalWrite(PIN_IR, active ? HIGH : LOW);
}

// ── LEDs ─────────────────────────────────────────────────────────────────────
// LED layout:
//   0    — Status-LED (erste in der Kette)
//   1–10 — Charakter (10 LEDs im Charakterkopf, Regenbogen-Effekt)

static constexpr int LED_STATUS     = 0;
static constexpr int LED_CHAR_FIRST = 1;
static constexpr int LED_CHAR_LAST  = 10;
static constexpr int LED_CHAR_COUNT = LED_CHAR_LAST - LED_CHAR_FIRST + 1;

static uint32_t wheel(uint8_t pos) {
    pos = 255 - pos;
    if (pos < 85)  return gLeds.Color(255 - pos * 3, 0, pos * 3);
    if (pos < 170) { pos -= 85; return gLeds.Color(0, pos * 3, 255 - pos * 3); }
    pos -= 170;    return gLeds.Color(pos * 3, 255 - pos * 3, 0);
}

// Regenbogen-Blink-Effekt: voller Umlauf ~680ms, 50ms dunkel alle 300ms.
// Aufruf: einmal pro ledUpdate()-Tick (~20ms). Event-triggered, nicht im Idle.
static void charRainbowTick() {
    static uint8_t hue = 0;
    uint32_t now = nowMs();
    bool on = (now % 300) >= 50;
    for (int i = LED_CHAR_FIRST; i <= LED_CHAR_LAST; i++)
        gLeds.setPixelColor(i, on ? wheel((hue + (i - LED_CHAR_FIRST) * 256 / LED_CHAR_COUNT) & 0xFF) : 0);
    hue += 3;
}

static void ledUpdate() {
    uint32_t now = nowMs();

    // ── LEDs 1–10: Charakter ───────────────────────────────────────────────────
    if (gMappingSlot >= 1 && gMappingSlot <= 8) {
        const uint8_t* c = MK_MAPPING_COLORS[gMappingSlot - 1];
        uint32_t col = gLeds.Color(c[0], c[1], c[2]);
        for (int i = LED_CHAR_FIRST; i <= LED_CHAR_LAST; i++)
            gLeds.setPixelColor(i, col);
    } else if (gStarActive) {
        charRainbowTick();
    } else {
        // Idle: aus — Regenbogen wird event-triggered via charRainbowTick()
        for (int i = LED_CHAR_FIRST; i <= LED_CHAR_LAST; i++)
            gLeds.setPixelColor(i, 0);
    }

    // ── LED 0: Status ─────────────────────────────────────────────────────────
    uint8_t bat = gBatWired ? readBattery() : 5;  // 0–5, 5=voll
    if (gBatWired && bat == 0) {
        // Akku kritisch (<16%): rot blinkend
        uint32_t col = ((now / 300) % 2) ? gLeds.Color(200, 0, 0) : 0;
        gLeds.setPixelColor(LED_STATUS, col);
    } else if (gBatWired && bat <= 1) {
        // Akku niedrig (<33%): gelb
        gLeds.setPixelColor(LED_STATUS, gLeds.Color(180, 80, 0));
    } else if (gPaired && !gRestored && msSinceLastPacket() < LINK_SHOWN_MS) {
        // Verbunden: grün
        gLeds.setPixelColor(LED_STATUS, gLeds.Color(0, 150, 0));
    } else {
        // Koppeln: orange blinkend (300ms, wie FB)
        uint8_t v = ((now / 300) % 2) ? 180 : 0;
        gLeds.setPixelColor(LED_STATUS, gLeds.Color(v, v / 3, 0));
    }

    gLeds.show();
}

// ── ESP-NOW send helper ───────────────────────────────────────────────────────
static void espnowSend(const uint8_t* dest, const void* data, size_t len) {
    esp_now_send(dest, (const uint8_t*)data, len);
}

// ── ESP-NOW peer management ───────────────────────────────────────────────────
static void addPeer(const uint8_t* mac) {
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;  // 0 = Home-Channel verwenden, kein Mismatch möglich
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_add_peer(&peer);
    }
}

// ── Pairing ───────────────────────────────────────────────────────────────────
static void sendBeacon() {
    uint8_t broadcast[6] = MK_BASE_MAC;
    addPeer(broadcast);

    MK_Beacon beacon;
    beacon.deviceType = DEVICE_CAR;
    beacon.charId     = gCharFolder;
    espnowSend(broadcast, &beacon, sizeof(beacon));
    gLastBeaconMs = nowMs();
    Serial.printf("[PAIR] Beacon → ch%d %s\n", gChannel, gCharName);
}

static void onPaired(const MK_Assign* pkt, const uint8_t* senderMac) {
    gSlot = pkt->slot;
    eepromSaveBaseMac(pkt->baseMac);
    addPeer(pkt->baseMac);

    gPaired       = true;
    gPairState    = PairState::PAIRED;
    gLastPacketMs = nowMs();
    Serial.printf("[PAIR] Slot %d, base=" MACSTR "\n", gSlot, MAC2STR(pkt->baseMac));

    // Send initial feedback so base knows we're alive
    MK_GameFeedback fb{};
    fb.carBat = readBattery();
    fb.trim   = gTrim;
    espnowSend(gBaseMac, &fb, sizeof(fb));
}

// ── Channel switch ────────────────────────────────────────────────────────────
static void applyChannelSwitch(uint8_t ch) {
    eepromSaveChannel(ch);
    if (ch == MK_ESPNOW_CHANNEL) {
        // Race end: deactivate IR, stop motors — pairing stays active
        gIrId = 0;
        irSetActive(false);
        gMappingSlot = 0;
        motorSet(0);
        servoSet(0);
    }
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    // Peer neu registrieren damit er den neuen Home-Channel sieht
    if (gPaired) {
        esp_now_del_peer(gBaseMac);
        addPeer(gBaseMac);
    }
    Serial.printf("[CH] → %d\n", ch);
}

// ── Firmware-Update über WLAN (OTA) ──────────────────────────────────────────
// Ablauf und Protokoll: OTA-Abschnitt in mk_protocol.h. Der Callback setzt nur
// das Flag, die Loop antwortet und schaltet um. Aus dem Update-Modus geht es
// immer per Neustart zurück — auch nach Fehlern, die alte Firmware bleibt dann.
static volatile bool gOtaRequested = false;

// "kart-<charakter>-<letzte 2 MAC-Bytes>", z.B. kart-mario-b924
static void otaHostname(char* out, size_t len) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char name[12];
    size_t i = 0;
    for (; gCharName[i] && i < sizeof(name) - 1; i++) name[i] = tolower(gCharName[i]);
    name[i] = '\0';
    snprintf(out, len, "kart-%s-%02x%02x", name, mac[4], mac[5]);
}

// Nur die Status-LED zeigt den Update-Modus, die Charakter-LEDs bleiben aus.
// blink = true: WLAN-Anmeldung bzw. Fehler; false: im WLAN, wartet/empfängt.
static void otaLed(uint8_t r, uint8_t g, uint8_t b, bool blink) {
    bool on = !blink || (nowMs() / 250) % 2;
    gLeds.clear();
    gLeds.setPixelColor(LED_STATUS, on ? gLeds.Color(r, g, b) : 0);
    gLeds.show();
}

[[noreturn]] static void otaRestart(const char* why) {
    Serial.printf("[OTA] %s — Neustart\n", why);
    uint32_t t0 = nowMs();
    while (nowMs() - t0 < 2000) { otaLed(150, 0, 0, true); delay(20); }   // rot blinkend
    ESP.restart();
    while (true) {}
}

[[noreturn]] static void otaRun(const char* host) {
#ifdef MK_WIFI_SSID
    // Ab hier kein Fahrbetrieb mehr. Die Watchdogs sind für die kurze Loop
    // ausgelegt; WLAN-Anmeldung und Flashen blockieren länger. Die Wartezeiten
    // unten beenden den Modus in jedem Fall per Neustart.
    mkWatchdogStop();
    motorSet(0);
    servoWriteUs(SERVO_MID_US);
    lightMainWrite(0);
    digitalWrite(PIN_LIGHT_REV, LOW);
    irSetActive(false);
    if (gDfOk) gDf.stop();

    esp_now_deinit();
    WiFi.disconnect();
    WiFi.setHostname(host);
    WiFi.begin(MK_WIFI_SSID, MK_WIFI_PASS);
    Serial.printf("[OTA] Update-Modus, verbinde mit \"%s\" ...\n", MK_WIFI_SSID);
    uint32_t t0 = nowMs();
    while (WiFi.status() != WL_CONNECTED) {
        if (nowMs() - t0 > MK_OTA_WIFI_MS) otaRestart("kein WLAN");
        otaLed(120, 0, 160, true);    // lila blinkend: meldet sich am WLAN an
        delay(20);
    }
    Serial.printf("[OTA] WLAN ok, IP %s — warte auf Firmware: %s.local\n",
                  WiFi.localIP().toString().c_str(), host);

    static bool started = false;
    ArduinoOTA.setHostname(host);
    ArduinoOTA.onStart([]() { started = true; Serial.println("[OTA] Empfang läuft"); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        static uint8_t lastPct = 0;
        uint8_t pct = total ? done * 100 / total : 0;
        if (pct / 10 != lastPct / 10) Serial.printf("[OTA] %u %%\n", pct);
        lastPct = pct;
    });
    ArduinoOTA.onEnd([]() { Serial.println("[OTA] fertig, Neustart"); });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("[OTA] Fehler %u\n", (unsigned)e);
        otaRestart("Update abgebrochen");
    });
    ArduinoOTA.begin();   // startet auch mDNS unter <host>.local

    t0 = nowMs();
    while (true) {
        ArduinoOTA.handle();   // nach erfolgreichem Update startet sie neu
        if (!started && nowMs() - t0 > MK_OTA_WAIT_MS) otaRestart("keine Firmware gekommen");
        otaLed(120, 0, 160, false);   // lila dauerhaft: im WLAN, bereit
        delay(10);
    }
#else
    otaRestart("ohne mk_secrets.h gebaut");
#endif
}

// Aus der Loop: antworten, dann umschalten oder weiterfahren.
static void otaHandleRequest() {
    gOtaRequested = false;
    MK_OtaStatus st;
    memset(st.hostname, 0, sizeof(st.hostname));
#ifndef MK_WIFI_SSID
    st.state = OTA_REJ_NOWIFI;
#else
    st.state = (gBatWired && gBatVoltage < BAT_WARN_V) ? OTA_REJ_BAT : OTA_ACCEPTED;
#endif
    if (st.state == OTA_ACCEPTED) otaHostname(st.hostname, sizeof(st.hostname));
    {
        MkWdtPause wdtPause;              // 3× mit Abstand passt nicht in 50 ms
        for (int i = 0; i < 3; i++) {     // die FB sendet den Request auch 3×
            espnowSend(gBaseMac, &st, sizeof(st));
            delay(15);
        }
    }
    Serial.printf("[OTA] Request → %s\n", st.state == OTA_ACCEPTED ? st.hostname
                  : st.state == OTA_REJ_BAT ? "abgelehnt, Akku zu leer"
                  : "abgelehnt, keine WLAN-Daten");
    if (st.state == OTA_ACCEPTED) otaRun(st.hostname);
}

// Hauptlicht: normal an/aus nach gLightsOn (Rot). Im Stern-Modus pulsiert es
// zur Lautstärke des Stern-Lieds (STAR_ENV, 20 ms pro Wert). STAR_ENV_DELAY_MS
// gleicht aus, dass der DFPlayer nach dem Befehl erst etwas später hörbar
// spielt. Quadratisch, damit die Dynamik fürs Auge sichtbar bleibt (LEDs
// wirken linear angesteuert schnell "fast voll"); nie ganz aus.
static constexpr uint32_t STAR_ENV_DELAY_MS = 150;
static void lightsUpdate(uint32_t now) {
    if (gStarActive && now - gStarStartMs >= STAR_ENV_DELAY_MS) {
        uint32_t idx = (now - gStarStartMs - STAR_ENV_DELAY_MS) / STAR_ENV_FRAME_MS;
        if (idx < STAR_ENV_LEN) {
            uint16_t e = STAR_ENV[idx];
            lightMainWrite(max<uint16_t>(e * e / 255, 8));
            return;
        }
    }
    lightMainWrite(gLightsOn ? 255 : 0);
}

// Vorschau der Lenkungs-Kalibrierung aktiv? Zeitstempel erst lesen, dann die
// Uhr — der Callback kann ihn dazwischen neu setzen, die Differenz liefe dann
// unter 0 (gleiches Muster wie msSinceLastPacket()).
static bool servoPreviewActive() {
    uint32_t last = gServoPreviewMs;
    if (!last) return false;
    uint32_t now = nowMs();
    uint32_t age = (int32_t)(now - last) < 0 ? 0 : now - last;
    return age < MK_SERVO_PREVIEW_MS;
}

// ── ESP-NOW receive callback ──────────────────────────────────────────────────
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    const uint8_t* senderMac = info->src_addr;
    if (len < 1) return;
    uint8_t type = data[0];

    switch (type) {
        case MSG_ASSIGN: {
            if (len < (int)sizeof(MK_Assign)) return;
            if (gPaired) return;  // already paired
            onPaired((const MK_Assign*)data, senderMac);
            break;
        }
        case MSG_CONTROL: {
            if (!gPaired || len < (int)sizeof(MK_ControlInput)) return;
            const auto* pkt = (const MK_ControlInput*)data;
            // Nur Variablen setzen — Aktuatoren werden im Main-Loop angesteuert
            int32_t t     = (int32_t)pkt->throttle * pkt->maxSpeed / 10;
            gThrottle     = (int8_t)constrain(t, -100, 100);
            gSteering     = pkt->steering;
            gButtons      = pkt->buttons;
            gLastPacketMs = nowMs();
            gRestored     = false;
            break;
        }
        case MSG_CONFIG: {
            if (!gPaired || len < (int)sizeof(MK_ConfigPacket)) return;
            const auto* pkt = (const MK_ConfigPacket*)data;
            int8_t trim = constrain(pkt->trim, -10, 10);
            if (pkt->save) eepromSaveTrim(trim);   // Grün im Trim-Menü
            else           gTrim = trim;           // Vorschau, EEPROM unverändert
            break;
        }
        case MSG_CHANNEL_SWITCH: {
            if (len < (int)sizeof(MK_ChannelSwitch)) return;
            const auto* pkt = (const MK_ChannelSwitch*)data;
            if (!gPaired) {
                // Das MSG_ASSIGN ist unterwegs verlorengegangen — die FB setzt
                // direkt davor ihre MAC von der Basis-MAC zurück auf die echte,
                // und das erste Paket danach fällt gern weg. Ungepairt den Kanal
                // zu wechseln wäre fatal: das Auto säße auf dem neuen Kanal ohne
                // Pairing, liefe in den 5s-Timeout und würde neu starten, während
                // die FB sich für verbunden hält. Der Absender ist zwangsläufig
                // unsere Gegenstelle, also hier das Pairing nachholen.
                eepromSaveBaseMac(senderMac);
                addPeer(senderMac);
                gSlot         = 1;
                gPaired       = true;
                gPairState    = PairState::PAIRED;
                gLastPacketMs = nowMs();
                Serial.printf("[PAIR] Assign verpasst — via ChannelSwitch gepairt: " MACSTR "\n",
                              MAC2STR(senderMac));
            }
            applyChannelSwitch(pkt->channel);
            break;
        }
        case MSG_MAPPING: {
            if (len < (int)sizeof(MK_Mapping)) return;
            const auto* pkt = (const MK_Mapping*)data;
            gMappingSlot = (pkt->slot == -1) ? 0 : pkt->slot;
            break;
        }
        case MSG_IR_CONFIG: {
            if (len < (int)sizeof(MK_IrConfig)) return;
            const auto* pkt = (const MK_IrConfig*)data;
            gIrId = pkt->irId;
            irSetActive(gIrId > 0);
            Serial.printf("[IR] id=%d %s\n", gIrId, gIrId ? "ON" : "OFF");
            break;
        }
        case MSG_SERVO_CAL: {
            if (!gPaired || len < (int)sizeof(MK_ServoCal)) return;
            if (memcmp(senderMac, gBaseMac, 6) != 0) return;   // nur der eigene Partner
            const auto* pkt = (const MK_ServoCal*)data;
            if (pkt->cmd == SERVO_CAL_PREVIEW) {
                gServoPreviewUs = constrain(pkt->us[0], MK_SERVO_HARD_MIN, MK_SERVO_HARD_MAX);
                gServoPreviewMs = nowMs();
            } else if (pkt->cmd == SERVO_CAL_SAVE && !gServoSavePending) {
                memcpy(gServoSaveUs, pkt->us, sizeof(gServoSaveUs));
                gServoSavePending = true;   // EEPROM nur aus der Loop
                gServoPreviewMs = 0;
            } else if (pkt->cmd == SERVO_CAL_CANCEL) {
                gServoPreviewMs = 0;
            }
            break;
        }
        case MSG_OTA_REQUEST: {
            if (!gPaired || len < (int)sizeof(MK_OtaRequest)) return;
            if (((const MK_OtaRequest*)data)->confirm != MK_OTA_CONFIRM) return;
            if (memcmp(senderMac, gBaseMac, 6) != 0) return;   // nur der eigene Partner
            gOtaRequested = true;
            break;
        }
        default:
            break;
    }
}

// ── IMU init ──────────────────────────────────────────────────────────────────
// ── Collision detection ───────────────────────────────────────────────────────
// Schwelle der Wake-up-Erkennung, gilt fuer jede einzelne Achse nach dem
// Hochpass (ohne Erdanziehung). 1 LSB = Messbereich/64 = 0.25 g bei ±16 g.
// Startwert 3 g, mit Messdaten nachschaerfen.
static constexpr uint8_t  COLLISION_THS_LSB     = 12;    // 3.0 g
static constexpr uint32_t COLLISION_COOLDOWN_MS = 3000;  // kein Re-Trigger innerhalb 3s
static uint8_t gImuVertical = 2;   // 0 = X, 1 = Y, 2 = Z — beim Start ermittelt

// Hochachse = die Achse, auf der im Stand die Erdanziehung liegt. So muss die
// Einbaulage der IMU nicht im Code stehen. Das Auto steht beim Einschalten.
static void imuDetectVertical() {
    delay(20);   // erste Messwerte bei 416 Hz abwarten
    Wire.beginTransmission(LSM_ADDR);
    Wire.write(0x28);  // OUTX_L_XL
    Wire.endTransmission(false);
    Wire.requestFrom(LSM_ADDR, (uint8_t)6);
    if (Wire.available() < 6) return;
    int16_t a[3];
    for (int i = 0; i < 3; i++) a[i] = Wire.read() | (Wire.read() << 8);
    uint8_t best = 2;
    for (uint8_t i = 0; i < 3; i++) if (abs(a[i]) > abs(a[best])) best = i;
    // 1 g = 2048 Counts bei ±16 g — unter ~0.7 g ist die Messung unbrauchbar
    if (abs(a[best]) > 1400) gImuVertical = best;
}

// Abfrage des gelatchten Wake-up-Status (WAKE_UP_SRC, wird beim Lesen
// geloescht). Gezaehlt werden nur Stoesse in der Waagerechten — Stoesse von
// unten (Bodenwellen, Bordstein) sind beim Fahren normal, ein Crash kommt von
// der Seite oder von vorne/hinten.
static void checkCollision() {
    if (!gImuOk) return;
    uint8_t src = i2cRead(LSM_ADDR, 0x1B);
    if (src == 0xFF || !(src & 0x08)) return;          // Lesefehler / kein Ereignis (WU_IA)
    static const uint8_t AXIS_BIT[3] = {0x04, 0x02, 0x01};   // X_WU, Y_WU, Z_WU
    uint8_t horiz = (src & 0x07) & ~AXIS_BIT[gImuVertical];
    if (!horiz) return;                                // nur Hochachse → ignorieren

    uint32_t now = nowMs();
    if (now - gLastCollisionMs > COLLISION_COOLDOWN_MS) {
        Serial.printf("[IMU] KOLLISION %s%s%s → Rumble 2s\n",
                      (horiz & 0x04) ? "X " : "", (horiz & 0x02) ? "Y " : "", (horiz & 0x01) ? "Z " : "");
        gLastCollisionMs = now;
        gRumbleUntilMs   = now + 2000;
    }
}

static void imuInit() {
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setTimeOut(5);   // sonst bis 50 ms Warten bei I2C-Stoerung → Loop-Watchdog
    uint8_t who = i2cRead(LSM_ADDR, 0x0F);
    if (who == 0x6C || who == 0x69) {
        // Stoss-Erkennung im Sensor (Wake-up-Funktion, Registersatz LSM6DS3):
        // er prueft JEDEN Messwert bei 416 Hz, zieht die Erdanziehung per
        // Hochpass ab und merkt sich das Ereignis samt Achse, bis wir es
        // abholen. Vorher lasen wir alle 100 ms einen einzigen Wert — ein
        // Aufprall (5–20 ms) lag meist dazwischen (2026-10-04).
        // Filter (2026-10-04 korrigiert): der Hochpass stand zuerst auf dem
        // Standard ODR/4 ≈ 104 Hz — dann sah die Erkennung fast nur Vibration
        // von Motor/Getriebe, den Aufprall (5–20 ms, ~25–100 Hz) kaum.
        //  - Hochpass ODR/100 ≈ 4 Hz: nimmt Erdanziehung, Anfahren, Kurven raus
        //  - analoger Tiefpass 100 Hz: schneidet Motor-/Getriebevibration ab
        // Die Ausgaberegister bleiben ungefiltert (HP_SLOPE_XL_EN = 0), daraus
        // bestimmt imuDetectVertical() die Hochachse.
        i2cWrite(LSM_ADDR, 0x13, 0x80);  // CTRL4_C: XL_BW_SCAL_ODR — Bandbreite aus BW_XL
        i2cWrite(LSM_ADDR, 0x10, 0x66);  // CTRL1_XL: 416 Hz, ±16 g, Tiefpass 100 Hz
        i2cWrite(LSM_ADDR, 0x11, 0x40);  // CTRL2_G: Gyro 104 Hz, 250 dps
        i2cWrite(LSM_ADDR, 0x17, 0x20);  // CTRL8_XL: HPCF_XL = ODR/100
        i2cWrite(LSM_ADDR, 0x58, 0x11);  // TAP_CFG: SLOPE_FDS (Hochpass statt Slope) + LIR (gelatcht)
        i2cWrite(LSM_ADDR, 0x5B, COLLISION_THS_LSB);  // WAKE_UP_THS
        i2cWrite(LSM_ADDR, 0x5C, 0x00);  // WAKE_UP_DUR: ein Messwert reicht
        i2cWrite(LSM_ADDR, 0x5E, 0x20);  // MD1_CFG: INT1_WU (Pin unbenutzt, Funktion aktiv)
        if (who != 0x69)
            Serial.printf("[IMU] WHO_AM_I=0x%02X — Stoss-Register sind fuer den LSM6DS3 (0x69) gesetzt\n", who);
        gImuOk = true;
        imuDetectVertical();
        Serial.printf("[IMU] OK WHO_AM_I=0x%02X, Stoss ab %.2f g, Hochachse %c\n",
                      who, COLLISION_THS_LSB * 0.25f, "XYZ"[gImuVertical]);
    } else {
        Serial.printf("[IMU] FEHLER WHO_AM_I=0x%02X\n", who);
    }
}

// Warmstart = Reset ohne Stromunterbrechung (Watchdog, Absturz, ESP.restart(),
// USB). Tritt beim ESP32-C6 rev v0.2 auch mitten im Rennen auf (Interrupt-WDT,
// siehe mk_clock_guard.h) — dann zählt jede Sekunde bis zur Fahrbereitschaft.
// Nur echtes Einschalten, Brownout oder Unbekannt gelten als Kaltstart.
static bool isWarmBoot() {
    esp_reset_reason_t r = esp_reset_reason();
    return !(r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_UNKNOWN);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
// ── DFPlayer-Diagnose ─────────────────────────────────────────────────────────
// Klärt, ob ein DFPlayer unsere Befehle überhaupt versteht (neue Nachbau-Chips
// spielen nicht, alte schon). Wartet erst, fragt dann mit Pausen ab. -1 = keine
// Antwort. Spielt danach der Boot-Sound, war es ein Timing-Problem.
static constexpr bool DF_DIAG = false;
// DFPlayer-Lautstärke 0–30. 30 seit 2026-10-04 (vorher 20) — 8-Ω-Lautsprecher
// bekommt am DFPlayer höchstens ~1 W, unkritisch.
static constexpr uint8_t DF_VOLUME = 30;

static void dfDiag() {
    Serial.println("[DFDIAG] warte 2s, sammle spontane Meldungen...");
    uint32_t t0 = nowMs();
    while (nowMs() - t0 < 2000) {
        if (gDf.available())
            Serial.printf("[DFDIAG] spontan: type=%d cmd=0x%02X value=%d\n",
                          gDf.readType(), gDf.readCommand(), gDf.read());
        delay(10);
    }
    gDf.setTimeOut(1000);
    int state   = gDf.readState();              delay(200);
    int vol     = gDf.readVolume();             delay(200);
    int files   = gDf.readFileCounts();         delay(200);
    int folders = gDf.readFolderCounts();       delay(200);
    int inF1    = gDf.readFileCountsInFolder(1); delay(200);
    Serial.printf("[DFDIAG] state=%d volume=%d files=%d folders=%d inFolder1=%d\n",
                  state, vol, files, folders, inF1);
    gDf.volume(20);                              delay(200);
    Serial.printf("[DFDIAG] volume nach volume(20): %d\n", gDf.readVolume());
    delay(200);
}

void setup() {
    Serial.begin(115200);
    // Non-blocking TX: ohne das kann Serial.print() bis zu ~2s blockieren,
    // wenn der USB-Host gerade nicht liest — das würde Motor/Servo/IMU in
    // derselben loop() mit ausbremsen.
    Serial.setTxTimeoutMs(0);
    mkClockGuardBegin();   // SYSTIMER-Fehler des C6 rev v0.2, siehe mk_clock_guard.h
    mkTickWatchBegin();    // Neustart, wenn der FreeRTOS-Tick stehen bleibt
    // Von carShutdown() festgehaltene Pins freigeben (Reset ohne Stromunterbrechung).
    for (uint8_t pin : { PIN_SERVO, PIN_RPWM, PIN_LPWM, PIN_LIGHT_MAIN,
                         PIN_LIGHT_REV, PIN_IR, PIN_LED })
        gpio_hold_dis((gpio_num_t)pin);

    const bool warm = isWarmBoot();

    // DFPlayer UART — start early to catch boot bytes
    gDfSerial.begin(9600, SERIAL_8N1, PIN_DF_RX, PIN_DF_TX);

    // Kaltstart: DFPlayer hochfahren lassen. Warmstart: der läuft noch.
    if (!warm) delay(5000);
    Serial.printf("\n=== MarioKartRC Auto-Firmware (Board V%d) === (%s, reset=%d)\n", KART_REV,
                  warm ? "Warmstart" : "Kaltstart", (int)esp_reset_reason());
#ifdef MK_TEST_NO_DRIVE
    Serial.println("[TEST] Motor/Servo deaktiviert (MK_TEST_NO_DRIVE)");
#endif

    // LEDs — blue while booting
    analogSetAttenuation(ADC_11db);
    gLeds.begin();
    gLeds.setBrightness(60);
    gLeds.fill(gLeds.Color(0, 0, 80));
    gLeds.show();

    // Character ID
    readCharacter();
    Serial.printf("[CHAR] %s (folder %d)\n", gCharName, gCharFolder);

    // Servo
    bool servoOk = ledcAttach(PIN_SERVO, SERVO_FREQ_HZ, SERVO_RES_BITS);
    if (servoOk && warm) {
        servoWriteUs(SERVO_MID_US);
    } else if (servoOk) {
        // Kurzer Selbsttest: links → rechts → Mitte (nur Kaltstart)
        servoWriteUs(SERVO_MIN_US);
        delay(400);
        servoWriteUs(SERVO_MAX_US);
        delay(400);
        servoWriteUs(SERVO_MID_US);
        Serial.println("[SERVO] ok");
    } else {
        Serial.println("[SERVO] FEHLER");
    }

    // Motor
    pinMode(PIN_RPWM, OUTPUT); digitalWrite(PIN_RPWM, LOW);
    pinMode(PIN_LPWM, OUTPUT); digitalWrite(PIN_LPWM, LOW);
    ledcAttach(PIN_RPWM, 10000, 8);
    ledcAttach(PIN_LPWM, 10000, 8);
    motorSet(0);
    ledcAttach(PIN_LIGHT_MAIN, LIGHT_PWM_HZ, LIGHT_PWM_BITS); lightMainWrite(0);
    pinMode(PIN_LIGHT_REV, OUTPUT);  digitalWrite(PIN_LIGHT_REV, LOW);
    if (!warm) {
        // Selbsttest nur beim Kaltstart: Licht an, sanft vor → zurück → aus
        lightMainWrite(255);
        digitalWrite(PIN_LIGHT_REV, HIGH);
        motorSet(25);
        delay(400);
        motorSet(-25);
        delay(400);
        motorSet(0);
        Serial.println("[MOTOR] ok");
        delay(200);
        lightMainWrite(0);
        digitalWrite(PIN_LIGHT_REV, LOW);
    }

    // IR LED
    pinMode(PIN_IR, OUTPUT);
    digitalWrite(PIN_IR, LOW);

    // IMU
#ifdef MK_TEST_NO_I2C
    // Testbuild (env car_noi2c): kein I2C-Verkehr, IMU bleibt aus (gImuOk=false).
    Serial.println("[TEST] I2C deaktiviert (MK_TEST_NO_I2C)");
#else
    imuInit();
#endif

    // EEPROM
    eepromLoad();
    Serial.printf("[EEPROM] ch=%d trim=%d\n", gChannel, gTrim);

    // DFPlayer — Warmstart: nicht zurücksetzen (spart bis zu 2.2 s Warten),
    // keine Diagnose, kein Intro-Sound mitten im Rennen.
    if (warm) {
        gDf.begin(gDfSerial, false, false);
        gDfOk = true;
        gDf.volume(DF_VOLUME);
        Serial.println("[DF] Warmstart, ohne Reset");
    } else if (gDf.begin(gDfSerial, false)) {
        gDfOk = true;
        Serial.println("[DF] ok");  // ohne ACK liefert begin() immer true — sagt nichts
        if (DF_DIAG) dfDiag();
        gDf.volume(DF_VOLUME);
        if (DF_DIAG) delay(200);
        playCharIntro();  // Soundcheck + Kontrolle der DIP-Stellung
        if (DF_DIAG) {
            delay(600);
            Serial.printf("[DFDIAG] state 600ms nach play: %d\n", gDf.readState());
        }
    } else {
        Serial.println("[DF] FEHLER");
    }

    // ESP-NOW
    // Vor dem Funkstart: die Bibliothek setzt beim STA_START-Ereignis den
    // Stromsparmodus selbst auf WiFi.getSleep() — kaeme das nach unserem
    // esp_wifi_set_ps() unten an, waere Modem-Sleep doch wieder an.
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    // Kein Modem-Sleep: mit dem SYSTIMER-Fehler (mk_clock_guard.h) kann der
    // Funk-Stack sonst in einen Watchdog-Absturz laufen.
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(gChannel, WIFI_SECOND_CHAN_NONE);
    WiFi.macAddress(gMyMac);
    Serial.printf("[MAC] " MACSTR "\n", MAC2STR(gMyMac));

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] init FEHLER");
        ESP.restart();
    }
    esp_now_register_recv_cb(onRecv);

    gPairingStartMs = nowMs();

    // Pairing aus dem EEPROM übernehmen, wenn wir auf einem Betriebskanal
    // stehen: die FB spricht uns dort direkt mit ihrer echten MAC an, ganz
    // ohne Beacon/Assign. Kommt binnen PAIR_TIMEOUT_MS nichts, greift unten
    // der Rückfall auf Kanal 1.
    bool macValid = false;
    for (int i = 0; i < 6; i++)
        if (gBaseMac[i] != 0x00 && gBaseMac[i] != 0xFF) macValid = true;
    if (gChannel != MK_ESPNOW_CHANNEL && gChannel <= 13 && macValid) {
        addPeer(gBaseMac);
        gSlot         = 1;
        gPaired       = true;
        gPairState    = PairState::PAIRED;
        gRestored     = true;
        gLastPacketMs = nowMs();
        Serial.printf("[PAIR] Pairing aus EEPROM: ch%d, base=" MACSTR "\n",
                      gChannel, MAC2STR(gBaseMac));
    } else {
        // Start pairing — beacon immediately
        sendBeacon();
        Serial.println("[PAIR] Suche Basis...");
    }

    gLeds.fill(gLeds.Color(80, 40, 0));  // orange = searching
    gLeds.show();

    mkWatchdogBegin();     // Loop-WDT 50 ms + RTC-WDT 1 s, siehe mk_watchdog.h
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    mkDiagTick();
    mkWatchdogFeed();
    uint32_t now = nowMs();

    if (gOtaRequested) otaHandleRequest();   // kehrt nur bei Ablehnung zurück

    // ── Aktuatoren (immer aus Main-Loop, nie aus Callback) ───────────────────
#ifdef MK_TEST_NO_DRIVE
    // Testbuild (env car_nodrive): Verbindung und alle Pakete laufen normal,
    // aber Motor und Servo bleiben aus — fuer Langzeittests der FB mit
    // abgezogenen Sticks, die sonst Zufallswerte als Fahrbefehle schicken.
    if (true) {
#else
    if (gBatCutoff || !gPaired || msSinceLastPacket() > CONTROL_TIMEOUT_MS) {
#endif
        motorSet(0);
        if (servoPreviewActive()) servoWriteUs(gServoPreviewUs);
        else                      servoSet(0);
        digitalWrite(PIN_LIGHT_REV, LOW);
    } else {
        motorSet(gThrottle);
        // Lenkung kalibrieren: solange Previews kommen, steht der Servo dort,
        // wo die FB ihn haben will (die FB sendet währenddessen Gas 0). Pro
        // Runde genau ein Servowert — nie erst normal, dann Vorschau.
        if (servoPreviewActive()) servoWriteUs(gServoPreviewUs);
        else                      servoSet(gSteering);
        digitalWrite(PIN_LIGHT_REV, gThrottle < 0 ? HIGH : LOW);
    }
    if (gServoSavePending) {
        eepromSaveServo(gServoSaveUs);
        gServoSavePending = false;
    }

    // ── Verbindungsverlust ────────────────────────────────────────────────────
    // Gepairt bleiben und warten, die FB meldet sich auf diesem Kanal zurück.
    // Erst nach LINK_LOST_MS (bzw. PAIR_TIMEOUT_MS direkt nach dem Einschalten
    // ohne ein einziges Paket) zurück auf Kanal 1. Auf Kanal 1 selbst nicht,
    // dort schickt die Basis in der Lobby keine Steuerpakete.
    if (gPaired && gChannel != MK_ESPNOW_CHANNEL) {
        uint32_t limit = gRestored ? PAIR_TIMEOUT_MS : LINK_LOST_MS;
        if (msSinceLastPacket() > limit) {
            Serial.printf("[PAIR] %lus nichts von der FB – reset auf Kanal 1\n",
                          (unsigned long)(limit / 1000));
            eepromClear();
            ESP.restart();
        }
    }

    // ── Pairing beacon ────────────────────────────────────────────────────────
    if (!gPaired) {
        if (now - gLastBeaconMs >= BEACON_INTERVAL_MS)
            sendBeacon();

        charPoll(now);

        // Kein Pairing nach 5s auf gespeichertem Kanal → EEPROM löschen, Neustart auf CH1
        if (!gPaired && gChannel != MK_ESPNOW_CHANNEL &&
            now - gPairingStartMs >= PAIR_TIMEOUT_MS) {
            // Gnadenfrist: der ESP-NOW-Callback läuft in einem eigenen Task und
            // kann gPaired/gChannel genau jetzt noch setzen (Assign+ChannelSwitch
            // treffen erfahrungsgemäß nah an der 5s-Marke ein). Kurz warten und
            // erneut prüfen, bevor EEPROM unwiderruflich gelöscht wird — sonst
            // sabotiert der Timeout ein Pairing, das im selben Moment fertig wird.
            MkWdtPause wdtPause;
            delay(100);
            if (!gPaired) {
                Serial.println("[PAIR] Timeout – reset auf Kanal 1");
                eepromClear();
                ESP.restart();
            }
        }
    }

    // ── DFPlayer-Rückmeldungen ausgeben (Diagnose) ───────────────────────────
    if (gDfOk && gDf.available()) {
        uint8_t t = gDf.readType();
        int     v = gDf.read();
        // Track-Ende: Original meldet PlayFinished (0x3D), die verbauten
        // Nachbau-Module stattdessen eine Rückmeldung mit Kommando 0x4C.
        bool finished = (t == DFPlayerPlayFinished) ||
                        (t == DFPlayerFeedBack && gDf.readCommand() == 0x4C);
        if (finished && gStarActive && now - gStarStartMs >= STAR_MIN_MS) {
            gStarActive = false;
            Serial.println("[STAR] Ende (Lied fertig)");
        }
        if (t == DFPlayerError) {
            const char* why = (v == FileMismatch) ? "Datei nicht gefunden (Ordner/Dateiname?)"
                            : (v == FileIndexOut) ? "Track-Index ausserhalb"
                            : (v == Busy)         ? "Modul busy / keine Karte"
                            : (v == Advertise)    ? "Advertise-Fehler"
                            : "unbekannt";
            Serial.printf("[DF] FEHLER %d — %s\n", v, why);
        } else if (t == DFPlayerPlayFinished) {
            Serial.printf("[DF] Track %d fertig\n", v);
        } else {
            Serial.printf("[DF] type=%d cmd=0x%02X value=%d\n", t, gDf.readCommand(), v);
        }
    }

    // ── Button-Log: jede Änderung der Tastenmaske von der FB ─────────────────
    static uint8_t lastLoggedButtons = 0;
    if (gButtons != lastLoggedButtons) {
        if (gButtons) {
            Serial.printf("[BTN] 0x%02X %s%s%s%s\n", gButtons,
                          (gButtons & MK_BTN_YELLOW) ? "YELLOW " : "",
                          (gButtons & MK_BTN_GREEN)  ? "GREEN "  : "",
                          (gButtons & MK_BTN_BLUE)   ? "BLUE "   : "",
                          (gButtons & MK_BTN_RED)    ? "RED "    : "");
        } else {
            Serial.println("[BTN] 0x00 — alle los");
        }
        lastLoggedButtons = gButtons;
    }

    if (gStarActive && now - gStarStartMs >= STAR_MAX_MS) {
        gStarActive = false;
        if (gDfOk) gDf.stop();
        Serial.println("[STAR] Ende (Zeitlimit)");
    }
    lightsUpdate(now);

    // ── Buttons: Blue → Joy-Sound, Red → Licht, Green → Stern ────────────────
    if (gPaired) {
        bool blueNow  = (gButtons     & MK_BTN_BLUE) != 0;
        bool bluePrev = (gPrevButtons & MK_BTN_BLUE) != 0;
        // Während des Sterns kein Joy-Sound — er würde das Stern-Lied abbrechen.
        if (gDfOk && blueNow && !bluePrev && !gStarActive) {
            const CharSounds& s = kSounds[gCharFolder - 1];
            if (s.joyCount > 0) {
                uint8_t track = s.joy[esp_random() % s.joyCount];
                Serial.printf("[SND] playFolder(%d, %d) → /%02d/%03d.mp3\n",
                              gCharFolder, track, gCharFolder, track);
                gDf.playFolder(gCharFolder, track);
            }
        }

        bool redNow  = (gButtons     & MK_BTN_RED) != 0;
        bool redPrev = (gPrevButtons & MK_BTN_RED) != 0;
        if (redNow && !redPrev) {
            gLightsOn = !gLightsOn;   // lightsUpdate() setzt es um
        }

        bool greenNow  = (gButtons     & MK_BTN_GREEN) != 0;
        bool greenPrev = (gPrevButtons & MK_BTN_GREEN) != 0;
        if (greenNow && !greenPrev && !gStarActive) {
            if (gDfOk) playGameSound(SND_STAR);
            gStarActive  = true;
            gStarStartMs = now;
            Serial.println("[STAR] Start");
        }

        gPrevButtons = gButtons;
    }

    // ── Collision check (10Hz) ────────────────────────────────────────────────
    static uint32_t lastImu = 0;
    if (now - lastImu >= 100) {
        checkCollision();
        lastImu = now;
    }

    // ── Feedback to base (5Hz) ────────────────────────────────────────────────
    static uint32_t lastFb = 0;
    if (gPaired && (now - lastFb >= 200)) {
        MK_GameFeedback fb{};
        fb.carBat  = readBattery();
        fb.rumble  = (nowMs() < gRumbleUntilMs) ? 1 : 0;
        fb.trim    = gTrim;
        espnowSend(gBaseMac, &fb, sizeof(fb));
        lastFb = now;
    }

    // ── Battery update (1Hz) ─────────────────────────────────────────────────
    static uint32_t lastBat = 0;
    if (now - lastBat >= 1000) {
        batUpdate();
        lastBat = now;
    }

    // ── LED update (50Hz) ─────────────────────────────────────────────────────
    static uint32_t lastLed = 0;
    if (now - lastLed >= 20) {
        ledUpdate();
        lastLed = now;
    }
}
