#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

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

// --- EEPROM ---
#define EEPROM_MAGIC        0xCAFE
#define EEPROM_ADDR_MAGIC   0
#define EEPROM_ADDR_CENTER  2   // 4x int16 = 8B
#define EEPROM_ADDR_MIN    10   // 4x int16 = 8B
#define EEPROM_ADDR_MAX    18   // 4x int16 = 8B
#define EEPROM_ADDR_TRIM   26   // int8
#define EEPROM_ADDR_LANG   27   // uint8
#define EEPROM_SIZE        32

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
        "An", "Aus"
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
        "On", "Off"
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
void ledRedBlink() { uint8_t v = ((millis() / 500) % 2) ? 60 : 0; setLed(v, 0, 0); }
void ledOrange()   { setLed(60, 20, 0); }
void ledPink()     { setLed(60, 0, 40); }
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
    servoTrim     = 0;
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
// Display: Akku-Icon (top-right, 15x6px)
// 6 Tiers: >83 / >66 / >50 / >33 / >16 / sonst → 6..1 Balken
// Balken je 1px breit, 4px hoch, 1px Lücke; Nub 2px rechts daneben
// ──────────────────────────────────────────────
void drawBatteryIcon(uint8_t pct) {
    uint8_t bars = (pct > 83) ? 6 :
                   (pct > 66) ? 5 :
                   (pct > 50) ? 4 :
                   (pct > 33) ? 3 :
                   (pct > 16) ? 2 : 1;
    display.drawRect(113, 1, 13, 6, SSD1306_WHITE);
    display.fillRect(126, 3, 2, 2, SSD1306_WHITE);
    for (uint8_t i = 0; i < 6; i++) {
        if (i < bars)
            display.fillRect(114 + i * 2, 2, 1, 4, SSD1306_WHITE);
    }
}

// ──────────────────────────────────────────────
// Display: Normalbetrieb
// ──────────────────────────────────────────────
void displayNormal(int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                   bool bYellow, bool bGreen, bool bBlue, bool bRed) {
    if (!dispOK) return;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,  0); display.printf("LX:%4d  LY:%4d", lx, ly);
    display.setCursor(0,  8); display.printf("RX:%4d  RY:%4d", rx, ry);
    display.setCursor(0, 16); display.printf("Y:%d G:%d B:%d R:%d", bYellow, bGreen, bBlue, bRed);
    drawBatteryIcon(batPct);
    display.display();
}

// ──────────────────────────────────────────────
// Display: Menü
// ──────────────────────────────────────────────
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
                ledGreen();
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
                ledGreen();
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
                    ledGreen();
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
            if (bYellowP) { servoTrim = trimTemp; saveSettings(); state = STATE_MENU; displayMenu(); }
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
                ledGreen();
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
    ledGreen();
}

// ──────────────────────────────────────────────
// Loop
// ──────────────────────────────────────────────
void loop() {
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

    int16_t rawLX = adsOK ? ads.readADC_SingleEnded(JS_LEFT_X)  : jsCenter[0];
    int16_t rawLY = adsOK ? ads.readADC_SingleEnded(JS_LEFT_Y)  : jsCenter[1];
    int16_t rawRX = adsOK ? ads.readADC_SingleEnded(JS_RIGHT_X) : jsCenter[2];
    int16_t rawRY = adsOK ? ads.readADC_SingleEnded(JS_RIGHT_Y) : jsCenter[3];

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

    bool active = handleState(bYellowP, bGreenP, bBlueP, bRedP, bGreen, bBlue, rawLX, rawLY, rawRX, rawRY);

    if (!active) {
        delay(50);
        return;
    }

    // Default: LY→throttle, RX→steering  |  Swapped: RY→throttle, LX→steering
    int8_t throttle = swapSticks ? mapJS(rawRY, JS_RIGHT_Y) : mapJS(rawLY, JS_LEFT_Y);
    int8_t steering = swapSticks ? mapJS(rawLX, JS_LEFT_X)  : mapJS(rawRX, JS_RIGHT_X);

    Serial.printf("LX:%6d | LY:%6d | RX:%6d | RY:%6d | Y:%d G:%d B:%d R:%d | thr:%4d str:%4d\n",
        rawLX, rawLY, rawRX, rawRY, bYellow, bGreen, bBlue, bRed, throttle, steering);

    if (!countdownShowing) {
        displayNormal(
            mapJS(rawLX, JS_LEFT_X), mapJS(rawLY, JS_LEFT_Y),
            mapJS(rawRX, JS_RIGHT_X), mapJS(rawRY, JS_RIGHT_Y),
            bYellow, bGreen, bBlue, bRed
        );
    }

    delay(100);
}
