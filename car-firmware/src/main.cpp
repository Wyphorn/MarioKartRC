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
#include "mk_protocol.h"
#include "mk_clock_guard.h"

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
// Tracknummer = Dateiname: SND_STAR 1 → 001.mp3, SND_BANANA 2 → 002.mp3, …
// 0 = nicht belegt / noch keine Datei vorhanden.

static constexpr uint8_t FOLDER_GAME  = 9;

static constexpr uint8_t SND_STAR     = 2;   // 001.mp3 – Stern
static constexpr uint8_t SND_BANANA   = 1;   // 002.mp3 – Banane ablegen
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
static constexpr int PIN_ADC_BAT  = 4;
static constexpr int PIN_DF_TX    = 18;  // ESP32 TX → DFPlayer RX
static constexpr int PIN_RPWM     = 5;
static constexpr int PIN_LPWM     = 6;
static constexpr int PIN_DF_RX    = 19;  // ESP32 RX ← DFPlayer TX
static constexpr int PIN_SCL      = 0;
static constexpr int PIN_SDA      = 1;
static constexpr int PIN_LED      = 20;
static constexpr int PIN_IR       = 3;
static constexpr int PIN_SERVO    = 7;
static constexpr int PIN_LIGHT_MAIN = 15;  // Front weiß + Heck rot (ein NPN)
static constexpr int PIN_LIGHT_REV  = 14;  // Heck weiß, Rückwärtsgang

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr int LED_COUNT    = 9;
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
    ledcWrite(PIN_SERVO, (uint32_t)us * ((1u << SERVO_RES_BITS) - 1) / SERVO_PERIOD_US);
}

// ── Battery thresholds (2S LiPo / 2× 18650) ──────────────────────────────────
// Spannungsteiler: R1 (VBat→ADC) + R2 (ADC→GND) — Werte anpassen wenn verdrahtet!
static constexpr float BAT_R1          = 100000.0f;
static constexpr float BAT_R2          =  47000.0f;
static constexpr float BAT_DIVIDER_INV = (BAT_R1 + BAT_R2) / BAT_R2;
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
    if (EEPROM.read(EE_ADDR_MAGIC) != EE_MAGIC) return;
    gChannel = EEPROM.read(EE_ADDR_CHANNEL);
    gTrim    = (int8_t)EEPROM.read(EE_ADDR_TRIM);
    for (int i = 0; i < 6; i++) gBaseMac[i] = EEPROM.read(EE_ADDR_BASE_MAC + i);
}

static void eepromSaveChannel(uint8_t ch) {
    gChannel = ch;
    EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.write(EE_ADDR_CHANNEL, ch);
    EEPROM.commit();
}

static void eepromSaveTrim(int8_t trim) {
    gTrim = trim;
    EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.write(EE_ADDR_TRIM, (uint8_t)trim);
    EEPROM.commit();
}

static void eepromSaveBaseMac(const uint8_t* mac) {
    memcpy(gBaseMac, mac, 6);
    EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC);
    for (int i = 0; i < 6; i++) EEPROM.write(EE_ADDR_BASE_MAC + i, mac[i]);
    EEPROM.commit();
}

// Nur das Pairing vergessen (Kanal → 1, FB-MAC → leer). Magic und Trim bleiben
// stehen — früher wurde hier das Magic-Byte gelöscht, eepromLoad() verwarf
// danach den ganzen Block und der Servo-Trim war bei jedem Rückfall auf
// Kanal 1 weg. Der Trim gehört zur Mechanik des Autos, nicht zum Pairing.
static void eepromClear() {
    EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.write(EE_ADDR_CHANNEL, MK_ESPNOW_CHANNEL);
    for (int i = 0; i < 6; i++) EEPROM.write(EE_ADDR_BASE_MAC + i, 0x00);
    EEPROM.commit();
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
    // map -100..100 to SERVO_MIN_US..SERVO_MAX_US, then apply trim
    int us = map(steering, -100, 100, SERVO_MIN_US, SERVO_MAX_US);
    // trim: ±10 steps → ±50µs
    us += gTrim * 5;
    us = constrain(us, SERVO_MIN_US, SERVO_MAX_US);
    servoWriteUs(us);
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
    gBatCutoff = true;
    Serial.printf("[BAT] %.2fV < %.2fV — Abschaltung (Deep Sleep)\n", gBatVoltage, BAT_OFF_V);
    motorSet(0);
    servoWriteUs(SERVO_MID_US);
    digitalWrite(PIN_LIGHT_MAIN, LOW);
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
    gBatVoltage = vAdc * BAT_DIVIDER_INV;
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
//   1–8  — Charakter-Kreis (8 LEDs im Charakterkopf)

static constexpr int LED_STATUS     = 0;
static constexpr int LED_CHAR_FIRST = 1;
static constexpr int LED_CHAR_LAST  = 8;

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
        gLeds.setPixelColor(i, on ? wheel((hue + (i - LED_CHAR_FIRST) * 256 / 8) & 0xFF) : 0);
    hue += 3;
}

static void ledUpdate() {
    uint32_t now = nowMs();

    // ── LEDs 1–8: Charakter-Kreis ────────────────────────────────────────────
    if (gMappingSlot >= 1 && gMappingSlot <= 8) {
        const uint8_t* c = MK_MAPPING_COLORS[gMappingSlot - 1];
        uint32_t col = gLeds.Color(c[0], c[1], c[2]);
        for (int i = LED_CHAR_FIRST; i <= LED_CHAR_LAST; i++)
            gLeds.setPixelColor(i, col);
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
        default:
            break;
    }
}

// ── IMU init ──────────────────────────────────────────────────────────────────
static void imuInit() {
    Wire.begin(PIN_SDA, PIN_SCL);
    uint8_t who = i2cRead(LSM_ADDR, 0x0F);
    if (who == 0x6C || who == 0x69) {
        i2cWrite(LSM_ADDR, 0x10, 0x44);  // accel 104Hz 16g
        i2cWrite(LSM_ADDR, 0x11, 0x40);  // gyro  104Hz 250dps
        gImuOk = true;
        Serial.printf("[IMU] OK WHO_AM_I=0x%02X\n", who);
    } else {
        Serial.printf("[IMU] FEHLER WHO_AM_I=0x%02X\n", who);
    }
}

// ── Collision detection ───────────────────────────────────────────────────────
// Simple magnitude threshold — tune COLLISION_G as needed
static constexpr float COLLISION_G = 1.5f;                   // TEST: empfindlich — nach Fahrtest wieder auf ~6g
static constexpr float LSM_ACCEL_SCALE = 16.0f / 32768.0f;  // 16g range
static constexpr uint32_t COLLISION_COOLDOWN_MS = 3000;      // kein Re-Trigger innerhalb 3s

static void checkCollision() {
    if (!gImuOk) return;

    Wire.beginTransmission(LSM_ADDR);
    Wire.write(0x28);  // OUTX_L_A (accel X low)
    Wire.endTransmission(false);
    Wire.requestFrom(LSM_ADDR, (uint8_t)6);
    if (Wire.available() < 6) return;

    int16_t ax = Wire.read() | (Wire.read() << 8);
    int16_t ay = Wire.read() | (Wire.read() << 8);
    int16_t az = Wire.read() | (Wire.read() << 8);

    float gx = ax * LSM_ACCEL_SCALE;
    float gy = ay * LSM_ACCEL_SCALE;
    float gz = az * LSM_ACCEL_SCALE;
    float mag = sqrtf(gx * gx + gy * gy + gz * gz);

    uint32_t now = nowMs();
    if (mag > COLLISION_G && (now - gLastCollisionMs > COLLISION_COOLDOWN_MS)) {
        Serial.printf("[IMU] KOLLISION! %.2fg → Rumble 2s\n", mag);
        gLastCollisionMs = now;
        gRumbleUntilMs   = now + 2000;
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
// ── DFPlayer-Diagnose ─────────────────────────────────────────────────────────
// Klärt, ob ein DFPlayer unsere Befehle überhaupt versteht (neue Nachbau-Chips
// spielen nicht, alte schon). Wartet erst, fragt dann mit Pausen ab. -1 = keine
// Antwort. Spielt danach der Boot-Sound, war es ein Timing-Problem.
static constexpr bool DF_DIAG = true;

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
    // Von carShutdown() festgehaltene Pins freigeben (Reset ohne Stromunterbrechung).
    for (uint8_t pin : { PIN_SERVO, PIN_RPWM, PIN_LPWM, PIN_LIGHT_MAIN,
                         PIN_LIGHT_REV, PIN_IR, PIN_LED })
        gpio_hold_dis((gpio_num_t)pin);

    // DFPlayer UART — start early to catch boot bytes
    gDfSerial.begin(9600, SERIAL_8N1, PIN_DF_RX, PIN_DF_TX);

    delay(5000);
    Serial.println("\n=== MarioKartRC Auto-Firmware ===");

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
    if (ledcAttach(PIN_SERVO, SERVO_FREQ_HZ, SERVO_RES_BITS)) {
        // Kurzer Selbsttest: links → rechts → Mitte
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
    // Beleuchtung — beim Motortest 1s an als Lichttest
    pinMode(PIN_LIGHT_MAIN, OUTPUT); digitalWrite(PIN_LIGHT_MAIN, HIGH);
    pinMode(PIN_LIGHT_REV, OUTPUT);  digitalWrite(PIN_LIGHT_REV, HIGH);

    // Kurzer Selbsttest: sanft vor → zurück → aus
    motorSet(25);
    delay(400);
    motorSet(-25);
    delay(400);
    motorSet(0);
    Serial.println("[MOTOR] ok");
    delay(200);
    digitalWrite(PIN_LIGHT_MAIN, LOW);
    digitalWrite(PIN_LIGHT_REV, LOW);

    // IR LED
    pinMode(PIN_IR, OUTPUT);
    digitalWrite(PIN_IR, LOW);

    // IMU
    imuInit();

    // EEPROM
    eepromLoad();
    Serial.printf("[EEPROM] ch=%d trim=%d\n", gChannel, gTrim);

    // DFPlayer
    if (gDf.begin(gDfSerial, false)) {
        gDfOk = true;
        Serial.println("[DF] ok");  // ohne ACK liefert begin() immer true — sagt nichts
        if (DF_DIAG) dfDiag();
        gDf.volume(20);
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
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = nowMs();

    // ── Aktuatoren (immer aus Main-Loop, nie aus Callback) ───────────────────
    if (gBatCutoff || !gPaired || msSinceLastPacket() > CONTROL_TIMEOUT_MS) {
        motorSet(0);
        servoSet(0);
        digitalWrite(PIN_LIGHT_REV, LOW);
    } else {
        motorSet(gThrottle);
        servoSet(gSteering);
        digitalWrite(PIN_LIGHT_REV, gThrottle < 0 ? HIGH : LOW);
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

    // ── Button: Blue → zufälliger Joy-Sound, Red → Beleuchtung an/aus ────────
    if (gPaired) {
        bool blueNow  = (gButtons     & MK_BTN_BLUE) != 0;
        bool bluePrev = (gPrevButtons & MK_BTN_BLUE) != 0;
        if (gDfOk && blueNow && !bluePrev) {
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
            gLightsOn = !gLightsOn;
            digitalWrite(PIN_LIGHT_MAIN, gLightsOn ? HIGH : LOW);
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
