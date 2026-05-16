#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WiFi.h>
#include <espnow.h>

// --- Pins ---
#define PIN_RUMBLE  D4
#define PIN_BTN1    D5
#define PIN_BTN2    D6
#define PIN_BTN3    D7
#define PIN_BTN4    D8

// --- ADS1115 Kanäle ---
#define JS_LEFT_X   0   // A0
#define JS_LEFT_Y   1   // A1
#define JS_RIGHT_X  2   // A2
#define JS_RIGHT_Y  3   // A3

// --- OLED ---
#define OLED_WIDTH  128
#define OLED_HEIGHT 64

Adafruit_ADS1115 ads;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// --- Daten-Struct FB → Auto (noch placeholder) ---
struct FbData {
    int16_t lx;     // Linker Joystick X
    int16_t ly;     // Linker Joystick Y
    int16_t rx;     // Rechter Joystick X
    int16_t ry;     // Rechter Joystick Y
    uint8_t buttons; // Bit 0–3 = Button 1–4
};

FbData fbData;

void setup() {
    Serial.begin(115200);

    // Buttons
    pinMode(PIN_BTN1, INPUT_PULLUP);
    pinMode(PIN_BTN2, INPUT_PULLUP);
    pinMode(PIN_BTN3, INPUT_PULLUP);
    pinMode(PIN_BTN4, INPUT_PULLUP);

    // Rumble
    pinMode(PIN_RUMBLE, OUTPUT);
    analogWrite(PIN_RUMBLE, 0);

    // I2C
    Wire.begin(D2, D1); // SDA, SCL

    // ADS1115
    if (!ads.begin(0x48)) {
        Serial.println("ADS1115 nicht gefunden!");
    }

    // OLED
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 nicht gefunden!");
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Mario Kart RC");
    display.println("FB init...");
    display.display();

    // ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != 0) {
        Serial.println("ESP-NOW init fehlgeschlagen!");
    }

    Serial.println("FB bereit.");
}

void loop() {
    // Joysticks lesen
    fbData.lx = ads.readADC_SingleEnded(JS_LEFT_X);
    fbData.ly = ads.readADC_SingleEnded(JS_LEFT_Y);
    fbData.rx = ads.readADC_SingleEnded(JS_RIGHT_X);
    fbData.ry = ads.readADC_SingleEnded(JS_RIGHT_Y);

    // Buttons lesen (LOW = gedrückt wegen PULLUP)
    fbData.buttons = 0;
    if (!digitalRead(PIN_BTN1)) fbData.buttons |= (1 << 0);
    if (!digitalRead(PIN_BTN2)) fbData.buttons |= (1 << 1);
    if (!digitalRead(PIN_BTN3)) fbData.buttons |= (1 << 2);
    if (!digitalRead(PIN_BTN4)) fbData.buttons |= (1 << 3);

    // Debug Serial
    Serial.printf("LX:%5d LY:%5d RX:%5d RY:%5d BTN:%02X\n",
        fbData.lx, fbData.ly, fbData.rx, fbData.ry, fbData.buttons);

    // OLED Update
    display.clearDisplay();
    display.setCursor(0, 0);
    display.printf("LX:%5d LY:%5d\n", fbData.lx, fbData.ly);
    display.printf("RX:%5d RY:%5d\n", fbData.rx, fbData.ry);
    display.printf("BTN: %02X\n", fbData.buttons);
    display.display();

    delay(50);
}
