#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

#define EEPROM_MAGIC    0xCAFE
#define EEPROM_ADDR_MAGIC   0
#define EEPROM_ADDR_CAL     2   // 4 x int16_t = 8 bytes
#define EEPROM_SIZE        16

#define PIN_RUMBLE  D8
#define PIN_BTN1    D5
#define PIN_BTN2    D6
#define PIN_BTN3    D7
#define PIN_BTN4    D3
#define PIN_LED     D4

#define JS_LEFT_X   0
#define JS_LEFT_Y   1
#define JS_RIGHT_X  2
#define JS_RIGHT_Y  3

#define OLED_WIDTH  128
#define OLED_HEIGHT 64

#define RUMBLE_INTERVAL_MS  10000
#define RUMBLE_DURATION_MS  1000
#define RUMBLE_PWM          255

#define DISP_DIVIDER_Y  25
#define DISP_RX_Y       29
#define DISP_RX_LINES    4

#define CAL_HOLD_MS     3000
#define CAL_PULSE_MS    3000
#define CAL_SHOW_MS     3000
#define JS_SCALE        12000

enum FBState {
    STATE_READY,
    STATE_CAL_PULSING,   // user releases sticks, LED pulses yellow
    STATE_CALIBRATING,   // calibration done, LED solid yellow, FB blocked
};

Adafruit_ADS1115 ads;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Adafruit_NeoPixel led(1, PIN_LED, NEO_GRB + NEO_KHZ800);

FBState state = STATE_READY;
unsigned long stateStart = 0;
unsigned long calHoldStart = 0;
bool calDone = false;

int16_t jsCenter[4] = {9850, 9850, 9850, 9850};

unsigned long lastRumble = 0;
bool rumbleActive = false;

char rxLines[DISP_RX_LINES][22];
String serialBuf = "";

void setLed(uint8_t r, uint8_t g, uint8_t b) {
    led.setPixelColor(0, led.Color(r, g, b));
    led.show();
}

void ledGreen()  { setLed(0, 60, 0); }
void ledYellow() { setLed(60, 60, 0); }

void ledPulseYellow() {
    uint32_t t = (millis() - stateStart) % 600;
    uint8_t v = (t < 300) ? (t * 60 / 300) : ((600 - t) * 60 / 300);
    setLed(v, v, 0);
}

void saveCalibration() {
    EEPROM.put(EEPROM_ADDR_MAGIC, (uint16_t)EEPROM_MAGIC);
    EEPROM.put(EEPROM_ADDR_CAL, jsCenter);
    EEPROM.commit();
    Serial.println("[CAL] gespeichert");
}

bool loadCalibration() {
    uint16_t magic;
    EEPROM.get(EEPROM_ADDR_MAGIC, magic);
    if (magic != EEPROM_MAGIC) return false;
    EEPROM.get(EEPROM_ADDR_CAL, jsCenter);
    Serial.printf("[CAL] geladen: %d %d %d %d\n", jsCenter[0], jsCenter[1], jsCenter[2], jsCenter[3]);
    return true;
}

void calibrate() {
    int32_t sum[4] = {};
    for (int i = 0; i < 10; i++) {
        for (int ch = 0; ch < 4; ch++)
            sum[ch] += ads.readADC_SingleEnded(ch);
        delay(20);
    }
    for (int ch = 0; ch < 4; ch++)
        jsCenter[ch] = sum[ch] / 10;
    Serial.printf("[CAL] %d %d %d %d\n", jsCenter[0], jsCenter[1], jsCenter[2], jsCenter[3]);
    saveCalibration();
}

#define JS_DEADZONE 5

int16_t mapJS(int16_t raw, int ch) {
    int32_t v = ((int32_t)raw - jsCenter[ch]) * 100 / JS_SCALE;
    if (v > 100) v = 100;
    if (v < -100) v = -100;
    if (v > -JS_DEADZONE && v < JS_DEADZONE) v = 0;
    return (int16_t)v;
}

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

void addRxLine(const String& line) {
    for (int i = 0; i < DISP_RX_LINES - 1; i++)
        memcpy(rxLines[i], rxLines[i + 1], 22);
    strncpy(rxLines[DISP_RX_LINES - 1], line.c_str(), 21);
    rxLines[DISP_RX_LINES - 1][21] = '\0';
}

void handleSerial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialBuf.length() > 0) {
                addRxLine(serialBuf);
                serialBuf = "";
            }
        } else if (serialBuf.length() < 21) {
            serialBuf += c;
        }
    }
}

void updateDisplay(int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                   bool b1, bool b2, bool b3, bool b4) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.printf("LX:%4d  LY:%4d", lx, ly);
    display.setCursor(0, 8);
    display.printf("RX:%4d  RY:%4d", rx, ry);
    display.setCursor(0, 16);
    display.printf("B1:%d B2:%d B3:%d B4:%d", b1, b2, b3, b4);

    display.drawFastHLine(0, DISP_DIVIDER_Y, 128, SSD1306_WHITE);

    for (int i = 0; i < DISP_RX_LINES; i++) {
        display.setCursor(0, DISP_RX_Y + i * 8);
        display.print(rxLines[i]);
    }

    display.display();
}

void displayCalibrating() {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 20);
    display.println("Kalibriere");
    display.setCursor(30, 40);
    display.println("...");
    display.display();
}

// Gibt true zurück wenn die FB normal bedienbar ist
bool handleState(bool b2, bool b3) {
    unsigned long now = millis();
    bool combo = b2 && b3;

    switch (state) {
        case STATE_READY:
            if (combo) {
                if (calHoldStart == 0) calHoldStart = now;
                if (now - calHoldStart >= CAL_HOLD_MS) {
                    state = STATE_CAL_PULSING;
                    stateStart = now;
                    calHoldStart = 0;
                }
            } else {
                calHoldStart = 0;
            }
            ledGreen();
            return true;

        case STATE_CAL_PULSING:
            ledPulseYellow();
            if (now - stateStart >= CAL_PULSE_MS) {
                state = STATE_CALIBRATING;
                stateStart = now;
                calDone = false;
                ledYellow();
                displayCalibrating();
            }
            return false;

        case STATE_CALIBRATING:
            if (!calDone) {
                calibrate();
                calDone = true;
            }
            ledYellow();
            if (now - stateStart >= CAL_SHOW_MS) {
                state = STATE_READY;
                ledGreen();
            }
            return false;
    }
    return true;
}

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

    if (!ads.begin(0x48))
        Serial.println("[FEHLER] ADS1115");
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
        Serial.println("[FEHLER] SSD1306");

    memset(rxLines, 0, sizeof(rxLines));

    if (!loadCalibration()) {
        Serial.println("[CAL] kein EEPROM-Wert, kalibriere...");
        calibrate();
    }

    state = STATE_READY;
    ledGreen();
}

void loop() {
    bool b1 = !digitalRead(PIN_BTN1);
    bool b2 = !digitalRead(PIN_BTN2);
    bool b3 = !digitalRead(PIN_BTN3);
    bool b4 = !digitalRead(PIN_BTN4);

    bool active = handleState(b2, b3);

    if (!active) {
        delay(100);
        return;
    }

    handleRumble();
    handleSerial();

    int16_t rawLX = ads.readADC_SingleEnded(JS_LEFT_X);
    int16_t rawLY = ads.readADC_SingleEnded(JS_LEFT_Y);
    int16_t rawRX = ads.readADC_SingleEnded(JS_RIGHT_X);
    int16_t rawRY = ads.readADC_SingleEnded(JS_RIGHT_Y);

    Serial.printf("LX:%6d | LY:%6d | RX:%6d | RY:%6d | B1:%d B2:%d B3:%d B4:%d\n",
        rawLX, rawLY, rawRX, rawRY, b1, b2, b3, b4);

    updateDisplay(mapJS(rawLX, JS_LEFT_X), mapJS(rawLY, JS_LEFT_Y),
                  mapJS(rawRX, JS_RIGHT_X), mapJS(rawRY, JS_RIGHT_Y),
                  b1, b2, b3, b4);

    delay(100);
}
