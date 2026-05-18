#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <ESP8266WiFi.h>
extern "C" {
    #include <espnow.h>
    #include <user_interface.h>
}
#include "mk_protocol.h"

// --- Pins ---
#define PIN_RUMBLE  D8
// Zuordnung Farbe → Pin: TBD beim PCB-Layout (Leiterbahnen Button-Board)
#define PIN_BTN_YELLOW  D5
#define PIN_BTN_GREEN   D6
#define PIN_BTN_BLUE    D7
#define PIN_BTN_RED     D3
#define PIN_LED     D4
#define PIN_MODE    D0

// --- ADS1115 Kanäle ---
#define JS_LEFT_X   0
#define JS_LEFT_Y   1
#define JS_RIGHT_X  2
#define JS_RIGHT_Y  3

// --- Display ---
#define OLED_WIDTH  128
#define OLED_HEIGHT 64

// --- Rumble ---
#define RUMBLE_INTERVAL_MS  10000
#define RUMBLE_DURATION_MS  1000
#define RUMBLE_PWM          255

// --- Timings ---
#define LOOP_PERIOD_MS    20     // 50 Hz Zielfrequenz
#define DISPLAY_PERIOD_MS 500   // Mindestabstand zwischen Display-Updates
#define CAL_HOLD_MS   3000
#define CAL_PULSE_MS  3000
#define CAL_SHOW_MS   3000

// --- Joystick ---
// Spannungsteiler: R_top = R_bot (beliebiger gleicher Wert 1k–100k)
// → V_out = 5V × 0.5 = 2.5V, ADS-Wert = 2.5/4.096 × 32767 ≈ 19989
// Minimum: Joystick bei GND → 0V → ADS ≈ 0
// Default-Center: halber Bereich (überschrieben durch Offset-Kalibrierung beim Boot)
#define JS_DEFAULT_MIN     0
#define JS_DEFAULT_MAX     19989
#define JS_DEFAULT_CENTER  9994      // JS_DEFAULT_MAX / 2
#define JS_MENU_THRESHOLD  4500      // ~45% Auslenkung ab Center für Menünavigation
#define JS_MENU_HIGH       (JS_DEFAULT_CENTER + JS_MENU_THRESHOLD)   // ≈ 14494
#define JS_MENU_LOW        (JS_DEFAULT_CENTER - JS_MENU_THRESHOLD)   // ≈ 5494
#define JS_DEADZONE        6
#define MENU_COOLDOWN      300
#define ADC_OVERSAMPLE     4    // Samples pro Kanal im Normalbetrieb (STATE_READY)

// --- EEPROM ---
#define EEPROM_MAGIC        0xCAFE
#define EEPROM_ADDR_MAGIC   0
#define EEPROM_ADDR_CENTER  2   // 4x int16 = 8B
#define EEPROM_ADDR_MIN    10   // 4x int16 = 8B
#define EEPROM_ADDR_MAX    18   // 4x int16 = 8B
#define EEPROM_ADDR_TRIM    26   // int8
#define EEPROM_ADDR_LANG    27   // uint8
#define EEPROM_ADDR_CHANNEL 28   // uint8, 0 = kein gespeicherter Kanal
#define EEPROM_SIZE         32

// --- Trim & Speed ---
#define TRIM_STEPS  21
#define TRIM_MIN   -10
#define TRIM_MAX    10
#define SPEED_STEPS  10

// --- Akku ---
// Teiler: 100kΩ extern + 220kΩ/100kΩ intern → Vbat = analogRead/1023 * 4.2
#define BAT_LOW_PCT   33
#define BAT_CRIT_PCT  16
#define BAT_CHECK_MS  10000

// --- Menü ---
#define MENU_ITEM_COUNT 8

// --- Sprachen ---
// Um eine weitere Sprache hinzuzufügen:
// 1. LANG_COUNT erhöhen
// 2. Neuen Eintrag in STRINGS[] ergänzen
#define LANG_COUNT  2
#define LANG_DE     0
#define LANG_EN     1

// --- Min/Max Kalibrierungsschritte ---
// 8 Schritte: LX-min, LX-max, LY-min, LY-max, RX-min, RX-max, RY-min, RY-max
const uint8_t CAL_STEP_CH[8]    = {0, 0, 1, 1, 2, 2, 3, 3};
const bool    CAL_STEP_ISMAX[8] = {false, true, false, true, false, true, false, true};

struct CalStep { const char* line1; const char* line2; };

struct Strings {
    const char* menuTitle;
    const char* menuItems[MENU_ITEM_COUNT];
    const char* calOffTitle;
    const char* calOffRelease;
    const char* calOffDoing;
    const char* calMMTitle;
    CalStep     calMMSteps[8];
    const char* calMMConfirm;
    const char* calMMStep;      // "Schritt" / "Step"
    const char* trimTitle;
    const char* speedTitle;
    const char* langTitle;
    const char* langNames[LANG_COUNT];
    const char* resetTitle;
    const char* resetDone;
    const char* rumbleOn;
    const char* rumbleOff;
    const char* swapOn;
    const char* swapOff;
    const char* connectingDirect;   // "Warte auf Auto"
    const char* connectingGame;     // "Verbinde mit Basis"
    const char* rejoiningLine1;     // Zeile 1: "Verbinde mit"
    const char* rejoiningLine2;     // Zeile 2: "letztem Spiel"
};

const Strings STRINGS[LANG_COUNT] = {
    // LANG_DE
    {
        "Einstellungen",
        { "Offset-Kalibrierung", "Min/Max-Kalibrierung",
          "Servo-Trim", "Max. Speed", "Sprache", "Rumble",
          "Joysticks tauschen", "Reset" },
        "Offset-Kalibrierung",
        "Sticks loslassen",
        "Kalibriere...",
        "Min/Max-Kalibrierung",
        {
            { "Linken Stick",  "nach links druecken"  },
            { "Linken Stick",  "nach rechts druecken" },
            { "Linken Stick",  "nach oben druecken"   },
            { "Linken Stick",  "nach unten druecken"  },
            { "Rechten Stick", "nach links druecken"  },
            { "Rechten Stick", "nach rechts druecken" },
            { "Rechten Stick", "nach oben druecken"   },
            { "Rechten Stick", "nach unten druecken"  },
        },
        "Gelb = bestaetigen",
        "Schritt",
        "Servo-Trim",
        "Max. Speed",
        "Sprache",
        { "Deutsch", "English" },
        "Reset",
        "Werte zurueckgesetzt",
        "An", "Aus",
        "An", "Aus",
        "Warte auf Auto", "Verbinde mit Basis",
        "Verbinde mit", "letztem Spiel"
    },
    // LANG_EN
    {
        "Settings",
        { "Offset Calibration", "Min/Max Calibration",
          "Servo Trim", "Max. Speed", "Language", "Rumble",
          "Swap Sticks", "Reset" },
        "Offset Calibration",
        "Release sticks",
        "Calibrating...",
        "Min/Max Calibration",
        {
            { "Left stick",  "push left"  },
            { "Left stick",  "push right" },
            { "Left stick",  "push up"    },
            { "Left stick",  "push down"  },
            { "Right stick", "push left"  },
            { "Right stick", "push right" },
            { "Right stick", "push up"    },
            { "Right stick", "push down"  },
        },
        "Yellow = confirm",
        "Step",
        "Servo Trim",
        "Max. Speed",
        "Language",
        { "Deutsch", "English" },
        "Reset",
        "Values reset",
        "On", "Off",
        "On", "Off",
        "Waiting for car", "Connecting to base",
        "Reconnecting", "to last game"
    }
};

// --- Betriebs-Modus ---
enum FBMode { MODE_DIRECT, MODE_GAME };

// --- States ---
enum FBState {
    STATE_READY,
    STATE_MENU,
    STATE_CAL_OFFSET_PULSE,
    STATE_CAL_OFFSET,
    STATE_CAL_MINMAX,
    STATE_TRIM,
    STATE_SPEED,
    STATE_LANGUAGE,
    STATE_RESET,
};

// --- Globale Variablen ---
Adafruit_ADS1115  ads;
Adafruit_SSD1306  display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Adafruit_NeoPixel led(1, PIN_LED, NEO_GRB + NEO_KHZ800);

bool    adsOK   = false;
bool    dispOK  = false;
uint8_t batPct  = 100;
FBMode  fbMode       = MODE_DIRECT;
bool    rumbleEnabled = true;

FBState  state        = STATE_READY;
uint8_t  langIndex    = LANG_DE;
int8_t   servoTrim    = 0;
uint8_t  maxSpeed     = SPEED_STEPS;   // temporär, nicht im EEPROM

int16_t  jsCenter[4]  = {JS_DEFAULT_CENTER, JS_DEFAULT_CENTER, JS_DEFAULT_CENTER, JS_DEFAULT_CENTER};
int16_t  jsMin[4]     = {JS_DEFAULT_MIN, JS_DEFAULT_MIN, JS_DEFAULT_MIN, JS_DEFAULT_MIN};
int16_t  jsMax[4]     = {JS_DEFAULT_MAX, JS_DEFAULT_MAX, JS_DEFAULT_MAX, JS_DEFAULT_MAX};

unsigned long stateStart   = 0;
unsigned long calHoldStart = 0;
unsigned long lastMenuMove = 0;
unsigned long lastRumble   = 0;
unsigned long lastBatCheck = 0;

bool    rumbleActive  = false;
bool    swapSticks    = false;
bool    calDone       = false;
int8_t  menuSel       = 0;
uint8_t calStep       = 0;

int8_t  trimTemp      = 0;
uint8_t speedTemp     = SPEED_STEPS;
uint8_t langTemp      = LANG_DE;

int16_t jsMinTemp[4];
int16_t jsMaxTemp[4];

// ──────────────────────────────────────────────
// ESP-NOW Globals
// ──────────────────────────────────────────────
#define BEACON_INTERVAL_MS        500
#define FEEDBACK_TIMEOUT_MS      2000
#define SAVED_CHANNEL_TIMEOUT_MS 5000

uint8_t  realMac[6];
uint8_t  peerMac[6];
bool     paired          = false;
unsigned long lastBeacon = 0;

volatile bool pairingBeaconRx = false;
volatile bool pairingAssignRx = false;
uint8_t pairingCarMac[6];
uint8_t pairingBaseMac[6];

bool             trySavedChannel    = false;
unsigned long    savedChannelStart  = 0;
volatile uint8_t pendingChannelSave = 0;

struct FeedbackState {
    uint8_t position   = 0;
    uint8_t lap        = 0;
    uint8_t lapTotal   = 0;
    uint8_t item       = 0;
    uint8_t speedLimit = 10;
    uint8_t rumble     = 0;
    uint8_t carBat     = 0;
};
volatile FeedbackState feedbackRaw;
volatile bool          feedbackNew    = false;
FeedbackState          feedback;
unsigned long          lastFeedbackMs = 0;
unsigned long          rumbleFbEnd    = 0;

const Strings& S() { return STRINGS[langIndex]; }

// ──────────────────────────────────────────────
// LED
// ──────────────────────────────────────────────
void setLed(uint8_t r, uint8_t g, uint8_t b) {
    led.setPixelColor(0, led.Color(r, g, b));
    led.show();
}
void ledGreen()  { setLed(0, 60, 0); }
void ledYellow() { setLed(60, 60, 0); }
void ledPulseYellow() {
    uint32_t t = (millis() - stateStart) % 600;
    uint8_t  v = (t < 300) ? (t * 60 / 300) : ((600 - t) * 60 / 300);
    setLed(v, v, 0);
}
void ledRedBlink()    { uint8_t v = ((millis() / 500) % 2) ? 60 : 0; setLed(v, 0, 0); }
void ledOrange()      { setLed(60, 20, 0); }
void ledPink()        { setLed(60, 0, 40); }
void ledBlue()        { setLed(0, 0, 60); }
void ledPulseOrange() { uint8_t v = ((millis() / 300) % 2) ? 60 : 0; setLed(v, v/3, 0); }
void ledPulseBlue()   { uint8_t v = ((millis() / 300) % 2) ? 60 : 0; setLed(0, 0, v); }
void ledReady() {
    if      (batPct <= BAT_CRIT_PCT) ledRedBlink();
    else if (batPct <= BAT_LOW_PCT)  ledOrange();
    else                             ledGreen();
}

// ──────────────────────────────────────────────
// EEPROM
// ──────────────────────────────────────────────
void saveSettings() {
    EEPROM.put(EEPROM_ADDR_MAGIC,  (uint16_t)EEPROM_MAGIC);
    EEPROM.put(EEPROM_ADDR_CENTER, jsCenter);
    EEPROM.put(EEPROM_ADDR_MIN,    jsMin);
    EEPROM.put(EEPROM_ADDR_MAX,    jsMax);
    EEPROM.put(EEPROM_ADDR_TRIM,   servoTrim);
    EEPROM.put(EEPROM_ADDR_LANG,   langIndex);
    EEPROM.commit();
}

bool loadSettings() {
    uint16_t magic;
    EEPROM.get(EEPROM_ADDR_MAGIC, magic);
    if (magic != EEPROM_MAGIC) return false;
    EEPROM.get(EEPROM_ADDR_CENTER, jsCenter);
    EEPROM.get(EEPROM_ADDR_MIN,    jsMin);
    EEPROM.get(EEPROM_ADDR_MAX,    jsMax);
    EEPROM.get(EEPROM_ADDR_TRIM,   servoTrim);
    EEPROM.get(EEPROM_ADDR_LANG,   langIndex);
    Serial.printf("[EEPROM] ctr:%d %d %d %d  min:%d %d %d %d  max:%d %d %d %d  trim:%d  lang:%d\n",
        jsCenter[0], jsCenter[1], jsCenter[2], jsCenter[3],
        jsMin[0], jsMin[1], jsMin[2], jsMin[3],
        jsMax[0], jsMax[1], jsMax[2], jsMax[3],
        servoTrim, langIndex);
    return true;
}

// ──────────────────────────────────────────────
// Kalibrierung
// ──────────────────────────────────────────────
void calibrateOffset() {
    if (!adsOK) return;
    int32_t sum[4] = {};
    for (int i = 0; i < 10; i++) {
        for (int ch = 0; ch < 4; ch++)
            sum[ch] += ads.readADC_SingleEnded(ch);
        delay(20);
    }
    for (int ch = 0; ch < 4; ch++)
        jsCenter[ch] = sum[ch] / 10;
    saveSettings();
    Serial.printf("[CAL-OFF] %d %d %d %d\n",
        jsCenter[0], jsCenter[1], jsCenter[2], jsCenter[3]);
}

void updateBattery() {
    int32_t sum = 0;
    for (int i = 0; i < 4; i++) sum += analogRead(A0);
    float vbat = (sum / 4.0f) / 1023.0f * 4.2f;
    batPct = (uint8_t)constrain((int)((vbat - 3.0f) / 1.2f * 100.0f), 0, 100);
    Serial.printf("[BAT] %.2fV %d%%\n", vbat, batPct);
}

void resetSettings() {
    for (int i = 0; i < 4; i++) {
        jsMin[i]    = JS_DEFAULT_MIN;
        jsMax[i]    = JS_DEFAULT_MAX;
        jsCenter[i] = JS_DEFAULT_CENTER;
    }
    servoTrim = 0;
    uint8_t zero = 0;
    EEPROM.put(EEPROM_ADDR_CHANNEL, zero);
    saveSettings();
    Serial.println("[RESET] Standardwerte wiederhergestellt – Offset-Kalibrierung noetig");
}

// ──────────────────────────────────────────────
// Joystick-Mapping
// Positive und negative Richtung werden separat skaliert,
// damit asymmetrische Joysticks trotzdem ±100 erreichen.
// ──────────────────────────────────────────────
int16_t mapJS(int16_t raw, int ch) {
    int32_t v;
    if (raw >= jsCenter[ch]) {
        int32_t range = jsMax[ch] - jsCenter[ch];
        v = (range > 0) ? ((int32_t)raw - jsCenter[ch]) * 100 / range : 0;
    } else {
        int32_t range = jsCenter[ch] - jsMin[ch];
        v = (range > 0) ? ((int32_t)raw - jsCenter[ch]) * 100 / range : 0;
    }
    if (v >  100) v =  100;
    if (v < -100) v = -100;
    if (v > -JS_DEADZONE && v < JS_DEADZONE) v = 0;
    return (int16_t)v;
}

// ──────────────────────────────────────────────
// Rumble
// ──────────────────────────────────────────────
void handleRumble() {
    if (!rumbleEnabled || batPct <= BAT_CRIT_PCT) {
        analogWrite(PIN_RUMBLE, 0);
        rumbleActive = false;
        return;
    }
    unsigned long now = millis();
    if (!rumbleActive && (now - lastRumble >= RUMBLE_INTERVAL_MS)) {
        analogWrite(PIN_RUMBLE, RUMBLE_PWM);
        rumbleActive = true;
        lastRumble = now;
        Serial.println("[RUMBLE] an");
    }
    if (rumbleActive && (now - lastRumble >= RUMBLE_DURATION_MS)) {
        analogWrite(PIN_RUMBLE, 0);
        rumbleActive = false;
        Serial.println("[RUMBLE] aus");
    }
}

// ──────────────────────────────────────────────
// Display-Hilfsfunktionen
// ──────────────────────────────────────────────
void dispTitle(const char* title) {
    if (!dispOK) return;
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(title);
    display.drawFastHLine(0, 9, 128, SSD1306_WHITE);
}

void dispHighlight(int y, const char* text, int h = 10) {
    display.fillRect(0, y, 128, h, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(4, y + (h >= 10 ? 1 : 0));
    display.print(text);
    display.setTextColor(SSD1306_WHITE);
}

// ──────────────────────────────────────────────
// Display: Akku-Icon (15x6px)
// Balken je 1px breit, 4px hoch, 1px Lücke; Nub 2px rechts daneben
// FB-Icon: pct → bars-Konversion, feste Position (113,1).
// Auto-Icon: carBat 0–5 direkt als bars, Position (95,1).
// ──────────────────────────────────────────────
void drawBatteryIconBars(uint8_t bars, uint8_t x, uint8_t y) {
    display.drawRect(x, y, 13, 6, SSD1306_WHITE);
    display.fillRect(x + 13, y + 2, 2, 2, SSD1306_WHITE);
    for (uint8_t i = 0; i < 6; i++) {
        if (i < bars)
            display.fillRect(x + 1 + i * 2, y + 1, 1, 4, SSD1306_WHITE);
    }
}

void drawBatteryIcon(uint8_t pct) {
    uint8_t bars = (pct > 83) ? 6 :
                   (pct > 66) ? 5 :
                   (pct > 50) ? 4 :
                   (pct > 33) ? 3 :
                   (pct > 16) ? 2 : 1;
    drawBatteryIconBars(bars, 113, 1);
}

// ──────────────────────────────────────────────
// Display: Normalbetrieb
// ──────────────────────────────────────────────
void displayNormal(int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                   bool bYellow, bool bGreen, bool bBlue, bool bRed) {
    if (!dispOK) return;

    static unsigned long lastDisplayMs = 0;
    static bool    c_paired   = false;
    static uint8_t c_position = 0xFF, c_lap = 0, c_lapTotal = 0, c_item = 0;
    static uint8_t c_batPct   = 0xFF, c_carBat = 0xFF;
    static bool    c_bY = false, c_bG = false, c_bB = false, c_bR = false;

    bool changed = (paired             != c_paired)   ||
                   (feedback.position  != c_position) ||
                   (feedback.lap       != c_lap)      ||
                   (feedback.lapTotal  != c_lapTotal)  ||
                   (feedback.item      != c_item)      ||
                   (feedback.carBat    != c_carBat)    ||
                   (batPct             != c_batPct)   ||
                   (bYellow != c_bY) || (bGreen != c_bG) ||
                   (bBlue   != c_bB) || (bRed   != c_bR);
    if (!changed || millis() - lastDisplayMs < DISPLAY_PERIOD_MS) return;
    lastDisplayMs = millis();

    c_paired    = paired;
    c_position  = feedback.position;
    c_lap       = feedback.lap;
    c_lapTotal  = feedback.lapTotal;
    c_item      = feedback.item;
    c_carBat    = feedback.carBat;
    c_batPct    = batPct;
    c_bY = bYellow; c_bG = bGreen; c_bB = bBlue; c_bR = bRed;

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,  0); display.printf("LX:%4d  LY:%4d", lx, ly);
    display.setCursor(0,  8); display.printf("RX:%4d  RY:%4d", rx, ry);
    display.setCursor(0, 16); display.printf("Y:%d G:%d B:%d R:%d", bYellow, bGreen, bBlue, bRed);
    display.setCursor(0, 24);
    if (!paired)
        display.print("Suche...");
    else if (feedback.position > 0)
        display.printf("P:%d L:%d/%d Item:%d", feedback.position, feedback.lap, feedback.lapTotal, feedback.item);
    else
        display.print("Verbunden");
    drawBatteryIcon(batPct);
    drawBatteryIconBars(feedback.carBat, 95, 1);
    display.display();
}

// ──────────────────────────────────────────────
// Display: Menü
// ──────────────────────────────────────────────
void displayConnecting() {
    if (!dispOK) return;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    if (trySavedChannel) {
        display.setCursor((128 - (int)strlen(S().rejoiningLine1) * 6) / 2, 24);
        display.print(S().rejoiningLine1);
        display.setCursor((128 - (int)strlen(S().rejoiningLine2) * 6) / 2, 34);
        display.print(S().rejoiningLine2);
    } else {
        const char* msg = (fbMode == MODE_DIRECT) ? S().connectingDirect : S().connectingGame;
        display.setCursor((128 - (int)strlen(msg) * 6) / 2, 28);
        display.print(msg);
    }
    drawBatteryIcon(batPct);
    display.display();
}

void displayMenu() {
    if (!dispOK) return;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print(S().menuTitle);
    const char* modeStr = fbMode == MODE_DIRECT ? "Direct" : "Game";
    display.setCursor(128 - (int)strlen(modeStr) * 6, 0);
    display.print(modeStr);
    display.drawFastHLine(0, 9, 128, SSD1306_WHITE);
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int y = 11 + i * 8;
        char buf[22];
        const char* itemText = S().menuItems[i];
        if (i == 5) {
            snprintf(buf, sizeof(buf), "Rumble: %s",
                rumbleEnabled ? S().rumbleOn : S().rumbleOff);
            itemText = buf;
        } else if (i == 6) {
            snprintf(buf, sizeof(buf), "%s: %s",
                S().menuItems[6],
                swapSticks ? S().swapOn : S().swapOff);
            itemText = buf;
        }
        if (i == menuSel)
            dispHighlight(y, itemText, 8);
        else {
            display.setCursor(4, y);
            display.print(itemText);
        }
    }
    display.display();
}

// ──────────────────────────────────────────────
// Display: Offset-Kalibrierung
// ──────────────────────────────────────────────
void displayOffsetRelease() {
    if (!dispOK) return;
    display.clearDisplay();
    dispTitle(S().calOffTitle);
    display.setCursor(0, 22);
    display.println(S().calOffRelease);
    display.display();
}
void displayOffsetDoing() {
    if (!dispOK) return;
    display.clearDisplay();
    dispTitle(S().calOffTitle);
    display.setCursor(0, 28);
    display.println(S().calOffDoing);
    display.display();
}

// ──────────────────────────────────────────────
// Display: Min/Max-Kalibrierung
// ──────────────────────────────────────────────
void displayMinMaxStep(uint8_t step) {
    if (!dispOK) return;
    display.clearDisplay();
    dispTitle(S().calMMTitle);
    display.setCursor(0, 16);
    display.println(S().calMMSteps[step].line1);
    display.setCursor(0, 26);
    display.println(S().calMMSteps[step].line2);
    display.setCursor(0, 44);
    display.printf("%s %d/8", S().calMMStep, step + 1);
    display.setCursor(0, 54);
    display.println(S().calMMConfirm);
    display.display();
}

// ──────────────────────────────────────────────
// Display: Trim-Bar
// Aktive Positionen (vom Zentrum bis zum Trimwert) werden invertiert.
// ──────────────────────────────────────────────
void displayTrimBar(int8_t trim) {
    if (!dispOK) return;
    display.clearDisplay();
    dispTitle(S().trimTitle);
    display.setCursor(1, 28);
    for (int i = 0; i < TRIM_STEPS; i++) {
        int pos = i - 10;
        bool active = (trim >= 0) ? (pos >= 0 && pos <= trim)
                                  : (pos <= 0 && pos >= trim);
        display.setTextColor(active ? SSD1306_BLACK : SSD1306_WHITE,
                             active ? SSD1306_WHITE : SSD1306_BLACK);
        display.print('|');
    }
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(55, 44);
    display.printf("%+d", trim);
    display.display();
}

// ──────────────────────────────────────────────
// Display: Speed-Bar
// Die aktuelle Stufe wird invertiert dargestellt.
// ──────────────────────────────────────────────
void displaySpeedBar(uint8_t speed) {
    if (!dispOK) return;
    display.clearDisplay();
    dispTitle(S().speedTitle);
    display.setCursor(4, 28);
    for (int i = 1; i <= SPEED_STEPS; i++) {
        bool active = (i == (int)speed);
        display.setTextColor(active ? SSD1306_BLACK : SSD1306_WHITE,
                             active ? SSD1306_WHITE : SSD1306_BLACK);
        display.print('|');
    }
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(72, 44);
    display.printf("%3d%%", speed * 10);
    display.display();
}

// ──────────────────────────────────────────────
// Display: Reset-Countdown
// ──────────────────────────────────────────────
void displayResetCountdown(int secs) {
    if (!dispOK) return;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(24, 4);
    display.print("Reset in");
    display.setTextSize(4);
    display.setCursor(44, 24);
    display.print(secs);
    display.print("s");
    display.display();
}

// ──────────────────────────────────────────────
// Display: Reset-Bestätigung
// ──────────────────────────────────────────────
void displayReset() {
    if (!dispOK) return;
    display.clearDisplay();
    dispTitle(S().resetTitle);
    display.setCursor(0, 28);
    display.println(S().resetDone);
    display.display();
}

// ──────────────────────────────────────────────
// Display: Sprachauswahl
// ──────────────────────────────────────────────
void displayLanguage(uint8_t sel) {
    if (!dispOK) return;
    display.clearDisplay();
    dispTitle(S().langTitle);
    for (int i = 0; i < LANG_COUNT; i++) {
        int y = 14 + i * 14;
        if (i == (int)sel)
            dispHighlight(y, S().langNames[i]);
        else {
            display.setCursor(4, y + 1);
            display.print(S().langNames[i]);
        }
    }
    display.display();
}

// ──────────────────────────────────────────────
// Joystick-Richtung für Menünavigation (Rohwerte)
// ──────────────────────────────────────────────
int8_t jsMenuY(int16_t rawLY, int16_t rawRY) {
    unsigned long now = millis();
    if (now - lastMenuMove < MENU_COOLDOWN) return 0;
    if (max(rawLY, rawRY) > JS_MENU_HIGH) { lastMenuMove = now; return  1; }
    if (min(rawLY, rawRY) < JS_MENU_LOW)  { lastMenuMove = now; return -1; }
    return 0;
}

int8_t jsMenuX(int16_t rawLX, int16_t rawRX) {
    unsigned long now = millis();
    if (now - lastMenuMove < MENU_COOLDOWN) return 0;
    if (max(rawLX, rawRX) > JS_MENU_HIGH) { lastMenuMove = now; return  1; }
    if (min(rawLX, rawRX) < JS_MENU_LOW)  { lastMenuMove = now; return -1; }
    return 0;
}

void sendConfigPacket();  // forward declaration (definiert im ESP-NOW-Block)

// ──────────────────────────────────────────────
// State Machine
// *P = Tastendruck (Flanke); bGreen/bBlue zusätzlich als Rohzustand für Combo
// ──────────────────────────────────────────────
bool handleState(bool bYellowP, bool bGreenP, bool bBlueP, bool bRedP,
                 bool bGreen, bool bBlue,
                 int16_t rawLX, int16_t rawLY, int16_t rawRX, int16_t rawRY) {
    unsigned long now = millis();

    switch (state) {

        case STATE_READY:
            if (bGreen && bBlue) {
                if (calHoldStart == 0) calHoldStart = now;
                if (now - calHoldStart >= CAL_HOLD_MS) {
                    state = STATE_MENU;
                    menuSel = 0;
                    calHoldStart = 0;
                    ledPink();
                    displayMenu();
                    return false;
                }
            } else {
                calHoldStart = 0;
            }
            ledReady();
            return true;

        case STATE_MENU: {
            ledPink();
            int8_t dir = jsMenuY(rawLY, rawRY);
            if (dir) {
                menuSel = constrain(menuSel + dir, 0, MENU_ITEM_COUNT - 1);
                displayMenu();
            }
            if (bYellowP) {
                switch (menuSel) {
                    case 0: // Offset-Kalibrierung
                        state = STATE_CAL_OFFSET_PULSE;
                        stateStart = now;
                        displayOffsetRelease();
                        break;
                    case 1: // Min/Max-Kalibrierung
                        memcpy(jsMinTemp, jsMin, sizeof(jsMin));
                        memcpy(jsMaxTemp, jsMax, sizeof(jsMax));
                        calStep = 0;
                        state = STATE_CAL_MINMAX;
                        displayMinMaxStep(0);
                        break;
                    case 2: // Trim
                        trimTemp = servoTrim;
                        state = STATE_TRIM;
                        displayTrimBar(trimTemp);
                        break;
                    case 3: // Speed
                        speedTemp = maxSpeed;
                        state = STATE_SPEED;
                        displaySpeedBar(speedTemp);
                        break;
                    case 4: // Sprache
                        langTemp = langIndex;
                        state = STATE_LANGUAGE;
                        displayLanguage(langTemp);
                        break;
                    case 5: // Rumble toggle (temporär, kein EEPROM)
                        rumbleEnabled = !rumbleEnabled;
                        displayMenu();
                        break;
                    case 6: // Joysticks tauschen (temporär, kein EEPROM)
                        swapSticks = !swapSticks;
                        displayMenu();
                        break;
                    case 7: // Reset
                        resetSettings();
                        state = STATE_RESET;
                        stateStart = now;
                        displayReset();
                        break;
                }
            }
            if (bRedP) {
                state = STATE_READY;
                ledReady();
            }
            return false;
        }

        case STATE_CAL_OFFSET_PULSE:
            ledPulseYellow();
            if (now - stateStart >= CAL_PULSE_MS) {
                state = STATE_CAL_OFFSET;
                stateStart = now;
                calDone = false;
                ledYellow();
                displayOffsetDoing();
            }
            return false;

        case STATE_CAL_OFFSET:
            if (!calDone) { calibrateOffset(); calDone = true; }
            ledYellow();
            if (now - stateStart >= CAL_SHOW_MS) {
                state = STATE_READY;
                ledReady();
            }
            return false;

        case STATE_CAL_MINMAX:
            ledYellow();
            if (bYellowP) {
                int16_t raw[4] = {rawLX, rawLY, rawRX, rawRY};
                int ch = CAL_STEP_CH[calStep];
                if (CAL_STEP_ISMAX[calStep])
                    jsMaxTemp[ch] = raw[ch];
                else
                    jsMinTemp[ch] = raw[ch];
                calStep++;
                if (calStep >= 8) {
                    memcpy(jsMin, jsMinTemp, sizeof(jsMin));
                    memcpy(jsMax, jsMaxTemp, sizeof(jsMax));
                    saveSettings();
                    Serial.printf("[CAL-MM] min:%d %d %d %d  max:%d %d %d %d\n",
                        jsMin[0],jsMin[1],jsMin[2],jsMin[3],
                        jsMax[0],jsMax[1],jsMax[2],jsMax[3]);
                    state = STATE_READY;
                    ledReady();
                } else {
                    displayMinMaxStep(calStep);
                }
            }
            if (bRedP) {
                state = STATE_MENU;
                displayMenu();
            }
            return false;

        case STATE_TRIM: {
            int8_t dir = jsMenuX(rawLX, rawRX);
            if (dir) {
                trimTemp = constrain(trimTemp + dir, TRIM_MIN, TRIM_MAX);
                displayTrimBar(trimTemp);
            }
            if (bYellowP) { servoTrim = trimTemp; saveSettings(); sendConfigPacket(); state = STATE_MENU; displayMenu(); }
            if (bRedP) { state = STATE_MENU; displayMenu(); }
            return false;
        }

        case STATE_SPEED: {
            int8_t dir = jsMenuX(rawLX, rawRX);
            if (dir) {
                speedTemp = constrain((int)speedTemp + dir, 1, SPEED_STEPS);
                displaySpeedBar(speedTemp);
            }
            if (bYellowP) { maxSpeed = speedTemp; state = STATE_MENU; displayMenu(); }
            if (bRedP) { state = STATE_MENU; displayMenu(); }
            return false;
        }

        case STATE_RESET:
            if (now - stateStart >= CAL_SHOW_MS) {
                state = STATE_READY;
                ledReady();
                return true;  // sofort displayNormal aufrufen
            }
            return false;

        case STATE_LANGUAGE: {
            int8_t dir = jsMenuY(rawLY, rawRY);
            if (dir) {
                langTemp = constrain((int)langTemp + dir, 0, LANG_COUNT - 1);
                displayLanguage(langTemp);
            }
            if (bYellowP) { langIndex = langTemp; saveSettings(); state = STATE_MENU; displayMenu(); }
            if (bRedP) { state = STATE_MENU; displayMenu(); }
            return false;
        }
    }
    return true;
}

// ──────────────────────────────────────────────
// ESP-NOW
// ──────────────────────────────────────────────
void onDataRecv(uint8_t *senderMac, uint8_t *data, uint8_t len) {
    if (len < 1) return;
    uint8_t msgType = data[0];
    if (!paired) {
        if (fbMode == MODE_DIRECT && msgType == MSG_BEACON) {
            memcpy(pairingCarMac, senderMac, 6);
            pairingBeaconRx = true;
        } else if (fbMode == MODE_GAME && msgType == MSG_ASSIGN) {
            if (len < (int)sizeof(MK_Assign)) return;
            memcpy(pairingBaseMac, ((MK_Assign*)data)->baseMac, 6);
            pairingAssignRx = true;
        }
        return;
    }
    if (msgType == MSG_FEEDBACK && len >= (int)sizeof(MK_GameFeedback)) {
        const MK_GameFeedback* fb = (const MK_GameFeedback*)data;
        feedbackRaw.position   = fb->position;
        feedbackRaw.lap        = fb->lap;
        feedbackRaw.lapTotal   = fb->lapTotal;
        feedbackRaw.item       = fb->item;
        feedbackRaw.speedLimit = fb->speedLimit;
        feedbackRaw.rumble     = fb->rumble;
        feedbackRaw.carBat     = fb->carBat;
        feedbackNew = true;
    } else if (msgType == MSG_CHANNEL_SWITCH && len >= (int)sizeof(MK_ChannelSwitch)) {
        uint8_t ch = ((const MK_ChannelSwitch*)data)->channel;
        wifi_set_channel(ch);
        pendingChannelSave = ch;
    }
}

void initEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.macAddress(realMac);
    if (fbMode == MODE_DIRECT) {
        uint8_t fakeMac[] = MK_BASE_MAC;
        wifi_set_macaddr(STATION_IF, fakeMac);
        Serial.println("[ESPNOW] Direct: MAC → DE:AD:BE:EF:BA:5E");
    }
    uint8_t savedCh = 0;
    EEPROM.get(EEPROM_ADDR_CHANNEL, savedCh);
    if (savedCh >= 1 && savedCh <= 13) {
        wifi_set_channel(savedCh);
        trySavedChannel  = true;
        savedChannelStart = millis();
        Serial.printf("[ESPNOW] Gespeicherter Kanal %d – warte auf Pairing\n", savedCh);
    } else {
        wifi_set_channel(MK_ESPNOW_CHANNEL);
    }
    if (esp_now_init() != 0) { Serial.println("[ESPNOW] Init fehlgeschlagen"); return; }
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onDataRecv);
    if (fbMode == MODE_GAME) {
        uint8_t baseMac[] = MK_BASE_MAC;
        esp_now_add_peer(baseMac, ESP_NOW_ROLE_COMBO, MK_ESPNOW_CHANNEL, NULL, 0);
    }
    Serial.printf("[ESPNOW] Init OK, Kanal %d\n", MK_ESPNOW_CHANNEL);
}

void handlePairing() {
    if (trySavedChannel && !paired && millis() - savedChannelStart > SAVED_CHANNEL_TIMEOUT_MS) {
        uint8_t zero = 0;
        EEPROM.put(EEPROM_ADDR_CHANNEL, zero);
        EEPROM.commit();
        Serial.println("[ESPNOW] Kein Pairing auf gespeichertem Kanal → Reboot");
        ESP.restart();
    }
    if (paired) return;
    if (fbMode == MODE_DIRECT && pairingBeaconRx) {
        pairingBeaconRx = false;
        memcpy(peerMac, pairingCarMac, 6);
        esp_now_add_peer(peerMac, ESP_NOW_ROLE_COMBO, MK_ESPNOW_CHANNEL, NULL, 0);
        wifi_set_macaddr(STATION_IF, realMac);
        MK_Assign assign;
        assign.slot = 1;
        memcpy(assign.baseMac, realMac, 6);
        esp_now_send(peerMac, (uint8_t*)&assign, sizeof(assign));
        // Zufälligen Betriebskanal wählen und Auto zum Wechsel auffordern
        const uint8_t channels[] = MK_DIRECT_CHANNELS;
        uint8_t ch = channels[random(MK_DIRECT_CHAN_COUNT)];
        MK_ChannelSwitch chSwitch;
        chSwitch.channel = ch;
        delay(20);  // kurz warten damit Auto MK_Assign verarbeiten kann
        esp_now_send(peerMac, (uint8_t*)&chSwitch, sizeof(chSwitch));
        delay(20);  // kurz warten damit Auto MK_ChannelSwitch verarbeiten kann
        wifi_set_channel(ch);
        EEPROM.put(EEPROM_ADDR_CHANNEL, ch);
        EEPROM.commit();
        paired = true;
        trySavedChannel = false;
        lastFeedbackMs = millis();
        Serial.printf("[ESPNOW] Direct: gepairt, Kanal %d\n", ch);
        ledReady();
    }
    if (fbMode == MODE_GAME && pairingAssignRx) {
        pairingAssignRx = false;
        memcpy(peerMac, pairingBaseMac, 6);
        esp_now_add_peer(peerMac, ESP_NOW_ROLE_COMBO, MK_ESPNOW_CHANNEL, NULL, 0);
        paired = true;
        trySavedChannel = false;
        lastFeedbackMs = millis();
        Serial.println("[ESPNOW] Game: gepairt");
        ledReady();
    }
}

void sendBeacon() {
    if (paired || fbMode != MODE_GAME) return;
    unsigned long now = millis();
    if (now - lastBeacon < BEACON_INTERVAL_MS) return;
    lastBeacon = now;
    MK_Beacon beacon;
    beacon.deviceType = DEVICE_FB;
    uint8_t baseMac[] = MK_BASE_MAC;
    esp_now_send(baseMac, (uint8_t*)&beacon, sizeof(beacon));
}

void sendControlInput(int8_t throttle, int8_t steering,
                      bool bY, bool bG, bool bB, bool bR) {
    if (!paired) return;
    MK_ControlInput pkt;
    pkt.throttle = throttle;
    pkt.steering = steering;
    pkt.buttons  = (bY ? MK_BTN_YELLOW : 0) | (bG ? MK_BTN_GREEN : 0) | (bB ? MK_BTN_BLUE : 0) | (bR ? MK_BTN_RED : 0);
    pkt.maxSpeed = min((uint8_t)maxSpeed, feedback.speedLimit);
    esp_now_send(peerMac, (uint8_t*)&pkt, sizeof(pkt));
}

void sendConfigPacket() {
    if (!paired) return;
    MK_ConfigPacket pkt;
    pkt.trim = servoTrim;
    esp_now_send(peerMac, (uint8_t*)&pkt, sizeof(pkt));
}

void handleFeedback() {
    if (feedbackNew) {
        noInterrupts();
        feedback.position   = feedbackRaw.position;
        feedback.lap        = feedbackRaw.lap;
        feedback.lapTotal   = feedbackRaw.lapTotal;
        feedback.item       = feedbackRaw.item;
        feedback.speedLimit = feedbackRaw.speedLimit;
        feedback.carBat     = feedbackRaw.carBat;
        uint8_t rumbleCmd   = feedbackRaw.rumble;
        feedbackNew = false;
        interrupts();
        lastFeedbackMs = millis();
        if (rumbleCmd == 1) rumbleFbEnd = millis() + 200;  // 200ms Safety-Timeout falls rumble=0 verloren geht
        else                rumbleFbEnd = 0;
    }
    if (paired && millis() - lastFeedbackMs > FEEDBACK_TIMEOUT_MS) {
        paired = false;
        lastFeedbackMs = millis();
        feedback = FeedbackState{};
        Serial.println("[ESPNOW] Verbindung verloren");
    }
}

void handleRumbleFb() {
    if (!rumbleEnabled || batPct <= BAT_CRIT_PCT) {
        analogWrite(PIN_RUMBLE, 0);
        return;
    }
    analogWrite(PIN_RUMBLE, (millis() < rumbleFbEnd) ? RUMBLE_PWM : 0);
}

// ──────────────────────────────────────────────
// Setup
// ──────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(PIN_BTN_YELLOW, INPUT_PULLUP);
    pinMode(PIN_BTN_GREEN,  INPUT_PULLUP);
    pinMode(PIN_BTN_BLUE,   INPUT_PULLUP);
    pinMode(PIN_BTN_RED,    INPUT_PULLUP);
    pinMode(PIN_RUMBLE, OUTPUT);
    analogWrite(PIN_RUMBLE, 0);
    Wire.begin(D2, D1);
    EEPROM.begin(EEPROM_SIZE);
    led.begin();
    led.setBrightness(80);
    setLed(0, 0, 0);
    pinMode(PIN_MODE, INPUT);
    fbMode = digitalRead(PIN_MODE) ? MODE_DIRECT : MODE_GAME;
    Serial.printf("[MODE] %s\n", fbMode == MODE_DIRECT ? "Direct" : "Game");
    adsOK  = ads.begin(0x48);
    if (adsOK) ads.setDataRate(RATE_ADS1115_860SPS);
    dispOK = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    if (!adsOK)  Serial.println("[FEHLER] ADS1115 nicht gefunden");
    if (!dispOK) Serial.println("[FEHLER] SSD1306 nicht gefunden");
    if (!digitalRead(PIN_BTN_RED)) {
        Serial.println("[RESET] Boot-Reset via Red");
        resetSettings();
        displayReset();
        delay(CAL_SHOW_MS);
    }
    if (!loadSettings()) {
        Serial.println("[EEPROM] kein Wert, Standardwerte aktiv – Offset-Kalibrierung noetig");
        saveSettings();
    }
    maxSpeed = SPEED_STEPS;
    updateBattery();
    lastBatCheck = millis();
    state = STATE_READY;
    initEspNow();
    ledPulseOrange();
}

// ──────────────────────────────────────────────
// Loop
// ──────────────────────────────────────────────
void loop() {
    static unsigned long lastLoopMs = 0;
    static bool prevYellow = false, prevGreen = false, prevBlue = false, prevRed = false;

    bool bYellow = !digitalRead(PIN_BTN_YELLOW);
    bool bGreen  = !digitalRead(PIN_BTN_GREEN);
    bool bBlue   = !digitalRead(PIN_BTN_BLUE);
    bool bRed    = !digitalRead(PIN_BTN_RED);

    bool bYellowP = bYellow && !prevYellow;
    bool bGreenP  = bGreen  && !prevGreen;
    bool bBlueP   = bBlue   && !prevBlue;
    bool bRedP    = bRed    && !prevRed;
    prevYellow = bYellow;
    prevGreen  = bGreen;
    prevBlue   = bBlue;
    prevRed    = bRed;

    // Letzter bekannter Wert bleibt erhalten wenn ein Kanal gerade nicht gelesen wird
    static int16_t rawLX = 0, rawLY = 0, rawRX = 0, rawRY = 0;
    if (adsOK) {
        if (state == STATE_READY) {
            // Nur die 2 aktiven Kanäle, dafür mit Oversampling
            uint8_t chA = swapSticks ? JS_RIGHT_Y : JS_LEFT_Y;
            uint8_t chB = swapSticks ? JS_LEFT_X  : JS_RIGHT_X;
            int32_t sumA = 0, sumB = 0;
            for (int i = 0; i < ADC_OVERSAMPLE; i++) {
                sumA += ads.readADC_SingleEnded(chA);
                sumB += ads.readADC_SingleEnded(chB);
            }
            if (swapSticks) { rawRY = sumA / ADC_OVERSAMPLE; rawLX = sumB / ADC_OVERSAMPLE; }
            else            { rawLY = sumA / ADC_OVERSAMPLE; rawRX = sumB / ADC_OVERSAMPLE; }
        } else {
            // Menü/Kalibrierung: alle 4 Kanäle für Navigation und Kalibrierung
            rawLX = ads.readADC_SingleEnded(JS_LEFT_X);
            rawLY = ads.readADC_SingleEnded(JS_LEFT_Y);
            rawRX = ads.readADC_SingleEnded(JS_RIGHT_X);
            rawRY = ads.readADC_SingleEnded(JS_RIGHT_Y);
        }
    }

    // Red 10s halten im Normalbetrieb → EEPROM-Reset (ab 5s Countdown)
    static unsigned long bRedResetHold = 0;
    static int           lastCountdown = -1;
    bool countdownShowing = false;
    if (state == STATE_READY) {
        if (bRed) {
            if (bRedResetHold == 0) bRedResetHold = millis();
            unsigned long elapsed = millis() - bRedResetHold;
            if (elapsed >= 10000) {
                resetSettings();
                state = STATE_RESET;
                stateStart = millis();
                displayReset();
                bRedResetHold = 0;
                lastCountdown = -1;
            } else if (elapsed >= 5000) {
                int cd = 10 - (int)(elapsed / 1000);
                if (cd != lastCountdown) {
                    lastCountdown = cd;
                    displayResetCountdown(cd);
                }
                countdownShowing = true;
            }
        } else {
            if (bRedResetHold != 0) lastCountdown = -1;
            bRedResetHold = 0;
        }
    }

    if (millis() - lastBatCheck >= BAT_CHECK_MS) {
        lastBatCheck = millis();
        updateBattery();
    }

    handleFeedback();
    if (pendingChannelSave > 0) {
        uint8_t ch = pendingChannelSave;
        pendingChannelSave = 0;
        EEPROM.put(EEPROM_ADDR_CHANNEL, ch);
        EEPROM.commit();
        Serial.printf("[ESPNOW] Kanal %d gespeichert\n", ch);
    }
    handlePairing();
    sendBeacon();

    // Suchzustand: LED pulsiert bis Pairing steht
    if (state == STATE_READY && !paired) {
        ledPulseOrange();
        displayConnecting();
    }

    bool active = handleState(bYellowP, bGreenP, bBlueP, bRedP, bGreen, bBlue, rawLX, rawLY, rawRX, rawRY);

    if (active) {
        // Default: LY→throttle, RX→steering  |  Swapped: RY→throttle, LX→steering
        int8_t throttle = swapSticks ? mapJS(rawRY, JS_RIGHT_Y) : mapJS(rawLY, JS_LEFT_Y);
        int8_t steering = swapSticks ? mapJS(rawLX, JS_LEFT_X)  : mapJS(rawRX, JS_RIGHT_X);

        Serial.printf("LX:%6d | LY:%6d | RX:%6d | RY:%6d | Y:%d G:%d B:%d R:%d | thr:%4d str:%4d\n",
            rawLX, rawLY, rawRX, rawRY, bYellow, bGreen, bBlue, bRed, throttle, steering);

        sendControlInput(throttle, steering, bYellow, bGreen, bBlue, bRed);

        if (paired && !countdownShowing) {
            displayNormal(
                mapJS(rawLX, JS_LEFT_X), mapJS(rawLY, JS_LEFT_Y),
                mapJS(rawRX, JS_RIGHT_X), mapJS(rawRY, JS_RIGHT_Y),
                bYellow, bGreen, bBlue, bRed
            );
        }
    }

    handleRumbleFb();

    unsigned long elapsed = millis() - lastLoopMs;
    if (LOOP_PERIOD_MS > elapsed) delay(LOOP_PERIOD_MS - elapsed);
    lastLoopMs = millis();
}
