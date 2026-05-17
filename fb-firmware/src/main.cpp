#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

// --- Pins ---
#define PIN_RUMBLE  D8
#define PIN_BTN1    D5
#define PIN_BTN2    D6
#define PIN_BTN3    D7
#define PIN_BTN4    D3
#define PIN_LED     D4

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

// --- Normales Display-Layout ---
#define DISP_DIVIDER_Y  25
#define DISP_RX_Y       29
#define DISP_RX_LINES    4

// --- Timings ---
#define CAL_HOLD_MS   3000
#define CAL_PULSE_MS  3000
#define CAL_SHOW_MS   3000

// --- Joystick ---
#define JS_DEADZONE     5
#define JS_DEFAULT_MIN  0
#define JS_DEFAULT_MAX  19700
#define JS_MENU_HIGH    14000
#define JS_MENU_LOW     2000
#define MENU_COOLDOWN   300

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
    const char* menuItems[5];
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
};

const Strings STRINGS[LANG_COUNT] = {
    // LANG_DE
    {
        "Einstellungen",
        { "Offset-Kalibrierung", "Min/Max-Kalibrierung",
          "Servo-Trim", "Max. Speed", "Sprache" },
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
        "B1 = bestaetigen",
        "Schritt",
        "Servo-Trim",
        "Max. Speed",
        "Sprache",
        { "Deutsch", "English" }
    },
    // LANG_EN
    {
        "Settings",
        { "Offset Calibration", "Min/Max Calibration",
          "Servo Trim", "Max. Speed", "Language" },
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
        "B1 = confirm",
        "Step",
        "Servo Trim",
        "Max. Speed",
        "Language",
        { "Deutsch", "English" }
    }
};

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
};

#define MENU_ITEM_COUNT 5

// --- Globale Variablen ---
Adafruit_ADS1115  ads;
Adafruit_SSD1306  display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Adafruit_NeoPixel led(1, PIN_LED, NEO_GRB + NEO_KHZ800);

bool adsOK  = false;
bool dispOK = false;

FBState  state        = STATE_READY;
uint8_t  langIndex    = LANG_DE;
int8_t   servoTrim    = 0;
uint8_t  maxSpeed     = SPEED_STEPS;   // temporär, nicht im EEPROM

int16_t  jsCenter[4]  = {9850, 9850, 9850, 9850};
int16_t  jsMin[4]     = {JS_DEFAULT_MIN, JS_DEFAULT_MIN, JS_DEFAULT_MIN, JS_DEFAULT_MIN};
int16_t  jsMax[4]     = {JS_DEFAULT_MAX, JS_DEFAULT_MAX, JS_DEFAULT_MAX, JS_DEFAULT_MAX};

unsigned long stateStart   = 0;
unsigned long calHoldStart = 0;
unsigned long lastMenuMove = 0;
unsigned long lastRumble   = 0;

bool    rumbleActive  = false;
bool    calDone       = false;
int8_t  menuSel       = 0;
uint8_t calStep       = 0;

int8_t  trimTemp      = 0;
uint8_t speedTemp     = SPEED_STEPS;
uint8_t langTemp      = LANG_DE;

int16_t jsMinTemp[4];
int16_t jsMaxTemp[4];

char    rxLines[DISP_RX_LINES][22];
String  serialBuf     = "";

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
// Serial-Eingabe
// ──────────────────────────────────────────────
void addRxLine(const String& line) {
    for (int i = 0; i < DISP_RX_LINES - 1; i++)
        memcpy(rxLines[i], rxLines[i+1], 22);
    strncpy(rxLines[DISP_RX_LINES-1], line.c_str(), 21);
    rxLines[DISP_RX_LINES-1][21] = '\0';
}
void handleSerial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialBuf.length() > 0) { addRxLine(serialBuf); serialBuf = ""; }
        } else if (serialBuf.length() < 21) {
            serialBuf += c;
        }
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

void dispHighlight(int y, const char* text) {
    display.fillRect(0, y, 128, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(4, y + 1);
    display.print(text);
    display.setTextColor(SSD1306_WHITE);
}

// ──────────────────────────────────────────────
// Display: Normalbetrieb
// ──────────────────────────────────────────────
void displayNormal(int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                   bool b1, bool b2, bool b3, bool b4) {
    if (!dispOK) return;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,  0); display.printf("LX:%4d  LY:%4d", lx, ly);
    display.setCursor(0,  8); display.printf("RX:%4d  RY:%4d", rx, ry);
    display.setCursor(0, 16); display.printf("B1:%d B2:%d B3:%d B4:%d", b1, b2, b3, b4);
    display.drawFastHLine(0, DISP_DIVIDER_Y, 128, SSD1306_WHITE);
    for (int i = 0; i < DISP_RX_LINES; i++) {
        display.setCursor(0, DISP_RX_Y + i * 8);
        display.print(rxLines[i]);
    }
    display.display();
}

// ──────────────────────────────────────────────
// Display: Menü
// ──────────────────────────────────────────────
void displayMenu() {
    if (!dispOK) return;
    display.clearDisplay();
    dispTitle(S().menuTitle);
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int y = 11 + i * 10;
        if (i == menuSel)
            dispHighlight(y, S().menuItems[i]);
        else {
            display.setCursor(4, y + 1);
            display.print(S().menuItems[i]);
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
// b1p/b4p = Tastendruck (Flanke), b2/b3 = Rohzustand für Combo
// ──────────────────────────────────────────────
bool handleState(bool b1p, bool b4p, bool b2, bool b3,
                 int16_t rawLX, int16_t rawLY, int16_t rawRX, int16_t rawRY) {
    unsigned long now = millis();

    switch (state) {

        case STATE_READY:
            if (b2 && b3) {
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
            ledGreen();
            return true;

        case STATE_MENU: {
            int8_t dir = jsMenuY(rawLY, rawRY);
            if (dir) {
                menuSel = constrain(menuSel + dir, 0, MENU_ITEM_COUNT - 1);
                displayMenu();
            }
            if (b1p) {
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
                }
            }
            if (b4p) {
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
            if (b1p) {
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
            if (b4p) {
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
            if (b1p) { servoTrim = trimTemp; saveSettings(); state = STATE_MENU; displayMenu(); }
            if (b4p) { state = STATE_MENU; displayMenu(); }
            return false;
        }

        case STATE_SPEED: {
            int8_t dir = jsMenuX(rawLX, rawRX);
            if (dir) {
                speedTemp = constrain((int)speedTemp + dir, 1, SPEED_STEPS);
                displaySpeedBar(speedTemp);
            }
            if (b1p) { maxSpeed = speedTemp; state = STATE_MENU; displayMenu(); }
            if (b4p) { state = STATE_MENU; displayMenu(); }
            return false;
        }

        case STATE_LANGUAGE: {
            int8_t dir = jsMenuY(rawLY, rawRY);
            if (dir) {
                langTemp = constrain((int)langTemp + dir, 0, LANG_COUNT - 1);
                displayLanguage(langTemp);
            }
            if (b1p) { langIndex = langTemp; saveSettings(); state = STATE_MENU; displayMenu(); }
            if (b4p) { state = STATE_MENU; displayMenu(); }
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
    pinMode(PIN_BTN1, INPUT_PULLUP);
    pinMode(PIN_BTN2, INPUT_PULLUP);
    pinMode(PIN_BTN3, INPUT_PULLUP);
    pinMode(PIN_BTN4, INPUT_PULLUP);
    pinMode(PIN_RUMBLE, OUTPUT);
    analogWrite(PIN_RUMBLE, 0);
    Wire.begin(D2, D1);
    EEPROM.begin(EEPROM_SIZE);
    led.begin();
    led.setBrightness(80);
    setLed(0, 0, 0);
    adsOK  = ads.begin(0x48);
    dispOK = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    if (!adsOK)  Serial.println("[FEHLER] ADS1115 nicht gefunden");
    if (!dispOK) Serial.println("[FEHLER] SSD1306 nicht gefunden");
    memset(rxLines, 0, sizeof(rxLines));
    if (!loadSettings()) {
        Serial.println("[EEPROM] kein Wert, kalibriere Offset...");
        calibrateOffset();
    }
    maxSpeed = SPEED_STEPS;
    state = STATE_READY;
    ledGreen();
}

// ──────────────────────────────────────────────
// Loop
// ──────────────────────────────────────────────
void loop() {
    static bool prevB1 = false, prevB4 = false;

    bool b1 = !digitalRead(PIN_BTN1);
    bool b2 = !digitalRead(PIN_BTN2);
    bool b3 = !digitalRead(PIN_BTN3);
    bool b4 = !digitalRead(PIN_BTN4);

    bool b1p = b1 && !prevB1;
    bool b4p = b4 && !prevB4;
    prevB1 = b1;
    prevB4 = b4;

    int16_t rawLX = adsOK ? ads.readADC_SingleEnded(JS_LEFT_X)  : jsCenter[0];
    int16_t rawLY = adsOK ? ads.readADC_SingleEnded(JS_LEFT_Y)  : jsCenter[1];
    int16_t rawRX = adsOK ? ads.readADC_SingleEnded(JS_RIGHT_X) : jsCenter[2];
    int16_t rawRY = adsOK ? ads.readADC_SingleEnded(JS_RIGHT_Y) : jsCenter[3];

    bool active = handleState(b1p, b4p, b2, b3, rawLX, rawLY, rawRX, rawRY);

    if (!active) {
        delay(50);
        return;
    }

    handleSerial();

    Serial.printf("LX:%6d | LY:%6d | RX:%6d | RY:%6d | B1:%d B2:%d B3:%d B4:%d\n",
        rawLX, rawLY, rawRX, rawRY, b1, b2, b3, b4);

    displayNormal(
        mapJS(rawLX, JS_LEFT_X), mapJS(rawLY, JS_LEFT_Y),
        mapJS(rawRX, JS_RIGHT_X), mapJS(rawRY, JS_RIGHT_Y),
        b1, b2, b3, b4
    );

    delay(100);
}
