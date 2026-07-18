#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include <DFRobotDFPlayerMini.h>
#include <ESP32Servo.h>
#include "mk_protocol.h"

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

// Yoshi (folder 02)
static const uint8_t kJoy_Yoshi[] = {1,2,4};
static const uint8_t kSad_Yoshi[] = {3,5};

// Bowser (folder 03)
static const uint8_t kJoy_Bowser[] = {1,4};
static const uint8_t kSad_Bowser[] = {2,3,5};

// DK (folder 04)
static const uint8_t kJoy_DK[] = {1,3,5};
static const uint8_t kSad_DK[] = {2,4};

// Luigi (folder 05)
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
    SOUNDS(Yoshi),
    SOUNDS(Bowser),
    SOUNDS(DK),
    SOUNDS(Luigi),
    SOUNDS(Peach),
    SOUNDS(Toad),
    SOUNDS(Rosalina),
};
#undef SOUNDS

// ── Pins ─────────────────────────────────────────────────────────────────────
static constexpr int PIN_ADC_CHAR = 2;
static constexpr int PIN_ADC_BAT  = 0;
static constexpr int PIN_DF_TX    = 15;  // ESP32 TX → DFPlayer RX
static constexpr int PIN_RPWM     = 3;
static constexpr int PIN_LPWM     = 4;
static constexpr int PIN_DF_RX    = 14;  // ESP32 RX ← DFPlayer TX
static constexpr int PIN_SCL      = 6;
static constexpr int PIN_SDA      = 7;
static constexpr int PIN_LED      = 1;
static constexpr int PIN_IR       = 19;
static constexpr int PIN_SERVO    = 5;

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr int LED_COUNT    = 9;
static constexpr uint8_t LSM_ADDR = 0x6A;

// Servo limits with 5° mechanical buffer (1000–2000 → 1050–1950)
static constexpr int SERVO_MIN_US = 1050;
static constexpr int SERVO_MAX_US = 1950;
static constexpr int SERVO_MID_US = 1500;

// ── Battery thresholds (2S LiPo / 2× 18650) ──────────────────────────────────
// Spannungsteiler: R1 (VBat→ADC) + R2 (ADC→GND) — Werte anpassen wenn verdrahtet!
static constexpr float BAT_R1          = 100000.0f;
static constexpr float BAT_R2          =  47000.0f;
static constexpr float BAT_DIVIDER_INV = (BAT_R1 + BAT_R2) / BAT_R2;
static constexpr float BAT_WARN_V      = 6.6f;  // 3.3V/Zelle
static constexpr float BAT_CUTOFF_V    = 6.0f;  // 3.0V/Zelle — latchend

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
static Servo               gServo;

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
static uint32_t gLastPacketMs = 0;
static constexpr uint32_t CONTROL_TIMEOUT_MS = 200;

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
static bool    gBatWired   = false;
static float   gBatVoltage = 8.4f;
static uint8_t gBatLevel   = 5;     // 0–5; 0=kritisch/Cutoff, 5=voll
static bool    gBatCutoff  = false; // latchend — nur Power-Cycle löst

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

static void eepromClear() {
    EEPROM.write(EE_ADDR_MAGIC, 0x00);
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
    gServo.writeMicroseconds(us);
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
static void readCharacter() {
    int raw = analogRead(PIN_ADC_CHAR);
    if (raw > 3940) {
        gCharName = "Rosalina"; gCharFolder = 8;
        return;
    }
    float v = raw * 2.985f / 4095.0f;
    if      (v < 0.708f) { gCharName = "Mario";   gCharFolder = 1; }
    else if (v < 1.070f) { gCharName = "Luigi";   gCharFolder = 2; }
    else if (v < 1.485f) { gCharName = "Yoshi";   gCharFolder = 3; }
    else if (v < 1.798f) { gCharName = "Bowser";  gCharFolder = 4; }
    else if (v < 2.073f) { gCharName = "DK";      gCharFolder = 5; }
    else if (v < 2.480f) { gCharName = "Peach";   gCharFolder = 6; }
    else                 { gCharName = "Toad";    gCharFolder = 7; }
}

// ── Battery ───────────────────────────────────────────────────────────────────
static void batUpdate() {
    if (!gBatWired) return;
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += analogRead(PIN_ADC_BAT);
    float vAdc = (sum / 4) * 3.3f / 4095.0f;
    gBatVoltage = vAdc * BAT_DIVIDER_INV;
    if (gBatVoltage < BAT_CUTOFF_V) {
        gBatLevel = 0;
        if (!gBatCutoff) {
            Serial.printf("[BAT] CUTOFF %.2fV — Motor gesperrt\n", gBatVoltage);
            if (gDfOk && gCharFolder >= 1 && gCharFolder <= 8) {
                const CharSounds& s = kSounds[gCharFolder - 1];
                if (s.sadCount > 0) gDf.playFolder(gCharFolder, s.sad[esp_random() % s.sadCount]);
            }
            gBatCutoff = true;
        }
        return;
    }
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
//   0–7  — Charakter-Kreis (8 LEDs im Charakterkopf)
//   8    — Status-LED

static constexpr int LED_CHAR_FIRST = 0;
static constexpr int LED_CHAR_LAST  = 7;
static constexpr int LED_STATUS     = 8;

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
    uint32_t now = millis();
    bool on = (now % 300) >= 50;
    for (int i = LED_CHAR_FIRST; i <= LED_CHAR_LAST; i++)
        gLeds.setPixelColor(i, on ? wheel((hue + i * 256 / 8) & 0xFF) : 0);
    hue += 3;
}

static void ledUpdate() {
    uint32_t now = millis();

    // ── LEDs 0–7: Charakter-Kreis ────────────────────────────────────────────
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

    // ── LED 8: Status ─────────────────────────────────────────────────────────
    uint8_t bat = gBatWired ? readBattery() : 5;  // 0–5, 5=voll
    if (gBatWired && bat == 0) {
        // Akku kritisch (<16%): rot blinkend
        uint32_t col = ((now / 300) % 2) ? gLeds.Color(200, 0, 0) : 0;
        gLeds.setPixelColor(LED_STATUS, col);
    } else if (gBatWired && bat <= 1) {
        // Akku niedrig (<33%): gelb
        gLeds.setPixelColor(LED_STATUS, gLeds.Color(180, 80, 0));
    } else if (gPaired) {
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
    gLastBeaconMs = millis();
    Serial.printf("[PAIR] Beacon → ch%d %s\n", gChannel, gCharName);
}

static void onPaired(const MK_Assign* pkt, const uint8_t* senderMac) {
    gSlot = pkt->slot;
    eepromSaveBaseMac(pkt->baseMac);
    addPeer(pkt->baseMac);

    gPaired    = true;
    gPairState = PairState::PAIRED;
    Serial.printf("[PAIR] Slot %d, base=" MACSTR "\n", gSlot, MAC2STR(pkt->baseMac));

    // Send initial feedback so base knows we're alive
    MK_GameFeedback fb{};
    fb.carBat = readBattery();
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
            gLastPacketMs = millis();
            break;
        }
        case MSG_CONFIG: {
            if (!gPaired || len < (int)sizeof(MK_ConfigPacket)) return;
            const auto* pkt = (const MK_ConfigPacket*)data;
            eepromSaveTrim(pkt->trim);
            break;
        }
        case MSG_CHANNEL_SWITCH: {
            if (len < (int)sizeof(MK_ChannelSwitch)) return;
            const auto* pkt = (const MK_ChannelSwitch*)data;
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

    uint32_t now = millis();
    if (mag > COLLISION_G && (now - gLastCollisionMs > COLLISION_COOLDOWN_MS)) {
        Serial.printf("[IMU] KOLLISION! %.2fg → Rumble 2s\n", mag);
        gLastCollisionMs = now;
        gRumbleUntilMs   = now + 2000;
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // DFPlayer UART — start early to catch boot bytes
    gDfSerial.begin(9600, SERIAL_8N1, PIN_DF_RX, PIN_DF_TX);

    delay(1000);
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

    // Servo — vor dem Motor attachen, damit die ESP32Servo-eigene Kanalverwaltung
    // Kanal 0 bekommt, bevor die rohen ledcAttach()-Aufrufe unten ihn belegen.
    // Hinweis: Servo::attach() gibt die Kanalnummer zurück (0 = Erfolg auf Kanal 0,
    // aber falsy in C++!) — darum hier über attached() prüfen, nicht den Rückgabewert.
    gServo.attach(PIN_SERVO, 1000, 2000);
    if (gServo.attached()) {
        gServo.writeMicroseconds(SERVO_MID_US);
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
    Serial.println("[MOTOR] ok");

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
        gDf.volume(20);
        gDfOk = true;
        Serial.println("[DF] ok");
    } else {
        Serial.println("[DF] FEHLER");
    }

    // ESP-NOW
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(gChannel, WIFI_SECOND_CHAN_NONE);
    WiFi.macAddress(gMyMac);
    Serial.printf("[MAC] " MACSTR "\n", MAC2STR(gMyMac));

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] init FEHLER");
        ESP.restart();
    }
    esp_now_register_recv_cb(onRecv);

    // Start pairing — beacon immediately
    gPairingStartMs = millis();
    sendBeacon();

    gLeds.fill(gLeds.Color(80, 40, 0));  // orange = searching
    gLeds.show();
    Serial.println("[PAIR] Suche Basis...");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // ── Aktuatoren (immer aus Main-Loop, nie aus Callback) ───────────────────
    if (gBatCutoff || !gPaired || (now - gLastPacketMs > CONTROL_TIMEOUT_MS)) {
        motorSet(0);
        servoSet(0);
    } else {
        motorSet(gThrottle);
        servoSet(gSteering);
    }

    // ── Pairing beacon ────────────────────────────────────────────────────────
    if (!gPaired) {
        if (now - gLastBeaconMs >= BEACON_INTERVAL_MS)
            sendBeacon();

        // Kein Pairing nach 5s auf gespeichertem Kanal → EEPROM löschen, Neustart auf CH1
        if (gChannel != MK_ESPNOW_CHANNEL &&
            now - gPairingStartMs >= PAIR_TIMEOUT_MS) {
            Serial.println("[PAIR] Timeout – reset auf Kanal 1");
            eepromClear();
            ESP.restart();
        }
    }

    // ── Button: Blue → zufälliger Joy-Sound ──────────────────────────────────
    if (gDfOk && gPaired) {
        bool blueNow  = (gButtons     & MK_BTN_BLUE) != 0;
        bool bluePrev = (gPrevButtons & MK_BTN_BLUE) != 0;
        if (blueNow && !bluePrev) {
            const CharSounds& s = kSounds[gCharFolder - 1];
            if (s.joyCount > 0) {
                uint8_t track = s.joy[esp_random() % s.joyCount];
                gDf.playFolder(gCharFolder, track);
            }
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
        fb.rumble  = (millis() < gRumbleUntilMs) ? 1 : 0;
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
