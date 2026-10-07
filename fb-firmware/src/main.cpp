#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "mk_protocol.h"
#include "mk_clock_guard.h"
#include "mk_watchdog.h"

// --- Pins (ESP32-C6-SuperMini, Repin 2026-09-06) ---
// Tabu: GPIO12/13 (USB D-/D+), GPIO8/9 (RGB-LED / BOOT-Strap),
//       GPIO16/17 (UART0 — TXD0 wird vom Boot-ROM aktiv getrieben; auf V2
//       traegt GPIO16 bewusst die Status-LED, ein reiner Ausgang).
// ADC1 liegt nur auf GPIO0–6, der Batterie-Monitor muss dorthin.
#define PIN_BAT_ADC      0   // ADC1_0, externer Teiler 100k/100k
#define PIN_MODE         1   // fest an 3.3V/GND — darf kein UART0-TX sein
#define PIN_BTN_RED      2
#define PIN_BTN_BLUE     3
#define PIN_BTN_GREEN    4
// FB-Board V2 (2026-10-06): Gelb von inneren Pad 23 an die Hauptleiste
// (GPIO5), Display-MOSI auf GPIO6, Status-LED auf GPIO16 (UART0 TX — der
// Boot-ROM gibt dort ~17 ms sein Startprotokoll aus, die LED blitzt beim
// Einschalten evtl. kurz in einer Zufallsfarbe, bis setup() sie setzt).
// Kein fliegender Draht mehr. env fb_v2 setzt -DFB_REV=2.
#ifndef FB_REV
#define FB_REV 1
#endif
#if FB_REV == 2
#define PIN_TFT_MOSI     6   // Display SDA, ueber R17 33 Ohm
#define PIN_LED         16   // WS2812B DATA, ueber R6 330 Ohm
#else
#define PIN_TFT_MOSI     5   // FSPI MOSI — IO_MUX, volle Taktrate
#define PIN_LED          6   // WS2812B DATA
#endif
#define PIN_TFT_SCK      7   // FSPI SCK — IO_MUX, volle Taktrate
#define PIN_TFT_BL      14   // Backlight, PWM-gedimmt
#define PIN_TFT_CS      15
// GPIO16 = UART0 TXD0 — im Layout nicht erreichbar, bleibt frei.
#define PIN_RUMBLE      17   // LEDC. GPIO17 = UART0 RXD0 — der ROM liest den Pin
                             // nur, treibt ihn nie. Kein Motorzappeln beim Boot.
#define PIN_TFT_DC      18
#define PIN_I2C_SDA     19
#define PIN_I2C_SCL     20
// GPIO21-23 liegen auf den inneren Pads des SuperMini, nicht an den
// Hauptleisten — dort ist je ein Draht noetig.
#define PIN_BTN_STICK_R 21   // reserviert, bewusst ohne Funktion
#define PIN_BTN_STICK_L 22   // reserviert, bewusst ohne Funktion
#if FB_REV == 2
#define PIN_BTN_YELLOW   5   // Hauptleiste. Strap MTDI (nur SDIO-Flanke) → folgenlos
#else
#define PIN_BTN_YELLOW  23   // inneres Pad — einziger Draht. Bewusst ein Taster
                             // und nicht MOSI: bei einem Tastersignal ist eine
                             // fliegende Verbindung unkritisch.
#endif

// --- ADS1115 Kanäle ---
// ADS1115-Kanal je Achse, am Gerät gemessen (2026-10-04, Log der Rohwerte).
// Die Belegung laut Schaltplan (A0 = links X ... A3 = rechts X) stimmte nicht
// mit der Einbaulage der Sticks überein. Alles Weitere folgt diesen vier
// Zeilen — Fahren, Menü-Navigation und auch CAL_STEP_CH.
#define JS_LEFT_X   2   // linker Stick seitlich
#define JS_LEFT_Y   3   // linker Stick vor/zurück
#define JS_RIGHT_Y  0   // rechter Stick vor/zurück
#define JS_RIGHT_X  1   // rechter Stick seitlich
// Kanäle, deren Rohwert entgegen der Erwartung läuft (rechts = kleiner Wert) —
// bei beiden Sticks die Seitwärtsachse. Gilt für Fahrbefehl und
// Menü-Navigation, nicht für die Kalibrierung.
#define JS_INVERTED(ch)  ((ch) == JS_RIGHT_X || (ch) == JS_LEFT_X)

// --- Display: 1.9" IPS 170x320 ST7789, Querformat ---
// init(170,320) laesst die Library colstart=35 / rowstart=0 rechnen — genau der
// Versatz, mit dem das 170er-Panel im 240x320-Controller sitzt.
// setRotation(1) dreht auf Querformat. Steht das Bild auf dem Kopf: 3 statt 1.
#define TFT_PANEL_W  170
#define TFT_PANEL_H  320
#define TFT_ROTATION   1
#define TFT_W        320
#define TFT_H        170

// GFX-Standardfont: 6x8 bei Groesse 1, also 12x16 bei Groesse 2.
// Groesse 2 ist die Basis — 26 Zeichen pro Zeile, 10 Zeilen.
#define CH_W          12
#define CH_H          16
#define TITLE_Y        6
#define RULE_Y        28
#define BODY_Y        40
#define LINE_H        22

// Backlight per LEDC gedimmt statt hart auf HIGH: spart Strom (das 1.9"-IPS
// zieht bei voller Helligkeit 30–60mA) und spaeter 18650-Laufzeit.
#define TFT_BL_FREQ   5000
#define TFT_BL_RES       8
#define TFT_BL_LEVEL   150   // ~60%, drinnen gut ablesbar
#define TFT_BL_DIM      10   // ~4%, nach TFT_DIM_AFTER_MS ohne Eingabe
#define TFT_DIM_AFTER_MS 60000

// Farben (RGB565)
#define COL_BG       ST77XX_BLACK
#define COL_FG       ST77XX_WHITE
#define COL_DIM      0x8410
#define COL_ACCENT   0xFD20
#define COL_OK       0x07E0
#define COL_WARN     0xFFE0
#define COL_CRIT     0xF800

// --- Rumble ---
// Auf dem C6 ist PWM Hardware (LEDC). Das Brummen beim Verbindungsverlust auf
// dem ESP8266 kam von dessen Software-PWM, die unter WLAN-Interruptlast glitchte
// — diese Fehlerklasse entfaellt hier.
#define RUMBLE_PWM          255
#define RUMBLE_PWM_FREQ   20000
#define RUMBLE_PWM_RES        8

// --- Timings ---
#define LOOP_PERIOD_MS    20     // 50 Hz Zielfrequenz
#define DISPLAY_PERIOD_MS 100   // Mindestabstand zwischen Display-Updates (Normalscreen, Teil-Updates)
#define CAL_HOLD_MS   3000
#define CAL_PULSE_MS  3000
#define CAL_SHOW_MS   3000

// --- Joystick ---
// Spannungsteiler: R_top = R_bot (beliebiger gleicher Wert 1k–100k)
// → V_out = 5V × 0.5 = 2.5V, ADS-Wert = 2.5/4.096 × 32767 ≈ 19989
// Minimum: Joystick bei GND → 0V → ADS ≈ 0
// Default-Center: halber Bereich (überschrieben durch die Offset-Kalibrierung im Menü, EEPROM)
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
#define EEPROM_ADDR_TRIM    26   // int8 — unbenutzt, der Trim gehoert dem Auto
#define EEPROM_ADDR_LANG    27   // uint8
#define EEPROM_ADDR_CHANNEL 28   // uint8, 0 = kein gespeicherter Kanal
#define EEPROM_ADDR_CARMAC  29   // 6B, MAC des zuletzt gepairten Autos (Direct Mode)
#define EEPROM_SIZE         40

// --- Trim & Speed ---
#define TRIM_STEPS  21
#define TRIM_MIN   -10
#define TRIM_MAX    10
#define SPEED_STEPS  10

// --- Akku ---
// Solange der Spannungsteiler (100k/100k an PIN_BAT_ADC) nicht verdrahtet ist,
// floatet der Pin und liefert Mist (~1.2V → 0%). Das wuerde die LED rot blinken
// lassen und den Rumble abschalten. Analog zu gBatWired in der Auto-Firmware.
// Auf true setzen, sobald der Teiler dran ist. Verdrahtet seit 2026-09-25.
#if defined(MK_TEST_BARE) || defined(MK_TEST_USB_POWER)
// Testbuilds: fb_bare (nacktes C6, GPIO0 haengt in der Luft) und fb_usbpower
// (Platine ohne Akku, Versorgung per USB — ueber den LDO rueckgespeist liegt
// das Akku-Netz bei ~2.7V). In beiden Faellen wuerde der Tiefentladeschutz
// sonst auf falsche Werte reagieren.
#define BAT_WIRED  false
#else
#define BAT_WIRED  true
#endif
// Der C6 hat keinen internen Teiler wie der ESP8266-A0. Extern 100k/100k:
// 4.2V → 2.1V, sicher unter der 3.3V-Referenz. analogReadMilliVolts() nutzt die
// werkskalibrierte ADC-Kurve, deshalb kein roher analogRead().
#define BAT_DIV_RATIO   2.0f
#define BAT_LOW_PCT   33
#define BAT_CRIT_PCT  16
#define BAT_CHECK_MS  10000
// Tiefentladeschutz: ungeschuetzte 18650. Liegt die Zelle BAT_OFF_SAMPLES
// Messungen in Folge (= 30 s) unter BAT_OFF_V, geht die FB in Deep Sleep ohne
// Weckquelle — nur Aus-/Einschalten am Schalter holt sie zurueck. Mehrere
// Messungen, damit ein kurzer Einbruch durch den Rumble nicht ausloest.
#define BAT_OFF_V       3.3f
#define BAT_OFF_SAMPLES 3

// --- Menü ---
// Menue-Eintraege ueber Namen statt Nummern — neue Eintraege verschieben sonst
// still alle Indizes dahinter.
enum MenuItem : int8_t {
    MI_CALIB, MI_SPEED, MI_LANG, MI_RUMBLE,
    MI_SWAP, MI_FIRMWARE, MI_DEBUG, MI_RESET, MI_EXIT, MI_COUNT
};
// Untermenue "Kalibrierung"
enum CalItem : int8_t { CI_OFFSET, CI_MINMAX, CI_TRIM, CI_STEERCAL, CI_COUNT };
#define MENU_ITEM_COUNT  MI_COUNT
#define MENU_VISIBLE     6

// --- Sprachen ---
// Um eine weitere Sprache hinzuzufügen:
// 1. LANG_COUNT erhöhen
// 2. Neuen Eintrag in STRINGS[] ergänzen
#define LANG_COUNT  2
#define LANG_DE     0
#define LANG_EN     1

// --- Min/Max Kalibrierungsschritte ---
// 8 Schritte: LX-min, LX-max, LY-min, LY-max, RX-min, RX-max, RY-min, RY-max
// Bewusst ueber die JS_*-Defines statt roher Kanalnummern: so folgt die
// Kalibrierung automatisch, wenn die ADS-Kanaele im Layout getauscht werden.
const uint8_t CAL_STEP_CH[8]    = {JS_LEFT_X,  JS_LEFT_X,  JS_LEFT_Y,  JS_LEFT_Y,
                                   JS_RIGHT_X, JS_RIGHT_X, JS_RIGHT_Y, JS_RIGHT_Y};
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
    const char* rejoiningSkip;      // Hinweis: Suche per Taste abbrechen
    const char* batEmpty;           // Statuszeile ab 0% (3.4V)
    const char* batOffLine1;        // Abschalt-Screen Zeile 1
    const char* batOffLine2;        // Abschalt-Screen Zeile 2
    const char* stickFault;         // Statuszeile bei I2C-Dauerstoerung
    // Auto-Update (OTA), siehe OTA-Abschnitt in mk_protocol.h
    const char* otaQuestion;
    const char* otaConfirmHint;
    const char* otaNoCar;
    const char* otaNoReply;
    const char* otaRejBat;
    const char* otaRejWifi;
    const char* otaWaiting;         // ueber dem Hostnamen
    const char* otaBack;
    const char* otaLeaveHint;
    const char* fwTargets[2];       // Untermenue Firmware-Update: Auto, Fernbedienung
    const char* fwFbLine1;          // Platzhalter FB-Update
    const char* fwFbLine2;
    // Lenkung kalibrieren
    const char* scSteps[3];         // links, rechts, geradeaus
    const char* scHint;
    const char* scSaved;
    const char* scInvalid;
    const char* calItems[CI_COUNT]; // Untermenue Kalibrierung
};

const Strings STRINGS[LANG_COUNT] = {
    // LANG_DE
    {
        "Einstellungen",
        { "Kalibrierung", "Max. Speed", "Sprache", "Rumble",
          "Joysticks tauschen", "Firmware-Update", "Debug", "Reset",
          "Speichern & beenden" },
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
        "Gruen = bestaetigen",
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
        "Verbinde mit", "letztem Spiel", "Taste = neu suchen",
        "Akku leer!",
        "Akku leer", "Bitte ausschalten",
        "Sticks gestoert!",
        "Auto updaten?", "Gruen = ja  Rot = nein",
        "Kein Auto verbunden", "Auto antwortet nicht",
        "Auto-Akku zu leer", "Auto ohne WLAN-Daten",
        "Warte auf Firmware:", "Auto wieder verbunden",
        "Rot = zurueck",
        { "Auto", "Fernbedienung" },
        "Noch nicht verfuegbar", "Bitte per USB flashen",
        { "Voll links", "Voll rechts", "Geradeaus" },
        "Gruen=ok  Rot=abbrechen",
        "Lenkung gespeichert", "Werte ungueltig",
        { "Offset (Nullpunkt)", "Min/Max (Sticks)", "Servo-Trim", "Lenkung kalibrieren" }
    },
    // LANG_EN
    {
        "Settings",
        { "Calibration", "Max. Speed", "Language", "Rumble",
          "Swap Sticks", "Firmware Update", "Debug", "Reset",
          "Save & exit" },
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
        "Green = confirm",
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
        "Reconnecting", "to last game", "Button = new search",
        "Battery empty!",
        "Battery empty", "Please switch off",
        "Stick fault!",
        "Update car?", "Green = yes  Red = no",
        "No car connected", "Car not responding",
        "Car battery too low", "Car has no WiFi data",
        "Waiting for firmware:", "Car reconnected",
        "Red = back",
        { "Car", "Remote" },
        "Not available yet", "Please flash via USB",
        { "Full left", "Full right", "Straight ahead" },
        "Green=ok  Red=cancel",
        "Steering saved", "Invalid values",
        { "Offset (center)", "Min/Max (sticks)", "Servo Trim", "Steering calibration" }
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
    STATE_DEBUG,
    STATE_CAL_MENU,      // Untermenue Kalibrierung
    STATE_SERVO_CAL,     // Lenkung kalibrieren: links, rechts, geradeaus
    STATE_INFO,          // kurze Meldung, danach Hauptmenue
    STATE_FW_MENU,       // Untermenue Firmware-Update: Auto / Fernbedienung
    STATE_OTA_CONFIRM,   // Sicherheitsabfrage
    STATE_OTA_SEND,      // Request raus, warte auf MK_OtaStatus
    STATE_OTA_WAIT,      // Auto im Update-Modus, warte auf seine Rueckkehr
    STATE_OTA_MSG,       // Meldung, danach zurueck ins Untermenue bzw. Normalbetrieb
};

// --- Globale Variablen ---
Adafruit_ADS1115  ads;
Adafruit_ST7789   display(PIN_TFT_CS, PIN_TFT_DC, -1);  // RST fest auf 3.3V
Adafruit_NeoPixel led(1, PIN_LED, NEO_GRB + NEO_KHZ800);

bool    adsOK   = false;

// ── I2C-Ueberwachung ADS1115 ──────────────────
// Am 2026-09-26 fiel der Bus zum ADS1115 in eine Dauerstoerung
// (ESP_ERR_INVALID_STATE bei jedem Zugriff) — die FB las Muell und schickte ihn
// als Lenkung ans Auto. Deshalb: vor und nach jedem Lesen pruefen, ob der ADS
// antwortet; sonst neutral senden, nach kurzer Stoerung den Bus neu aufsetzen
// und jede Stoerung ins Log schreiben.
#define I2C_REINIT_AFTER_CYCLES  10     // ~200 ms Stoerung, dann Bus neu
#define I2C_REINIT_GAP_MS       500     // Mindestabstand zwischen Neuinits
#define I2C_FAULT_AFTER_REINITS   3     // danach Hinweis auf dem Display
bool stickFault = false;                // Dauerstoerung, Anzeige in der Statuszeile

// ── Plausibilitaet der Stick-Werte (2026-10-04) ──
// Anlass: Die FB sendete Vollgas + volle Lenkung, waehrend die Sticks ruhig
// lagen, und reagierte auf keine Eingabe, bis sie aus- und eingeschaltet wurde.
// Der ADS1115 antwortete also auf seine Adresse, lieferte aber unbrauchbare
// Werte — das fing weder adsAlive() noch die Pruefung auf negative Counts ab.
// Ursache nicht bestaetigt. Im Fahrbetrieb gilt deshalb zusaetzlich als Stoerung:
//  - ein Wert deutlich ausserhalb des kalibrierten Bereichs: ein Poti kommt
//    nicht weiter als bis zu seinem Anschlag (stickInRange), und
//  - drei verschiedene Kanaele mit exakt demselben Wert: dann gibt der ADS
//    immer dasselbe Register zurueck, statt zu messen. Dafuer wird in jeder
//    Runde einer der beiden ungenutzten Kanaele mitgelesen (Loop).
// Bewusst NICHT "Wert bleibt laenger gleich": am Anschlag rauscht ein Stick
// kaum (gemessen: 8–9 Counts, bis 680 ms am Stueck identisch) — das gaebe
// Fehlalarme bei gehaltenem Vollgas.
// Folge wie bei jeder I2C-Stoerung: neutral senden, Bus neu initialisieren.
#define STICK_RANGE_MARGIN_PCT  15     // Reserve ueber den kalibrierten Anschlag hinaus
#define STICK_RANGE_MARGIN_MIN  1500   // mindestens so viele Counts Reserve

extern int16_t jsMin[4], jsMax[4];   // Kalibrierung, weiter unten definiert

bool stickInRange(uint8_t ch, int16_t v) {
    int32_t span   = (int32_t)jsMax[ch] - jsMin[ch];
    int32_t margin = max((int32_t)STICK_RANGE_MARGIN_MIN, span * STICK_RANGE_MARGIN_PCT / 100);
    if (v >= jsMin[ch] - margin && v <= jsMax[ch] + margin) return true;
    static uint32_t lastLog = 0;   // hoechstens eine Meldung pro Sekunde
    if (nowMs() - lastLog >= 1000) {
        lastLog = nowMs();
        Serial.printf("[STICK] Kanal A%u = %d ausserhalb %d..%d\n", ch, v, jsMin[ch], jsMax[ch]);
    }
    return false;
}

bool adsAlive() {
    Wire.beginTransmission(0x48);
    return Wire.endTransmission() == 0;
}

void handleI2cHealth(bool ok) {
    static uint32_t failCycles = 0, failStartMs = 0, incidents = 0, reinits = 0;
    static uint32_t lastReinitMs = 0;
    if (ok) {
        if (failCycles) {
            Serial.printf("[I2C] wieder ok nach %lu ms (%lu Runden, %lu Neuinit.)\n",
                          (unsigned long)(nowMs() - failStartMs),
                          (unsigned long)failCycles, (unsigned long)reinits);
        }
        failCycles = 0;
        reinits    = 0;
        stickFault = false;
        return;
    }
    if (failCycles++ == 0) {
        gDiag.i2c++;
        failStartMs = nowMs();
        incidents++;
        Serial.printf("[I2C] ADS1115 antwortet nicht – sende neutral (Stoerung #%lu)\n",
                      (unsigned long)incidents);
    }
    if (failCycles >= I2C_REINIT_AFTER_CYCLES && nowMs() - lastReinitMs >= I2C_REINIT_GAP_MS) {
        lastReinitMs = nowMs();
        // Bus-Freiraeumen im I2C-Treiber (s_i2c_master_clear_bus) kann laenger
        // als 50 ms dauern — gewollter Rettungsschritt, nicht ueberwachen.
        MkWdtPause wdtPause;
        reinits++;
        gDiag.i2cReinit++;
        Wire.end();
        Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    // Kurzer Timeout: eine ADS-Uebertragung dauert <1 ms. Mit dem Standard von
    // 50 ms wartete die Loop bei einer I2C-Stoerung so lange, dass der
    // Loop-Watchdog (50 ms, mk_watchdog.h) ausloeste — FB 2026-09-28.
    Wire.setTimeOut(5);
        bool back = ads.begin(0x48);
        if (back) ads.setDataRate(RATE_ADS1115_860SPS);
        Serial.printf("[I2C] Bus neu initialisiert (#%lu) – ADS %s\n",
                      (unsigned long)reinits, back ? "antwortet" : "fehlt");
        if (reinits >= I2C_FAULT_AFTER_REINITS && !stickFault) {
            stickFault = true;
            Serial.println("[I2C] Dauerstoerung – Hinweis auf dem Display");
        }
    }
}
bool    dispOK  = false;
uint8_t batPct  = 100;
float   batVolt = 0.0f;
FBMode  fbMode       = MODE_DIRECT;
bool    rumbleEnabled = true;

FBState  state        = STATE_READY;
uint8_t  langIndex    = LANG_DE;
uint8_t  maxSpeed     = SPEED_STEPS;   // temporär, nicht im EEPROM

int16_t  jsCenter[4]  = {JS_DEFAULT_CENTER, JS_DEFAULT_CENTER, JS_DEFAULT_CENTER, JS_DEFAULT_CENTER};
int16_t  jsMin[4]     = {JS_DEFAULT_MIN, JS_DEFAULT_MIN, JS_DEFAULT_MIN, JS_DEFAULT_MIN};
int16_t  jsMax[4]     = {JS_DEFAULT_MAX, JS_DEFAULT_MAX, JS_DEFAULT_MAX, JS_DEFAULT_MAX};

unsigned long stateStart   = 0;
unsigned long calHoldStart = 0;
unsigned long lastMenuMove = 0;
unsigned long lastBatCheck = 0;

bool    swapSticks    = false;
bool    calDone       = false;
int8_t  menuSel       = 0;
int8_t  menuScroll    = 0;
uint8_t calStep       = 0;

int8_t  trimTemp      = 0;
int8_t  trimOrig      = 0;   // Trim des Autos beim Betreten des Menues, fuer Rot
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
// Direct Mode: FB meldet sich nach Neustart/Funkstille selbst beim bekannten Auto
// (gespeicherter Kanal + Auto-MAC), ohne Beacon. Nach dem Boot kurz, im Betrieb
// so lange wie das Auto auf dem Kanal wartet (LINK_LOST_MS in car-firmware).
// Nach dem Boot kurz halten: das gespeicherte Auto kann seit Wochen weg sein.
// Gibt die FB zu frueh auf, ist das harmlos — das Auto faellt 5 s nach seinem
// Boot selbst auf Kanal 1 zurueck und beide koppeln dort neu. Per Taste laesst
// sich die Suche nach dem Boot sofort abbrechen (reconnectSkippable).
#define RECONNECT_BOOT_MS        5000
#define RECONNECT_LOST_MS       30000

uint8_t  realMac[6];
uint8_t  peerMac[6];
bool     paired          = false;
unsigned long lastBeacon = 0;

volatile bool pairingBeaconRx = false;
volatile bool pairingAssignRx = false;
uint8_t pairingCarMac[6];
uint8_t pairingBaseMac[6];

bool             trySavedChannel    = false;
bool             reconnecting       = false;   // Direct: gepairt, aber noch kein Feedback
unsigned long    reconnectStart     = 0;
unsigned long    reconnectLimit     = 0;
bool             reconnectSkippable = false;   // nur die Suche direkt nach dem Boot
unsigned long    savedChannelStart  = 0;
volatile uint8_t pendingChannelSave = 0;

struct FeedbackState {
    uint8_t position = 0;
    uint8_t lap      = 0;
    uint8_t lapTotal = 0;
    uint8_t item     = 0;
    uint8_t rumble   = 0;
    uint8_t carBat   = 0;
    int8_t  trim     = 0;   // Servo-Trim des Autos, nur zur Anzeige im Trim-Menue
};
volatile FeedbackState feedbackRaw;
volatile bool          feedbackNew    = false;
FeedbackState          feedback;
unsigned long          lastFeedbackMs = 0;
unsigned long          rumbleFbEnd    = 0;
// Bestaetigung beim Einschalten im Menue — eigener Timer, damit das Feedback
// des Autos (5 Hz, rumble=0 setzt rumbleFbEnd zurueck) sie nicht abwuergt.
unsigned long          rumbleConfirmEnd = 0;

volatile int8_t mappingSlot = -1;  // -1 = inaktiv, 1–8 = Slot anzeigen

// Auto-Update: Antwort des Autos aus dem Callback, Auswertung im State
volatile bool   otaStatusNew = false;
MK_OtaStatus    otaStatusRaw;
char            otaHost[sizeof(MK_OtaStatus::hostname)] = "";
bool            otaMsgToReady = false;   // STATE_OTA_MSG: danach Normalbetrieb statt Untermenue
int8_t          fwSel = 0;               // Untermenue Firmware-Update: 0 = Auto, 1 = FB

// Lenkung kalibrieren: der Lenk-Stick gibt die Geschwindigkeit vor, mit der
// sich der Servo bewegt (voll ausgelenkt SC_RATE_US_PER_S), nicht die Position.
#define SC_RATE_US_PER_S  400
int8_t   calSel = 0;     // Untermenue Kalibrierung
uint8_t  scStep = 0;
float    scUs   = 1500;
uint16_t scVals[3];

const Strings& S() { return STRINGS[langIndex]; }

// ESP-NOW-Helfer sind weiter unten definiert, displayDebug() braucht sie schon hier.
static uint8_t getChannel();
static void    getMac(uint8_t* out);

// ──────────────────────────────────────────────
// LED
// ──────────────────────────────────────────────
void setLed(uint8_t r, uint8_t g, uint8_t b) {
#ifdef MK_TEST_NO_LED
    // Testbuild: WS2812B bleibt dunkel — prueft, ob ihre PWM-Stromimpulse auf
    // der 3.3V-Schiene die C6-Fehler ausloesen. Einmal aus, danach keine Daten.
    static bool done = false;
    if (done) return;
    done = true;
    r = g = b = 0;
#endif
    led.setPixelColor(0, led.Color(r, g, b));
    led.show();
}
void ledGreen()  { setLed(0, 60, 0); }
void ledYellow() { setLed(60, 60, 0); }
void ledPulseYellow() {
    uint32_t t = (nowMs() - stateStart) % 600;
    uint8_t  v = (t < 300) ? (t * 60 / 300) : ((600 - t) * 60 / 300);
    setLed(v, v, 0);
}
// 300ms-Takt wie der Status-Blink im Auto (car-firmware, LED_STATUS)
void ledRedBlink()    { uint8_t v = ((nowMs() / 300) % 2) ? 60 : 0; setLed(v, 0, 0); }
void ledOrange()      { setLed(60, 20, 0); }
void ledPink()        { setLed(60, 0, 40); }
void ledBlue()        { setLed(0, 0, 60); }
void ledPulseOrange() { uint8_t v = ((nowMs() / 300) % 2) ? 60 : 0; setLed(v, v/3, 0); }
void ledPulseBlue()   { uint8_t v = ((nowMs() / 300) % 2) ? 60 : 0; setLed(0, 0, v); }
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
    EEPROM.put(EEPROM_ADDR_LANG,   langIndex);
    MkWdtPause wdtPause;   // Flash schreiben kann bis ~400 ms dauern
    EEPROM.commit();
}

// Min = kleinerer, Max = groesserer Rohwert, unabhaengig von der Stick-Richtung.
// Bei den invertierten Seitwaertsachsen liefert der Schritt "nach links" den
// hohen Wert — ohne das hier waeren Min und Max vertauscht und die Achse tot
// (2026-10-04). Die Richtung regelt JS_INVERTED, nicht die Kalibrierung.
void normalizeMinMax() {
    for (int i = 0; i < 4; i++) {
        if (jsMin[i] > jsMax[i]) { int16_t t = jsMin[i]; jsMin[i] = jsMax[i]; jsMax[i] = t; }
    }
}

bool loadSettings() {
    uint16_t magic;
    EEPROM.get(EEPROM_ADDR_MAGIC, magic);
    if (magic != EEPROM_MAGIC) return false;
    EEPROM.get(EEPROM_ADDR_CENTER, jsCenter);
    EEPROM.get(EEPROM_ADDR_MIN,    jsMin);
    EEPROM.get(EEPROM_ADDR_MAX,    jsMax);
    EEPROM.get(EEPROM_ADDR_LANG,   langIndex);
    normalizeMinMax();   // repariert auch vor dem Fix gespeicherte Kalibrierungen
    Serial.printf("[EEPROM] ctr:%d %d %d %d  min:%d %d %d %d  max:%d %d %d %d  lang:%d\n",
        jsCenter[0], jsCenter[1], jsCenter[2], jsCenter[3],
        jsMin[0], jsMin[1], jsMin[2], jsMin[3],
        jsMax[0], jsMax[1], jsMax[2], jsMax[3],
        langIndex);
    return true;
}

// ──────────────────────────────────────────────
// Kalibrierung
// ──────────────────────────────────────────────
void calibrateOffset() {
    if (!adsOK) return;
    MkWdtPause wdtPause;   // ~300 ms Mittelung plus EEPROM
    if (!adsAlive()) { Serial.println("[CAL-OFF] ADS antwortet nicht – abgebrochen"); return; }
    int32_t sum[4] = {};
    for (int i = 0; i < 10; i++) {
        for (int ch = 0; ch < 4; ch++)
            sum[ch] += ads.readADC_SingleEnded(ch);
        delay(20);
    }
    // Mit gestoertem Bus gelesene Werte nicht als Nullpunkt speichern
    if (!adsAlive()) { Serial.println("[CAL-OFF] ADS antwortet nicht – abgebrochen"); return; }
    for (int ch = 0; ch < 4; ch++)
        jsCenter[ch] = sum[ch] / 10;
    saveSettings();
    Serial.printf("[CAL-OFF] %d %d %d %d\n",
        jsCenter[0], jsCenter[1], jsCenter[2], jsCenter[3]);
}

// Tiefentladeschutz: alles Abschaltbare aus, Meldung, dann Deep Sleep ohne
// Weckquelle. Uebrig bleiben ein paar mA (Joystick-Potis, WS2812B-Ruhestrom,
// LDO) — das genuegt, entscheidend ist, dass nicht mehr gefahren wird.
void dispClear();
void dispCentered(const char* text, int y, uint8_t size, uint16_t col);

void fbShutdown() {
    mkWatchdogStop();   // 5 s Meldung, dann Deep Sleep — RTC-WDT darf nicht weiterlaufen
    Serial.printf("[BAT] %.2fV < %.2fV — Abschaltung (Deep Sleep)\n", batVolt, BAT_OFF_V);
    ledcWrite(PIN_RUMBLE, 0);
    setLed(0, 0, 0);
    if (dispOK) {
        dispClear();
        mkWatchdogCheckpoint(); dispCentered(S().batOffLine1, 58, 2, COL_CRIT);
        mkWatchdogCheckpoint(); dispCentered(S().batOffLine2, 90, 2, COL_FG);
    }
    delay(5000);
    if (dispOK) {
        display.enableDisplay(false);
        display.enableSleep(true);
    }
    // Ausgaenge im Deep Sleep auf LOW festhalten — sonst floaten sie und die
    // Hintergrundbeleuchtung oder der Rumble-Transistor koennten anlaufen.
    ledcDetach(PIN_TFT_BL);
    ledcDetach(PIN_RUMBLE);
    const uint8_t lowPins[] = { PIN_TFT_BL, PIN_RUMBLE, PIN_LED };
    for (uint8_t pin : lowPins) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        gpio_hold_en((gpio_num_t)pin);
    }
    Serial.flush();
    esp_deep_sleep_start();   // keine Weckquelle konfiguriert
}

void updateBattery() {
    if (!BAT_WIRED) {
        // Teiler nicht verdrahtet — volle Ladung vortaeuschen, damit LED-Zustand
        // und Rumble nicht faelschlich in den Kritisch-Fall laufen.
        batVolt = 0.0f;
        batPct  = 100;
        return;
    }
    int32_t sum = 0;
    for (int i = 0; i < 4; i++) sum += analogReadMilliVolts(PIN_BAT_ADC);
    batVolt = (sum / 4.0f) / 1000.0f * BAT_DIV_RATIO;
    // 0% = 3.4V, nicht 3.0V: darunter faellt der LDO aus der Regelung, die
    // 3.3V-Schiene sackt ab und die Joystick-Werte wandern (siehe CLAUDE.md,
    // "Akku-Abschaltschwelle 3.4V").
    batPct = (uint8_t)constrain((int)((batVolt - 3.4f) / 0.8f * 100.0f), 0, 100);
    Serial.printf("[BAT] %.2fV %d%%\n", batVolt, batPct);

    static uint8_t lowSamples = 0;
    lowSamples = (batVolt < BAT_OFF_V) ? lowSamples + 1 : 0;
    if (lowSamples >= BAT_OFF_SAMPLES) fbShutdown();
}

void resetSettings() {
    for (int i = 0; i < 4; i++) {
        jsMin[i]    = JS_DEFAULT_MIN;
        jsMax[i]    = JS_DEFAULT_MAX;
        jsCenter[i] = JS_DEFAULT_CENTER;
    }
    uint8_t zero = 0;
    uint8_t noMac[6] = {};
    EEPROM.put(EEPROM_ADDR_CHANNEL, zero);
    EEPROM.put(EEPROM_ADDR_CARMAC, noMac);
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
    if (JS_INVERTED(ch)) v = -v;   // siehe JS_INVERTED bei den Kanal-Defines
    return (int16_t)v;
}

// ──────────────────────────────────────────────
// Rumble
// ──────────────────────────────────────────────

// ──────────────────────────────────────────────
// Display-Hilfsfunktionen
// Der ST7789 zeichnet direkt ins Panel — kein Framebuffer, also kein
// clearDisplay()/display()-Paar mehr. dispClear() ersetzt beides.
// ──────────────────────────────────────────────
// false, sobald ein anderer Screen gezeichnet wurde — displayNormal() zeichnet
// sonst nur bei Aenderungen neu und liesse z.B. nach dem Reconnect den
// Verbinde-Screen stehen, weil paired dabei durchgehend true bleibt.
bool normalOnScreen = false;
bool connectingOnScreen = false;   // wie normalOnScreen, fuer displayConnecting()

void dispClear() {
    mkWatchdogCheckpoint();
    display.fillScreen(COL_BG);   // ~25 ms bei 40 MHz
    mkWatchdogCheckpoint();
    normalOnScreen = false;
    connectingOnScreen = false;
}

void dispTitle(const char* title) {
    if (!dispOK) return;
    display.setTextSize(2);
    display.setTextColor(COL_ACCENT);
    mkWatchdogCheckpoint(); display.setCursor(6, TITLE_Y);
    display.print(title);
    display.drawFastHLine(0, RULE_Y, TFT_W, COL_DIM);
    display.setTextColor(COL_FG);
}

void dispHighlight(int y, const char* text, int h = LINE_H) {
    display.fillRect(0, y, TFT_W, h, COL_ACCENT);
    display.setTextSize(2);
    display.setTextColor(COL_BG);
    mkWatchdogCheckpoint(); display.setCursor(6, y + (h - CH_H) / 2);
    display.print(text);
    display.setTextColor(COL_FG);
}

// Text horizontal zentriert bei Textgroesse size
void dispCentered(const char* text, int y, uint8_t size, uint16_t col) {
    display.setTextSize(size);
    display.setTextColor(col);
    int w = (int)strlen(text) * 6 * size;
    mkWatchdogCheckpoint(); display.setCursor((TFT_W - w) / 2, y);
    display.print(text);
}

// ──────────────────────────────────────────────
// Display: Akku-Icon (28x14px)
// Balken je 3px breit, 10px hoch, 1px Luecke; Nub 3px rechts daneben
// FB-Icon rechts oben, Auto-Icon links daneben.
// ──────────────────────────────────────────────
void drawBatteryIconBars(uint8_t bars, uint16_t x, uint16_t y, uint16_t col) {
    display.drawRect(x, y, 26, 14, col);
    display.fillRect(x + 26, y + 4, 3, 6, col);
    for (uint8_t i = 0; i < 6; i++) {
        if (i < bars)
            display.fillRect(x + 2 + i * 4, y + 2, 3, 10, col);
    }
}

void drawBatteryIcon(uint8_t pct) {
    uint8_t bars = (pct > 83) ? 6 :
                   (pct > 66) ? 5 :
                   (pct > 50) ? 4 :
                   (pct > 33) ? 3 :
                   (pct > 16) ? 2 : 1;
    uint16_t col = (pct <= BAT_CRIT_PCT) ? COL_CRIT
                 : (pct <= BAT_LOW_PCT)  ? COL_WARN : COL_OK;
    mkWatchdogCheckpoint(); drawBatteryIconBars(bars, TFT_W - 34, 6, col);
}

// ──────────────────────────────────────────────
// Display: Normalbetrieb
// ──────────────────────────────────────────────
void displayNormal(int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                   bool bYellow, bool bGreen, bool bBlue, bool bRed) {
    if (!dispOK) return;

    // Kein Framebuffer: fillScreen() ist als Schwarzblitzen sichtbar. Deshalb
    // nur beim Betreten des Screens einmal loeschen, danach jedes Feld einzeln
    // aktualisieren — Text deckend (Vorder- UND Hintergrundfarbe), damit die
    // alten Zeichen direkt ueberschrieben werden, ohne vorher schwarz zu werden.
    static unsigned long lastDisplayMs = 0;
    static uint32_t c_diag[7];
    static bool    c_bY, c_bG, c_bB, c_bR;
    static uint8_t c_batPct, c_carBat;
    static char    c_status[32];

    bool full = !normalOnScreen;
    if (!full && nowMs() - lastDisplayMs < DISPLAY_PERIOD_MS) return;
    lastDisplayMs = nowMs();

    if (full) {
        dispClear();
        normalOnScreen = true;
        display.setTextSize(1);
        display.setTextColor(COL_DIM);
        mkWatchdogCheckpoint(); display.setCursor(6, 10);
        display.print(fbMode == MODE_DIRECT ? "DIRECT" : "GAME");
        mkWatchdogCheckpoint(); display.setCursor(TFT_W - 108, 10);
        display.print("KART");
        display.drawFastHLine(0, RULE_Y, TFT_W, COL_DIM);
    }

    // Kopfzeile: Akku FB rechts, Akku Auto links daneben. Aendert sich selten,
    // die kleine Flaeche vorher zu loeschen faellt nicht auf.
    if (full || batPct != c_batPct || feedback.carBat != c_carBat) {
        c_batPct = batPct;
        c_carBat = feedback.carBat;
        display.fillRect(TFT_W - 74, 6, 72, 14, COL_BG);
        mkWatchdogCheckpoint(); drawBatteryIconBars(feedback.carBat, TFT_W - 74, 6, COL_DIM);
        mkWatchdogCheckpoint(); drawBatteryIcon(batPct);
    }

    display.setTextSize(2);
    display.setTextColor(COL_FG, COL_BG);
    // Statt der Achswerte vorerst die Ereigniszaehler seit dem Einschalten
    // (mk_clock_guard.h) — so laesst sich auch ohne USB-Log sehen, was passiert.
    uint32_t d[7] = { gDiag.upSec / 60, gDiag.clock, gDiag.tick, gDiag.wdt,
                      gDiag.panic, gDiag.i2c, gDiag.i2cReinit };
    if (full || memcmp(d, c_diag, sizeof(d)) != 0) {
        memcpy(c_diag, d, sizeof(d));
        mkWatchdogCheckpoint(); display.setCursor(6, BODY_Y);
        display.printf("%3lumin U%-3lu T%-3lu W%-3lu ", (unsigned long)d[0], (unsigned long)d[1],
                       (unsigned long)d[2], (unsigned long)d[3]);
        mkWatchdogCheckpoint(); display.setCursor(6, BODY_Y + LINE_H);
        display.printf("I2C %-4lu Neu %-3lu A%-3lu ", (unsigned long)d[5], (unsigned long)d[6],
                       (unsigned long)d[4]);
    }
    if (full || bYellow != c_bY || bGreen != c_bG || bBlue != c_bB || bRed != c_bR) {
        c_bY = bYellow; c_bG = bGreen; c_bB = bBlue; c_bR = bRed;
        mkWatchdogCheckpoint(); display.setCursor(6, BODY_Y + LINE_H * 2);
        display.printf("Y:%d G:%d B:%d R:%d", bYellow, bGreen, bBlue, bRed);
    }

    // Statuszeile: zentriert und mit wechselnder Laenge — nur bei Textwechsel
    // die Zeile loeschen und neu setzen, das passiert selten.
    char status[32];
    uint16_t statusCol;
    if (stickFault) {
        snprintf(status, sizeof(status), "%s", S().stickFault);
        statusCol = COL_CRIT;
    } else if (BAT_WIRED && batPct == 0) {
        snprintf(status, sizeof(status), "%s", S().batEmpty);
        statusCol = COL_CRIT;
    } else if (!paired) {
        snprintf(status, sizeof(status), "Suche...");
        statusCol = COL_ACCENT;
    } else if (feedback.position > 0) {
        snprintf(status, sizeof(status), "P:%d  L:%d/%d  Item:%d",
                 feedback.position, feedback.lap, feedback.lapTotal, feedback.item);
        statusCol = COL_OK;
    } else {
        snprintf(status, sizeof(status), "Verbunden");
        statusCol = COL_OK;
    }
    if (full || strcmp(status, c_status) != 0) {
        strcpy(c_status, status);
        int y = BODY_Y + LINE_H * 3 + 12;
        display.fillRect(0, y, TFT_W, CH_H, COL_BG);
        mkWatchdogCheckpoint(); dispCentered(status, y, 2, statusCol);
    }
    display.setTextColor(COL_FG);
}

// ──────────────────────────────────────────────
// Display: Verbindungsaufbau
// ──────────────────────────────────────────────
void displayConnecting() {
    if (!dispOK) return;
    // Der Inhalt aendert sich fast nie — ohne Change-Detection wurde hier alle
    // 500ms der komplette Schirm schwarz gefuellt und neu beschrieben, was als
    // Schwarzblitzen sichtbar ist. Jetzt nur noch zeichnen, wenn sich wirklich
    // etwas geaendert hat.
    static bool    c_valid = false;
    static bool    c_saved = false;
    static FBMode  c_mode  = MODE_DIRECT;
    static uint8_t c_bat   = 0xFF;

    static bool    c_skip  = false;

    bool rejoin = trySavedChannel || reconnecting;
    bool skip   = reconnecting && reconnectSkippable;
    // connectingOnScreen: zwischendurch war etwas anderes zu sehen (z.B. das
    // Menue) — dann neu zeichnen, auch wenn sich am Inhalt nichts geaendert hat.
    if (c_valid && connectingOnScreen && rejoin == c_saved && skip == c_skip
                && fbMode == c_mode && batPct == c_bat) return;
    c_valid = true;
    c_saved = rejoin;
    c_skip  = skip;
    c_mode  = fbMode;
    c_bat   = batPct;

    dispClear();
    if (rejoin) {
        mkWatchdogCheckpoint(); dispCentered(S().rejoiningLine1, 58, 2, COL_FG);
        mkWatchdogCheckpoint(); dispCentered(S().rejoiningLine2, 84, 2, COL_FG);
        if (skip) { mkWatchdogCheckpoint(); dispCentered(S().rejoiningSkip, 130, 2, COL_DIM); }
    } else {
        const char* msg = (fbMode == MODE_DIRECT) ? S().connectingDirect : S().connectingGame;
        mkWatchdogCheckpoint(); dispCentered(msg, 72, 2, COL_FG);
    }
    mkWatchdogCheckpoint(); drawBatteryIcon(batPct);
    connectingOnScreen = true;
}

void displayMapping(int8_t slot) {
    if (!dispOK) return;
    dispClear();
    mkWatchdogCheckpoint(); dispCentered("Mapping", 34, 2, COL_DIM);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", slot);
    mkWatchdogCheckpoint(); dispCentered(buf, 74, 6, COL_ACCENT);
}

// ──────────────────────────────────────────────
// Display: Menue — 6 von 9 Eintraegen sichtbar
// ──────────────────────────────────────────────
// Eine Menuezeile deckend zeichnen: Hintergrund der Zeile fuellen, dann Text.
// So aendert sich beim Scrollen nur die Zeile selbst, kein schwarzer Blitz
// ueber den ganzen Schirm (siehe ST7789 ohne Framebuffer).
void drawMenuRow(int i) {
    int y = 34 + (i - menuScroll) * LINE_H;
    char buf[32];
    const char* itemText = S().menuItems[i];
    if (i == MI_RUMBLE) {
        snprintf(buf, sizeof(buf), "Rumble: %s",
            rumbleEnabled ? S().rumbleOn : S().rumbleOff);
        itemText = buf;
    } else if (i == MI_SWAP) {
        snprintf(buf, sizeof(buf), "%s: %s",
            S().menuItems[MI_SWAP],
            swapSticks ? S().swapOn : S().swapOff);
        itemText = buf;
    }
    // 320px / 12px = 26 Zeichen, minus Rand und Scrollbar
    char truncBuf[25];
    if (strlen(itemText) > 24) {
        memcpy(truncBuf, itemText, 24);
        truncBuf[24] = '\0';
        itemText = truncBuf;
    }
    if (i == menuSel) {
        dispHighlight(y, itemText, LINE_H);   // fuellt die Zeile selbst
    } else {
        display.fillRect(0, y, TFT_W - 6, LINE_H, COL_BG);
        display.setTextSize(2);
        display.setTextColor(COL_FG);
        mkWatchdogCheckpoint(); display.setCursor(6, y + (LINE_H - CH_H) / 2);
        display.print(itemText);
    }
}

void drawMenuScrollbar() {
    const int barY = 34, barH = MENU_VISIBLE * LINE_H;
    display.fillRect(TFT_W - 5, barY, 5, barH, COL_BG);
    display.drawFastVLine(TFT_W - 3, barY, barH, COL_DIM);
    int thumbH = barH * MENU_VISIBLE / MENU_ITEM_COUNT;
    int thumbY = barY + (barH - thumbH) * menuScroll / (MENU_ITEM_COUNT - MENU_VISIBLE);
    display.fillRect(TFT_W - 5, thumbY, 4, thumbH, COL_ACCENT);
}

// Ganzes Menue — beim Betreten und beim Zurueckkehren aus einem Untermenue.
void displayMenu() {
    if (!dispOK) return;
    dispClear();
    dispTitle(S().menuTitle);
    for (int i = menuScroll; i < menuScroll + MENU_VISIBLE && i < MENU_ITEM_COUNT; i++)
        drawMenuRow(i);
    drawMenuScrollbar();
}

// Nach Auswahlwechsel: ohne Scrollen nur alte und neue Zeile, mit Scrollen
// alle sichtbaren Zeilen und die Scrollbar — jeweils ohne fillScreen().
void displayMenuMove(int oldSel, int oldScroll) {
    if (!dispOK) return;
    if (menuScroll == oldScroll) {
        drawMenuRow(oldSel);
        drawMenuRow(menuSel);
    } else {
        for (int i = menuScroll; i < menuScroll + MENU_VISIBLE && i < MENU_ITEM_COUNT; i++)
            drawMenuRow(i);
    }
    drawMenuScrollbar();   // die markierte Zeile geht ueber die volle Breite
}

// ──────────────────────────────────────────────
// Display: Offset-Kalibrierung
// ──────────────────────────────────────────────
void displayOffsetRelease() {
    if (!dispOK) return;
    dispClear();
    dispTitle(S().calOffTitle);
    mkWatchdogCheckpoint(); dispCentered(S().calOffRelease, 88, 2, COL_FG);
}
void displayOffsetDoing() {
    if (!dispOK) return;
    dispClear();
    dispTitle(S().calOffTitle);
    mkWatchdogCheckpoint(); dispCentered(S().calOffDoing, 88, 2, COL_ACCENT);
}

// ──────────────────────────────────────────────
// Display: Min/Max-Kalibrierung
// ──────────────────────────────────────────────
void displayMinMaxStep(uint8_t step) {
    if (!dispOK) return;
    dispClear();
    dispTitle(S().calMMTitle);
    mkWatchdogCheckpoint(); dispCentered(S().calMMSteps[step].line1, 48, 2, COL_FG);
    mkWatchdogCheckpoint(); dispCentered(S().calMMSteps[step].line2, 74, 2, COL_FG);

    char buf[24];
    snprintf(buf, sizeof(buf), "%s %d/8", S().calMMStep, step + 1);
    mkWatchdogCheckpoint(); dispCentered(buf, 110, 2, COL_DIM);
    mkWatchdogCheckpoint(); dispCentered(S().calMMConfirm, 140, 2, COL_ACCENT);
}

// ──────────────────────────────────────────────
// Display: Trim-Bar
// 21 Stufen als Balken; aktiv ist alles zwischen Mitte und Trimwert.
// ──────────────────────────────────────────────
// full = beim Betreten (Schirm loeschen), sonst nur Balken und Zahl deckend
// neu — kein fillScreen pro Stick-Schritt, das blitzt (ST7789 ohne Framebuffer).
void displayTrimBar(int8_t trim, bool full = true) {
    if (!dispOK) return;
    if (full) { dispClear(); dispTitle(S().trimTitle); }

    const int segW = 13, gap = 2, barH = 34, barY = 56;
    const int totalW = TRIM_STEPS * segW + (TRIM_STEPS - 1) * gap;
    const int x0 = (TFT_W - totalW) / 2;
    for (int i = 0; i < TRIM_STEPS; i++) {
        int pos = i - 10;
        bool active = (trim >= 0) ? (pos >= 0 && pos <= trim)
                                  : (pos <= 0 && pos >= trim);
        int x = x0 + i * (segW + gap);
        if (active) display.fillRect(x, barY, segW, barH, COL_ACCENT);
        else { display.fillRect(x, barY, segW, barH, COL_BG);
               display.drawRect(x, barY, segW, barH, COL_DIM); }
    }
    // Mittenmarkierung
    display.drawFastVLine(TFT_W / 2, barY - 6, 4, COL_FG);

    char buf[8];
    snprintf(buf, sizeof(buf), "%+3d", trim);   // feste Breite, deckend
    display.setTextSize(3);
    display.setTextColor(COL_FG, COL_BG);
    mkWatchdogCheckpoint(); display.setCursor((TFT_W - 3 * 18) / 2, 112);
    display.print(buf);
    display.setTextColor(COL_FG);
}

// ──────────────────────────────────────────────
// Display: Speed-Bar
// ──────────────────────────────────────────────
void displaySpeedBar(uint8_t speed, bool full = true) {   // full: wie displayTrimBar
    if (!dispOK) return;
    if (full) { dispClear(); dispTitle(S().speedTitle); }

    const int segW = 26, gap = 4, barH = 34, barY = 56;
    const int totalW = SPEED_STEPS * segW + (SPEED_STEPS - 1) * gap;
    const int x0 = (TFT_W - totalW) / 2;
    for (int i = 1; i <= SPEED_STEPS; i++) {
        int x = x0 + (i - 1) * (segW + gap);
        if (i <= (int)speed) display.fillRect(x, barY, segW, barH, COL_ACCENT);
        else { display.fillRect(x, barY, segW, barH, COL_BG);
               display.drawRect(x, barY, segW, barH, COL_DIM); }
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%3d%%", speed * 10);   // feste Breite, deckend
    display.setTextSize(3);
    display.setTextColor(COL_FG, COL_BG);
    mkWatchdogCheckpoint(); display.setCursor((TFT_W - 4 * 18) / 2, 112);
    display.print(buf);
    display.setTextColor(COL_FG);
}

// ──────────────────────────────────────────────
// Display: Reset-Countdown
// ──────────────────────────────────────────────
void displayResetCountdown(int secs) {
    if (!dispOK) return;
    // Wird einmal pro Sekunde aufgerufen. Nur beim ersten Mal komplett loeschen,
    // danach reicht das Ziffernfeld — sonst blitzt der Schirm im Sekundentakt.
    static int c_last = -1;
    if (c_last < 0 || secs > c_last) {
        dispClear();
        mkWatchdogCheckpoint(); dispCentered("Reset in", 30, 2, COL_FG);
    } else {
        display.fillRect(0, 74, TFT_W, 48, COL_BG);
    }
    c_last = secs;
    char buf[8];
    snprintf(buf, sizeof(buf), "%ds", secs);
    mkWatchdogCheckpoint(); dispCentered(buf, 74, 6, COL_CRIT);
}

// ──────────────────────────────────────────────
// Display: Reset-Bestaetigung
// ──────────────────────────────────────────────
void displayReset() {
    if (!dispOK) return;
    dispClear();
    dispTitle(S().resetTitle);
    mkWatchdogCheckpoint(); dispCentered(S().resetDone, 88, 2, COL_FG);
}

// ──────────────────────────────────────────────
// Display: Auto-Update
// ──────────────────────────────────────────────
// Untermenue Firmware-Update — zwei Zeilen wie im Hauptmenue. full = mit
// Schirm loeschen (beim Betreten), sonst nur die beiden Zeilen neu.
void displaySubMenu(const char* title, const char* const* items, int count, int sel, bool full) {
    if (!dispOK) return;
    if (full) { dispClear(); dispTitle(title); }
    for (int i = 0; i < count; i++) {
        int y = 34 + i * LINE_H;
        if (i == sel) {
            dispHighlight(y, items[i], LINE_H);
        } else {
            display.fillRect(0, y, TFT_W, LINE_H, COL_BG);
            display.setTextSize(2);
            display.setTextColor(COL_FG);
            mkWatchdogCheckpoint(); display.setCursor(6, y + (LINE_H - CH_H) / 2);
            display.print(items[i]);
        }
    }
}

void displayFwMenu(bool full) {
    displaySubMenu(S().menuItems[MI_FIRMWARE], S().fwTargets, 2, fwSel, full);
}

void displayCalMenu(bool full) {
    displaySubMenu(S().menuItems[MI_CALIB], S().calItems, CI_COUNT, calSel, full);
}

// Aktionen der Kalibrierung kehren ins Untermenue zurueck, nicht ins Hauptmenue.
void backToCalMenu() {
    state = STATE_CAL_MENU;
    displayCalMenu(true);
}

void displayOtaMsg2(const char* l1, const char* l2) {
    if (!dispOK) return;
    dispClear();
    dispTitle(S().menuItems[MI_FIRMWARE]);
    mkWatchdogCheckpoint(); dispCentered(l1, 72, 2, COL_FG);
    mkWatchdogCheckpoint(); dispCentered(l2, 100, 2, COL_DIM);
}

// Kurze Meldung mit Titel — STATE_INFO kehrt danach ins Hauptmenue zurueck.
void displayInfo(const char* title, const char* line) {
    if (!dispOK) return;
    dispClear();
    dispTitle(title);
    mkWatchdogCheckpoint(); dispCentered(line, 80, 2, COL_FG);
}

// Lenkung kalibrieren: Schritt-Text fest, darunter die aktuelle Pulsbreite —
// nur die Zahl wird deckend ueberschrieben.
void displayServoCal(bool full) {
    if (!dispOK) return;
    if (full) {
        dispClear();
        dispTitle(S().calItems[CI_STEERCAL]);
        mkWatchdogCheckpoint(); dispCentered(S().scSteps[scStep], 48, 2, COL_FG);
        mkWatchdogCheckpoint(); dispCentered(S().scHint, 146, 2, COL_DIM);
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%4u us", (unsigned)scUs);
    display.setTextSize(3);
    display.setTextColor(COL_ACCENT, COL_BG);
    mkWatchdogCheckpoint(); display.setCursor((TFT_W - 7 * 18) / 2, 90);
    display.print(buf);
    display.setTextColor(COL_FG);
}

void displayOtaConfirm() {
    if (!dispOK) return;
    dispClear();
    dispTitle(S().menuItems[MI_FIRMWARE]);
    mkWatchdogCheckpoint(); dispCentered(S().otaQuestion, 70, 2, COL_FG);
    mkWatchdogCheckpoint(); dispCentered(S().otaConfirmHint, 120, 2, COL_DIM);
}

void displayOtaMsg(const char* msg) {
    if (!dispOK) return;
    dispClear();
    dispTitle(S().menuItems[MI_FIRMWARE]);
    mkWatchdogCheckpoint(); dispCentered(msg, 88, 2, COL_FG);
}

// Hostname fest, darunter die Wartezeit — nur die Zeit wird pro Sekunde
// deckend ueberschrieben, kein Neuzeichnen des ganzen Schirms (Flackern).
void displayOtaWait(bool full, uint32_t secs) {
    if (!dispOK) return;
    if (full) {
        dispClear();
        dispTitle(S().menuItems[MI_FIRMWARE]);
        mkWatchdogCheckpoint(); dispCentered(S().otaWaiting, 44, 2, COL_FG);
        mkWatchdogCheckpoint(); dispCentered(otaHost, 70, 2, COL_ACCENT);
        mkWatchdogCheckpoint(); dispCentered(S().otaLeaveHint, 146, 2, COL_DIM);
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    display.setTextSize(3);
    display.setTextColor(COL_FG, COL_BG);
    mkWatchdogCheckpoint(); display.setCursor((TFT_W - 4 * 18) / 2, 104);
    display.print(buf);
    display.setTextColor(COL_FG);
}

// ──────────────────────────────────────────────
// Display: Sprachauswahl
// ──────────────────────────────────────────────
void displayLanguage(uint8_t sel) {
    if (!dispOK) return;
    dispClear();
    dispTitle(S().langTitle);
    for (int i = 0; i < LANG_COUNT; i++) {
        int y = 48 + i * (LINE_H + 10);
        if (i == (int)sel) {
            dispHighlight(y, S().langNames[i]);
        } else {
            display.setTextSize(2);
            display.setTextColor(COL_FG);
            mkWatchdogCheckpoint(); display.setCursor(6, y + (LINE_H - CH_H) / 2);
            display.print(S().langNames[i]);
        }
    }
}

// ──────────────────────────────────────────────
// Joystick-Richtung für Menünavigation (Rohwerte)
// ──────────────────────────────────────────────
int8_t jsMenuY(int16_t rawLY, int16_t rawRY) {
    unsigned long now = nowMs();
    if (now - lastMenuMove < MENU_COOLDOWN) return 0;
    if (max(rawLY, rawRY) > JS_MENU_HIGH) { lastMenuMove = now; return  1; }
    if (min(rawLY, rawRY) < JS_MENU_LOW)  { lastMenuMove = now; return -1; }
    return 0;
}

int8_t jsMenuX(int16_t rawLX, int16_t rawRX) {
    unsigned long now = nowMs();
    if (now - lastMenuMove < MENU_COOLDOWN) return 0;
    // Invertierte Kanäle um die Mitte spiegeln, damit rechts immer +1 ist
    if (JS_INVERTED(JS_LEFT_X))  rawLX = 2 * JS_DEFAULT_CENTER - rawLX;
    if (JS_INVERTED(JS_RIGHT_X)) rawRX = 2 * JS_DEFAULT_CENTER - rawRX;
    if (max(rawLX, rawRX) > JS_MENU_HIGH) { lastMenuMove = now; return  1; }
    if (min(rawLX, rawRX) < JS_MENU_LOW)  { lastMenuMove = now; return -1; }
    return 0;
}

void sendConfigPacket(int8_t trim, bool save);  // forward declaration (definiert im ESP-NOW-Block)
void sendOtaRequest();
void sendServoCal(uint8_t cmd, const uint16_t* us);

// ──────────────────────────────────────────────
// Display: Debug
// ──────────────────────────────────────────────
bool debugNeedsClear = true;

void displayDebug(int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                  bool bY, bool bG, bool bB, bool bR) {
    if (!dispOK) return;
    static unsigned long lastDebugMs = 0;
    if (nowMs() - lastDebugMs < 200) return;
    lastDebugMs = nowMs();

    // Die Debugwerte aendern sich staendig, Change-Detection bringt hier nichts.
    // Stattdessen: nur beim Betreten einmal loeschen, danach mit deckendem Text
    // (Vorder- UND Hintergrundfarbe) die alten Zeichen direkt ueberschreiben.
    // Ohne das fuellt jeder Frame den Schirm schwarz — 5x pro Sekunde sichtbar.
    // debugNeedsClear setzt der Menuepunkt bei JEDEM Betreten — vorher merkte
    // sich die Funktion das selbst und loeschte nur beim ersten Mal, ab dem
    // zweiten blieb der Menue-Balken stehen.
    if (debugNeedsClear) {
        debugNeedsClear = false;
        dispClear();
        dispTitle("Debug");
    }

    display.setTextSize(2);
    display.setTextColor(COL_FG, COL_BG);
    mkWatchdogCheckpoint(); display.setCursor(6, BODY_Y);              display.printf("LX:%4d  LY:%4d ", lx, ly);
    mkWatchdogCheckpoint(); display.setCursor(6, BODY_Y + LINE_H);     display.printf("RX:%4d  RY:%4d ", rx, ry);
    mkWatchdogCheckpoint(); display.setCursor(6, BODY_Y + LINE_H * 2); display.printf("Y:%d G:%d B:%d R:%d ", bY, bG, bB, bR);
    mkWatchdogCheckpoint(); display.setCursor(6, BODY_Y + LINE_H * 3); display.printf("%.2fV %3d%% %s Ch:%-2d ",
        batVolt, batPct,
        fbMode == MODE_DIRECT ? "D" : "G",
        (int)getChannel());
    uint8_t curMac[6];
    getMac(curMac);
    mkWatchdogCheckpoint(); display.setCursor(6, BODY_Y + LINE_H * 4);
    display.printf("%02X:%02X:%02X:%02X:%02X:%02X",
        curMac[0], curMac[1], curMac[2], curMac[3], curMac[4], curMac[5]);
    display.setTextColor(COL_FG);
}

// ──────────────────────────────────────────────
// State Machine
// *P = Tastendruck (Flanke); bGreen/bBlue zusätzlich als Rohzustand für Combo
// ──────────────────────────────────────────────
bool handleState(bool bYellowP, bool bGreenP, bool bBlueP, bool bRedP,
                 bool bGreen, bool bBlue,
                 int16_t rawLX, int16_t rawLY, int16_t rawRX, int16_t rawRY) {
    unsigned long now = nowMs();

    switch (state) {

        case STATE_READY:
            if (bGreen && bBlue) {
                if (calHoldStart == 0) calHoldStart = now;
                if (now - calHoldStart >= CAL_HOLD_MS) {
                    state = STATE_MENU;
                    menuSel = 0;
                    menuScroll = 0;
                    calHoldStart = 0;
                    ledPink();
                    displayMenu();
                    return false;
                }
            } else {
                calHoldStart = 0;
            }
            if (paired && !reconnecting) ledReady();
            return true;

        case STATE_MENU: {
            ledPink();
            int8_t dir = jsMenuY(rawLY, rawRY);
            if (dir) {
                int oldSel = menuSel, oldScroll = menuScroll;
                menuSel = constrain(menuSel + dir, 0, MENU_ITEM_COUNT - 1);
                if (menuSel < menuScroll) menuScroll = menuSel;
                if (menuSel >= menuScroll + MENU_VISIBLE) menuScroll = menuSel - MENU_VISIBLE + 1;
                if (menuSel != oldSel) displayMenuMove(oldSel, oldScroll);
            }
            if (bGreenP) {
                switch (menuSel) {
                    case MI_CALIB: // Untermenue Kalibrierung
                        calSel = 0;
                        state = STATE_CAL_MENU;
                        displayCalMenu(true);
                        break;
                    case MI_SPEED: // Speed
                        speedTemp = maxSpeed;
                        state = STATE_SPEED;
                        displaySpeedBar(speedTemp);
                        break;
                    case MI_LANG: // Sprache
                        langTemp = langIndex;
                        state = STATE_LANGUAGE;
                        displayLanguage(langTemp);
                        break;
                    case MI_RUMBLE: // Rumble toggle (temporär, kein EEPROM)
                        rumbleEnabled = !rumbleEnabled;
                        if (rumbleEnabled) rumbleConfirmEnd = nowMs() + 5000;
                        if (dispOK) { drawMenuRow(menuSel); drawMenuScrollbar(); }
                        break;
                    case MI_SWAP: // Joysticks tauschen (temporär, kein EEPROM)
                        swapSticks = !swapSticks;
                        if (dispOK) { drawMenuRow(menuSel); drawMenuScrollbar(); }
                        break;
                    case MI_FIRMWARE: // Firmware-Update → Untermenue Auto / Fernbedienung
                        fwSel = 0;
                        state = STATE_FW_MENU;
                        displayFwMenu(true);
                        break;
                    case MI_DEBUG: // Debug
                        debugNeedsClear = true;
                        state = STATE_DEBUG;
                        break;
                    case MI_RESET: // Reset
                        resetSettings();
                        state = STATE_RESET;
                        stateStart = now;
                        displayReset();
                        break;
                    case MI_EXIT: // Speichern & beenden — wie Rot. Gespeichert ist
                             // ohnehin schon bei jeder Aenderung; der Eintrag
                             // ist der sichtbare Ausgang fuer alle, die Rot
                             // nicht trauen.
                        state = STATE_READY;
                        ledReady();
                        return true;
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
            if (now - stateStart >= CAL_SHOW_MS) backToCalMenu();
            return false;

        case STATE_CAL_MINMAX:
            ledYellow();
            if (bGreenP) {
                // Nach ADS-Kanal einsortieren — CAL_STEP_CH liefert Kanalnummern,
                // nicht die Reihenfolge LX/LY/RX/RY der Variablen.
                int16_t raw[4];
                raw[JS_LEFT_X] = rawLX;  raw[JS_LEFT_Y] = rawLY;
                raw[JS_RIGHT_X] = rawRX; raw[JS_RIGHT_Y] = rawRY;
                int ch = CAL_STEP_CH[calStep];
                if (CAL_STEP_ISMAX[calStep])
                    jsMaxTemp[ch] = raw[ch];
                else
                    jsMinTemp[ch] = raw[ch];
                calStep++;
                if (calStep >= 8) {
                    memcpy(jsMin, jsMinTemp, sizeof(jsMin));
                    memcpy(jsMax, jsMaxTemp, sizeof(jsMax));
                    normalizeMinMax();
                    saveSettings();
                    Serial.printf("[CAL-MM] min:%d %d %d %d  max:%d %d %d %d\n",
                        jsMin[0],jsMin[1],jsMin[2],jsMin[3],
                        jsMax[0],jsMax[1],jsMax[2],jsMax[3]);
                    backToCalMenu();
                } else {
                    displayMinMaxStep(calStep);
                }
            }
            if (bRedP) backToCalMenu();
            return false;

        case STATE_TRIM: {
            int8_t dir = jsMenuX(rawLX, rawRX);
            if (dir) {
                trimTemp = constrain(trimTemp + dir, TRIM_MIN, TRIM_MAX);
                displayTrimBar(trimTemp, false);
                sendConfigPacket(trimTemp, false);   // Vorschau: Raeder bewegen sich sofort
            }
            if (bGreenP) { sendConfigPacket(trimTemp, true);  feedback.trim = trimTemp; backToCalMenu(); }
            if (bRedP)   { sendConfigPacket(trimOrig, false); backToCalMenu(); }
            return false;
        }

        case STATE_SPEED: {
            int8_t dir = jsMenuX(rawLX, rawRX);
            if (dir) {
                speedTemp = constrain((int)speedTemp + dir, 1, SPEED_STEPS);
                displaySpeedBar(speedTemp, false);
            }
            if (bGreenP) { maxSpeed = speedTemp; state = STATE_MENU; displayMenu(); }
            if (bRedP) { state = STATE_MENU; displayMenu(); }
            return false;
        }

        case STATE_RESET:
            if (now - stateStart >= CAL_SHOW_MS) {
                state = STATE_MENU;   // jede Menue-Aktion endet im Hauptmenue
                displayMenu();
            }
            return false;

        case STATE_LANGUAGE: {
            int8_t dir = jsMenuY(rawLY, rawRY);
            if (dir) {
                langTemp = constrain((int)langTemp + dir, 0, LANG_COUNT - 1);
                displayLanguage(langTemp);
            }
            if (bGreenP) { langIndex = langTemp; saveSettings(); state = STATE_MENU; displayMenu(); }
            if (bRedP) { state = STATE_MENU; displayMenu(); }
            return false;
        }

        case STATE_DEBUG:
            if (bRedP) { state = STATE_MENU; displayMenu(); }
            return false;

        // ── Auto-Update ─────────────────────────────────────────────────────
        // Waehrend aller OTA-States laeuft der Keepalive der Loop weiter
        // (neutrale MSG_CONTROL). Das Auto hoert ihn nach seinem Neustart
        // innerhalb seiner 5 s und bleibt dieser FB zugeordnet.
        case STATE_CAL_MENU: {
            ledPink();
            int8_t dir = jsMenuY(rawLY, rawRY);
            if (dir) {
                int8_t n = constrain(calSel + dir, 0, CI_COUNT - 1);
                if (n != calSel) { calSel = n; displayCalMenu(false); }
            }
            if (bGreenP) {
                switch (calSel) {
                    case CI_OFFSET: // Offset-Kalibrierung
                        state = STATE_CAL_OFFSET_PULSE;
                        stateStart = now;
                        displayOffsetRelease();
                        break;
                    case CI_MINMAX: // Min/Max-Kalibrierung
                        memcpy(jsMinTemp, jsMin, sizeof(jsMin));
                        memcpy(jsMaxTemp, jsMax, sizeof(jsMax));
                        calStep = 0;
                        state = STATE_CAL_MINMAX;
                        displayMinMaxStep(0);
                        break;
                    case CI_TRIM: // Trim
                        // Startwert ist der echte Trim des Autos aus dem Feedback
                        trimOrig = feedback.trim;
                        trimTemp = trimOrig;
                        state = STATE_TRIM;
                        displayTrimBar(trimTemp);
                        break;
                    case CI_STEERCAL: // Lenkung kalibrieren
                        if (!paired || reconnecting) {
                            state = STATE_INFO;
                            stateStart = now;
                            displayInfo(S().calItems[CI_STEERCAL], S().otaNoCar);
                        } else {
                            scStep = 0;
                            scUs   = 1500;
                            state  = STATE_SERVO_CAL;
                            displayServoCal(true);
                        }
                        break;
                }
            }
            if (bRedP) { state = STATE_MENU; displayMenu(); }
            return false;
        }

        case STATE_SERVO_CAL: {
            ledYellow();
            // Lenk-Stick wie beim Fahren (je nach "Joysticks tauschen")
            int16_t m = swapSticks ? mapJS(rawLX, JS_LEFT_X) : mapJS(rawRX, JS_RIGHT_X);
            float before = scUs;
            scUs += (float)m * SC_RATE_US_PER_S / 100.0f * LOOP_PERIOD_MS / 1000.0f;
            scUs = constrain(scUs, (float)MK_SERVO_HARD_MIN, (float)MK_SERVO_HARD_MAX);
            uint16_t us = (uint16_t)scUs;
            sendServoCal(SERVO_CAL_PREVIEW, &us);   // jede Runde, das Auto hat 300 ms Timeout
            if ((uint16_t)before != us) displayServoCal(false);
            if (bGreenP) {
                // Schritte: links, rechts, geradeaus — gespeichert wird aber in
                // der Protokoll-Reihenfolge links, mitte, rechts (MK_ServoCal.us)
                static const uint8_t SC_SLOT[3] = {0, 2, 1};
                scVals[SC_SLOT[scStep++]] = us;
                if (scStep < 3) {
                    displayServoCal(true);
                } else {
                    uint16_t lo = min(scVals[0], scVals[2]), hi = max(scVals[0], scVals[2]);
                    bool ok = hi - lo >= MK_SERVO_MIN_SPAN && scVals[1] > lo && scVals[1] < hi;
                    if (ok) {
                        for (int i = 0; i < 3; i++) sendServoCal(SERVO_CAL_SAVE, scVals);
                        feedback.trim = 0;   // das Auto setzt den Trim beim Speichern zurueck
                    } else {
                        sendServoCal(SERVO_CAL_CANCEL, scVals);
                    }
                    Serial.printf("[SERVO] links %u  mitte %u  rechts %u µs → %s\n",
                                  scVals[0], scVals[1], scVals[2], ok ? "gespeichert" : "ungueltig");
                    state = STATE_INFO;
                    stateStart = now;
                    displayInfo(S().calItems[CI_STEERCAL], ok ? S().scSaved : S().scInvalid);
                }
            }
            if (bRedP) {
                sendServoCal(SERVO_CAL_CANCEL, &us);
                backToCalMenu();
            }
            return false;
        }

        case STATE_INFO:   // nur von "Lenkung kalibrieren" genutzt
            if (now - stateStart >= 2500) backToCalMenu();
            return false;

        case STATE_FW_MENU: {
            ledPink();
            int8_t dir = jsMenuY(rawLY, rawRY);
            if (dir) {
                int8_t n = constrain(fwSel + dir, 0, 1);
                if (n != fwSel) { fwSel = n; displayFwMenu(false); }
            }
            if (bGreenP) {
                otaMsgToReady = false;
                if (fwSel == 1) {
                    // Fernbedienung: noch Platzhalter, Update per USB
                    state = STATE_OTA_MSG;
                    stateStart = now;
                    displayOtaMsg2(S().fwFbLine1, S().fwFbLine2);
                } else if (fbMode != MODE_DIRECT || !paired || reconnecting) {
                    // Auto-Update nur im Direct Mode, das Auto muss antworten
                    state = STATE_OTA_MSG;
                    stateStart = now;
                    displayOtaMsg(S().otaNoCar);
                } else {
                    state = STATE_OTA_CONFIRM;
                    displayOtaConfirm();
                }
            }
            if (bRedP) { state = STATE_MENU; displayMenu(); }
            return false;
        }

        case STATE_OTA_CONFIRM:
            ledPink();
            if (bGreenP) {
                otaStatusNew = false;
                sendOtaRequest();
                state = STATE_OTA_SEND;
                stateStart = now;
            }
            if (bRedP) { state = STATE_FW_MENU; displayFwMenu(true); }
            return false;

        case STATE_OTA_SEND: {
            ledPink();
            const char* msg = nullptr;
            if (otaStatusNew) {
                otaStatusNew = false;
                if (otaStatusRaw.state == OTA_ACCEPTED) {
                    memcpy(otaHost, (const char*)otaStatusRaw.hostname, sizeof(otaHost));
                    otaHost[sizeof(otaHost) - 1] = '\0';
                    Serial.printf("[OTA] Auto im Update-Modus: %s.local\n", otaHost);
                    // Funkstille des Autos ist jetzt gewollt: statt 30 s bis
                    // MK_OTA_PARTNER_MS warten, erst dann gilt es als verloren.
                    reconnecting       = true;
                    reconnectStart     = now;
                    reconnectLimit     = MK_OTA_PARTNER_MS;
                    reconnectSkippable = false;
                    state = STATE_OTA_WAIT;
                    stateStart = now;
                    displayOtaWait(true, 0);
                    return false;
                }
                msg = (otaStatusRaw.state == OTA_REJ_BAT) ? S().otaRejBat : S().otaRejWifi;
            } else if (now - stateStart > 1500) {
                msg = S().otaNoReply;
            }
            if (msg) {
                otaMsgToReady = false;
                state = STATE_OTA_MSG;
                stateStart = now;
                displayOtaMsg(msg);
            }
            return false;
        }

        case STATE_OTA_WAIT: {
            ledPink();
            // Das Auto schickt nach der Zusage evtl. noch ein letztes Feedback —
            // das ist nicht die Rueckkehr. Neustart + WLAN dauern ohnehin laenger.
            if (!reconnecting) {
                if (now - stateStart < 5000) {
                    reconnecting = true;
                } else {
                    Serial.println("[OTA] Auto wieder verbunden");
                    otaMsgToReady = true;
                    state = STATE_OTA_MSG;
                    stateStart = now;
                    displayOtaMsg(S().otaBack);
                    return false;
                }
            }
            static uint32_t lastSecs = 0;
            uint32_t secs = (now - stateStart) / 1000;
            if (secs != lastSecs) { lastSecs = secs; displayOtaWait(false, secs); }
            // Zurueck ins Menue: das Update laeuft weiter, die lange Wartezeit
            // bleibt aktiv, bis das Auto sich meldet. Erst nach den 5 s oben,
            // sonst koennte ein spaetes Feedback die Wartezeit beenden.
            if (bRedP && now - stateStart >= 5000) { state = STATE_MENU; displayMenu(); }
            return false;
        }

        case STATE_OTA_MSG:
            if (now - stateStart >= 2500) {
                if (otaMsgToReady) { state = STATE_MENU; displayMenu(); return false; }
                state = STATE_FW_MENU;
                displayFwMenu(true);
            }
            return false;
    }
    return true;
}

// ──────────────────────────────────────────────
// ESP-NOW
// ──────────────────────────────────────────────
static void setChannel(uint8_t ch) {
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static uint8_t getChannel() {
    uint8_t pri = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&pri, &sec);
    return pri;
}

static void setMac(const uint8_t* mac) {
    esp_wifi_set_mac(WIFI_IF_STA, (uint8_t*)mac);
}

static void getMac(uint8_t* out) {
    esp_wifi_get_mac(WIFI_IF_STA, out);
}

static void addPeer(const uint8_t* mac) {
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;      // 0 = Home-Channel verwenden, kein Mismatch moeglich
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(mac)) esp_now_add_peer(&peer);
}

// Suchzustand vollstaendig wiederherstellen.
// Ohne das hing die FB nach einem Verbindungsverlust auf "warte auf Auto":
// sie blieb auf dem Betriebskanal (das Auto beacont nach seinem Neustart aber
// auf Kanal 1) UND hatte ihre echte MAC statt der gespooften MK_BASE_MAC — sie
// konnte die Beacons also aus zwei Gruenden nicht hoeren.
static void enterSearchState() {
    paired          = false;
    trySavedChannel = false;
    pairingBeaconRx = false;
    pairingAssignRx = false;
    feedback        = FeedbackState{};
    rumbleFbEnd     = 0;

    uint8_t zero = 0;
    uint8_t noMac[6] = {};
    EEPROM.put(EEPROM_ADDR_CHANNEL, zero);
    EEPROM.put(EEPROM_ADDR_CARMAC, noMac);
    { MkWdtPause wdtPause; EEPROM.commit(); }
    reconnecting = false;

    if (fbMode == MODE_DIRECT) {
        if (esp_now_is_peer_exist(peerMac)) esp_now_del_peer(peerMac);
        uint8_t fakeMac[] = MK_BASE_MAC;
        setMac(fakeMac);
    }
    setChannel(MK_ESPNOW_CHANNEL);

    lastFeedbackMs = nowMs();
    stateStart     = nowMs();
    Serial.println("[ESPNOW] Zurueck in Suchzustand (Kanal 1, MAC gespooft)");
}

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    const uint8_t* senderMac = info->src_addr;
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
        feedbackRaw.position = fb->position;
        feedbackRaw.lap      = fb->lap;
        feedbackRaw.lapTotal = fb->lapTotal;
        feedbackRaw.item     = fb->item;
        feedbackRaw.rumble   = fb->rumble;
        feedbackRaw.carBat   = fb->carBat;
        feedbackRaw.trim     = fb->trim;
        feedbackNew = true;
    } else if (msgType == MSG_CHANNEL_SWITCH && len >= (int)sizeof(MK_ChannelSwitch)) {
        uint8_t ch = ((const MK_ChannelSwitch*)data)->channel;
        setChannel(ch);
        pendingChannelSave = ch;
    } else if (msgType == MSG_MAPPING && len >= (int)sizeof(MK_Mapping)) {
        mappingSlot = ((const MK_Mapping*)data)->slot;
    } else if (msgType == MSG_OTA_STATUS && len >= (int)sizeof(MK_OtaStatus)
               && memcmp(senderMac, peerMac, 6) == 0) {
        memcpy(&otaStatusRaw, data, sizeof(MK_OtaStatus));
        otaStatusNew = true;
    }
}

void onEspNowSent(const esp_now_send_info_t*, esp_now_send_status_t);

void initEspNow() {
    // Vor dem Funkstart: die Bibliothek setzt beim STA_START-Ereignis den
    // Stromsparmodus selbst auf WiFi.getSleep() — kaeme das nach unserem
    // esp_wifi_set_ps() unten an, waere Modem-Sleep doch wieder an.
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    // Kein Modem-Sleep: mit dem SYSTIMER-Fehler (mk_clock_guard.h) kann der
    // Funk-Stack sonst in einen Watchdog-Absturz laufen. Ohne Router-Verbindung
    // spart der Modus ohnehin kaum etwas.
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.disconnect();
    WiFi.macAddress(realMac);
    uint8_t savedCh = 0;
    EEPROM.get(EEPROM_ADDR_CHANNEL, savedCh);
    uint8_t carMac[6];
    EEPROM.get(EEPROM_ADDR_CARMAC, carMac);
    bool carMacValid = false;
    for (int i = 0; i < 6; i++)
        if (carMac[i] != 0x00 && carMac[i] != 0xFF) carMacValid = true;

    // Direct-Reconnect: die FB ist Master und kennt Kanal und Auto aus dem
    // EEPROM. Sie spricht das Auto direkt mit ihrer echten MAC an und sendet
    // Steuerpakete — kein Beacon, kein Assign, kein Umweg ueber Kanal 1 (dort
    // gewinnt die erste FB, die Zuordnung koennte wechseln). Kommt Feedback,
    // steht die Verbindung; sonst nach RECONNECT_BOOT_MS Rueckfall auf Kanal 1.
    if (fbMode == MODE_DIRECT && savedCh != MK_ESPNOW_CHANNEL
            && savedCh >= 1 && savedCh <= 13 && carMacValid) {
        setChannel(savedCh);
        if (esp_now_init() != ESP_OK) { Serial.println("[ESPNOW] Init fehlgeschlagen"); return; }
        esp_now_register_recv_cb(onDataRecv);
        esp_now_register_send_cb(onEspNowSent);
        memcpy(peerMac, carMac, 6);
        addPeer(peerMac);
        paired         = true;
        reconnecting   = true;
        reconnectStart = nowMs();
        reconnectLimit = RECONNECT_BOOT_MS;
        reconnectSkippable = true;
        lastFeedbackMs = nowMs();
        Serial.printf("[ESPNOW] Direct: melde mich bei %02X:%02X:%02X:%02X:%02X:%02X auf Kanal %d\n",
            carMac[0], carMac[1], carMac[2], carMac[3], carMac[4], carMac[5], savedCh);
        return;
    }

    if (fbMode == MODE_DIRECT) {
        uint8_t fakeMac[] = MK_BASE_MAC;
        setMac(fakeMac);
        Serial.println("[ESPNOW] Direct: MAC → DE:AD:BE:EF:BA:5E");
    }
    if (fbMode == MODE_GAME && savedCh >= 1 && savedCh <= 13) {
        setChannel(savedCh);
        trySavedChannel  = true;
        savedChannelStart = nowMs();
        Serial.printf("[ESPNOW] Gespeicherter Kanal %d – warte auf Pairing\n", savedCh);
    } else {
        setChannel(MK_ESPNOW_CHANNEL);
    }
    if (esp_now_init() != ESP_OK) { Serial.println("[ESPNOW] Init fehlgeschlagen"); return; }
    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onEspNowSent);
    if (fbMode == MODE_GAME) {
        uint8_t baseMac[] = MK_BASE_MAC;
        addPeer(baseMac);
    }
    Serial.printf("[ESPNOW] Init OK, Kanal %d\n", MK_ESPNOW_CHANNEL);
}

// Senden und auf die Quittung des Funk-Stacks warten (hoechstens 10 ms).
// Ersetzt feste delay()-Pausen im Pairing.
volatile bool espNowSentFlag = false;
void onEspNowSent(const esp_now_send_info_t*, esp_now_send_status_t) { espNowSentFlag = true; }

void espNowSendWait(const uint8_t* mac, const uint8_t* data, size_t len) {
    espNowSentFlag = false;
    esp_now_send(mac, data, len);
    uint32_t t0 = nowMs();
    while (!espNowSentFlag && nowMs() - t0 < 10) delay(1);
}

// Gespeichertes Auto vergessen und neu starten — die FB sucht danach auf Kanal 1.
void forgetCarAndRestart() {
    uint8_t zero = 0;
    uint8_t noMac[6] = {};
    EEPROM.put(EEPROM_ADDR_CHANNEL, zero);
    EEPROM.put(EEPROM_ADDR_CARMAC, noMac);
    mkWatchdogStop();   // Neustart folgt ohnehin
    EEPROM.commit();
    ESP.restart();
}

// Die Neustarts unten (Rueckfall auf Kanal 1) nicht mitten im Menue oder in
// der Kalibrierung — sonst schliesst sich das Menue scheinbar von selbst. Sie
// holen das nach, sobald das Menue verlassen ist.
static bool restartAllowed() {
    return state == STATE_READY || state == STATE_OTA_WAIT;
}

void handlePairing() {
    if (reconnecting && nowMs() - reconnectStart > reconnectLimit && restartAllowed()) {
        Serial.printf("[ESPNOW] %lus kein Feedback vom Auto → Reboot auf Kanal 1\n",
                      reconnectLimit / 1000);
        forgetCarAndRestart();
    }
    if (trySavedChannel && !paired && nowMs() - savedChannelStart > SAVED_CHANNEL_TIMEOUT_MS
            && restartAllowed()) {
        uint8_t zero = 0;
        EEPROM.put(EEPROM_ADDR_CHANNEL, zero);
        mkWatchdogStop();   // Neustart folgt ohnehin
        EEPROM.commit();
        Serial.println("[ESPNOW] Kein Pairing auf gespeichertem Kanal → Reboot");
        ESP.restart();
    }
    if (paired) return;
    if (fbMode == MODE_DIRECT && pairingBeaconRx) {
        MkWdtPause wdtPause;   // Pairing inkl. EEPROM, nur beim Verbinden
        pairingBeaconRx = false;
        memcpy(peerMac, pairingCarMac, 6);
        addPeer(peerMac);
        // MAC-Wechsel gespoofte Basis-MAC → echte MAC. Auf dem ESP8266 ging das
        // erste Paket danach regelmaessig verloren (Bug 2026-09-03), dort
        // brauchte es 50 ms. Beim C6 genuegt eine kurze Pause; das Assign geht
        // trotzdem doppelt raus, und das Auto holt ein verlorenes Assign ueber
        // den ChannelSwitch nach. Statt fester Wartezeiten wird auf die
        // Sende-Quittung gewartet (zusammen ~20-30 ms statt ~110 ms).
        setMac(realMac);
        delay(10);
        MK_Assign assign;
        assign.slot = 1;
        memcpy(assign.baseMac, realMac, 6);
        espNowSendWait(peerMac, (uint8_t*)&assign, sizeof(assign));
        espNowSendWait(peerMac, (uint8_t*)&assign, sizeof(assign));
        // Zufälligen Betriebskanal wählen und Auto zum Wechsel auffordern
        const uint8_t channels[] = MK_DIRECT_CHANNELS;
        uint8_t ch = channels[random(MK_DIRECT_CHAN_COUNT)];
        MK_ChannelSwitch chSwitch;
        chSwitch.channel = ch;
        // Muss noch auf dem alten Kanal raus sein, bevor wir selbst wechseln
        espNowSendWait(peerMac, (uint8_t*)&chSwitch, sizeof(chSwitch));
        setChannel(ch);
        EEPROM.put(EEPROM_ADDR_CHANNEL, ch);
        EEPROM.put(EEPROM_ADDR_CARMAC, peerMac);
        EEPROM.commit();
        paired = true;
        trySavedChannel = false;
        lastFeedbackMs = nowMs();
        Serial.printf("[ESPNOW] Direct: gepairt, Kanal %d\n", ch);
        ledReady();
    }
    if (fbMode == MODE_GAME && pairingAssignRx) {
        pairingAssignRx = false;
        memcpy(peerMac, pairingBaseMac, 6);
        addPeer(peerMac);
        paired = true;
        trySavedChannel = false;
        lastFeedbackMs = nowMs();
        Serial.println("[ESPNOW] Game: gepairt");
        ledReady();
    }
}

void sendBeacon() {
    if (paired || fbMode != MODE_GAME) return;
    unsigned long now = nowMs();
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
    pkt.maxSpeed = maxSpeed;
    esp_now_send(peerMac, (uint8_t*)&pkt, sizeof(pkt));
}

// Auto in den Update-Modus schicken. 3× — einzelne Pakete gehen verloren,
// das Auto wertet nur den ersten aus (danach ist es im WLAN).
void sendOtaRequest() {
    if (!paired) return;
    MK_OtaRequest pkt;
    for (int i = 0; i < 3; i++) espNowSendWait(peerMac, (const uint8_t*)&pkt, sizeof(pkt));
    Serial.println("[OTA] Request gesendet");
}

void sendServoCal(uint8_t cmd, const uint16_t* us) {
    if (!paired) return;
    MK_ServoCal pkt;
    pkt.cmd = cmd;
    pkt.us[0] = us[0];
    pkt.us[1] = (cmd == SERVO_CAL_SAVE) ? us[1] : 0;
    pkt.us[2] = (cmd == SERVO_CAL_SAVE) ? us[2] : 0;
    esp_now_send(peerMac, (const uint8_t*)&pkt, sizeof(pkt));
}

void sendConfigPacket(int8_t trim, bool save) {
    if (!paired) return;
    MK_ConfigPacket pkt;
    pkt.trim = trim;
    pkt.save = save ? 1 : 0;
    esp_now_send(peerMac, (uint8_t*)&pkt, sizeof(pkt));
}

void handleFeedback() {
    if (feedbackNew) {
        noInterrupts();
        feedback.position   = feedbackRaw.position;
        feedback.lap        = feedbackRaw.lap;
        feedback.lapTotal   = feedbackRaw.lapTotal;
        feedback.item       = feedbackRaw.item;
        feedback.carBat     = feedbackRaw.carBat;
        feedback.trim       = feedbackRaw.trim;
        uint8_t rumbleCmd   = feedbackRaw.rumble;
        feedbackNew = false;
        interrupts();
        lastFeedbackMs = nowMs();
        if (reconnecting) {
            reconnecting = false;
            reconnectSkippable = false;
            Serial.println("[ESPNOW] Direct: Auto antwortet — wieder verbunden");
        }
        if (rumbleCmd == 1) rumbleFbEnd = nowMs() + 200;  // 200ms Safety-Timeout falls rumble=0 verloren geht
        else                rumbleFbEnd = 0;
    }
    // Timeout nur auf Betriebskanal aktiv — Kanal 1 ist Setup-Kanal (Mapping,
    // Pairing), dort schickt die Basis kein Feedback und kein Heartbeat nötig.
    if (paired && !reconnecting && getChannel() != MK_ESPNOW_CHANNEL
            && nowMs() - lastFeedbackMs > FEEDBACK_TIMEOUT_MS) {
        Serial.println("[ESPNOW] Verbindung verloren");
        if (fbMode == MODE_DIRECT) {
            // Gepairt bleiben, weiter senden: das Auto wartet auf diesem Kanal.
            reconnecting   = true;
            reconnectStart = nowMs();
            reconnectLimit = RECONNECT_LOST_MS;
            reconnectSkippable = false;   // im Betrieb nicht: Tasten fahren das Auto
            rumbleFbEnd    = 0;
        } else {
            enterSearchState();
        }
    }
}

void handleRumbleFb() {
    if (!rumbleEnabled || batPct <= BAT_CRIT_PCT) {
        ledcWrite(PIN_RUMBLE, 0);
        return;
    }
    uint32_t now = nowMs();
    bool on = now < rumbleFbEnd || now < rumbleConfirmEnd;
    ledcWrite(PIN_RUMBLE, on ? RUMBLE_PWM : 0);
}

// Warmstart = Reset ohne Stromunterbrechung (Watchdog, Absturz, ESP.restart(),
// USB). Tritt beim ESP32-C6 rev v0.2 auch mitten im Rennen auf (Interrupt-WDT,
// siehe mk_clock_guard.h) — dann zählt jede Sekunde bis zur Fahrbereitschaft.
// Nur echtes Einschalten, Brownout oder Unbekannt gelten als Kaltstart.
static bool isWarmBoot() {
    esp_reset_reason_t r = esp_reset_reason();
    return !(r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_UNKNOWN);
}

// ──────────────────────────────────────────────
// Setup
// ──────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    // USB-CDC: ist das Kabel steckt, aber kein Monitor offen, liest niemand den
    // Puffer aus — jedes printf wartet dann bis zum TX-Timeout und bremst die
    // Loop so stark aus, dass Tastendruecke und Steuerpakete verloren gehen.
    // Mit 0 wird bei vollem Puffer einfach verworfen statt gewartet.
    Serial.setTxTimeoutMs(0);
    mkClockGuardBegin();   // SYSTIMER-Fehler des C6 rev v0.2, siehe mk_clock_guard.h
    mkTickWatchBegin();    // Neustart, wenn der FreeRTOS-Tick stehen bleibt
    // Von fbShutdown() festgehaltene Pins freigeben (falls ein Reset ohne
    // Stromunterbrechung kam, z.B. ueber USB).
    gpio_hold_dis((gpio_num_t)PIN_TFT_BL);
    gpio_hold_dis((gpio_num_t)PIN_RUMBLE);
    gpio_hold_dis((gpio_num_t)PIN_LED);
    const bool warm = isWarmBoot();
    Serial.printf("[BOARD] FB V%d\n", FB_REV);
    Serial.printf("[RESET] reason=%d (%s)\n", (int)esp_reset_reason(),
                  warm ? "Warmstart" : "Kaltstart");
    pinMode(PIN_BTN_YELLOW, INPUT_PULLUP);
    pinMode(PIN_BTN_GREEN,  INPUT_PULLUP);
    pinMode(PIN_BTN_BLUE,   INPUT_PULLUP);
    pinMode(PIN_BTN_RED,    INPUT_PULLUP);
    // Joystick-Taster: nur definiert halten, bewusst ohne Auswertung.
    pinMode(PIN_BTN_STICK_L, INPUT_PULLUP);
    pinMode(PIN_BTN_STICK_R, INPUT_PULLUP);
    ledcAttach(PIN_RUMBLE, RUMBLE_PWM_FREQ, RUMBLE_PWM_RES);
    ledcWrite(PIN_RUMBLE, 0);
#ifndef MK_TEST_NO_I2C
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    // Kurzer Timeout: eine ADS-Uebertragung dauert <1 ms. Mit dem Standard von
    // 50 ms wartete die Loop bei einer I2C-Stoerung so lange, dass der
    // Loop-Watchdog (50 ms, mk_watchdog.h) ausloeste — FB 2026-09-28.
    Wire.setTimeOut(5);
#endif
    EEPROM.begin(EEPROM_SIZE);
    led.begin();
    led.setBrightness(80);
    setLed(0, 0, 0);
    pinMode(PIN_MODE, INPUT);
#ifdef MK_TEST_BARE
    fbMode = MODE_DIRECT;   // ohne Platine haengt der Modus-Pin in der Luft
    Serial.println("[TEST] Nacktes C6 ohne Platine (MK_TEST_BARE), Direct fest");
#else
    fbMode = digitalRead(PIN_MODE) ? MODE_DIRECT : MODE_GAME;
#endif
#ifdef MK_TEST_USB_POWER
    Serial.println("[TEST] Platine per USB versorgt, Akku-Messung aus (MK_TEST_USB_POWER)");
#endif
    Serial.printf("[MODE] %s\n", fbMode == MODE_DIRECT ? "Direct" : "Game");
    // I2C-Scan beim Boot (nur Kaltstart — beim Warmstart zaehlt jede ms) — zeigt sofort, ob nichts am Bus haengt (Verdrahtung,
    // Versorgung, Pull-ups) oder ob ein Geraet nur auf einer anderen Adresse
    // sitzt (ADS1115: ADDR→GND 0x48, →VDD 0x49, →SDA 0x4A, →SCL 0x4B).
#ifdef MK_TEST_NO_I2C
    // Testbuild (env fb_noi2c): kein I2C-Verkehr, um zu pruefen, ob der
    // SYSTIMER-Fehler des C6 ohne I2C ausbleibt. Sticks liefern dann nichts,
    // die FB sendet neutrale Werte (Sicherheitsgurt !adsOK).
    Serial.println("[TEST] I2C deaktiviert (MK_TEST_NO_I2C)");
    adsOK = false;
#else
    if (!warm) {
        int found = 0;
        Serial.print("[I2C] Scan:");
        for (uint8_t a = 1; a < 127; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); found++; }
        }
        if (!found) Serial.print(" nichts gefunden");
        Serial.printf("  (%d Geraet(e))\n", found);
    }

    adsOK  = ads.begin(0x48);
    if (adsOK) ads.setDataRate(RATE_ADS1115_860SPS);
#endif
    // ST7789 haengt am Hardware-SPI (FSPI). MISO bleibt frei — das Display
    // liest nicht zurueck, also gibt es keinen Presence-Check wie beim I2C-OLED.
    // Fehlt das Panel, zeichnet die Firmware ins Leere; auf SPI ist das
    // folgenlos und still, anders als die I2C-Fehlerflut vorher.
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, LOW);          // Backlight aus bis das Bild steht
    SPI.begin(PIN_TFT_SCK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
    display.init(TFT_PANEL_W, TFT_PANEL_H);
#ifdef MK_TEST_SPI_10MHZ
    // Testbuild: Display-SPI mit 10 statt 40 MHz — prueft, ob die schnellen
    // Flanken auf den (bei abgezogenem Display offen endenden) Leitungen die
    // SYSTIMER-/I2C-Fehler des C6 beguenstigen.
    display.setSPISpeed(10000000);
    Serial.println("[TEST] Display-SPI 10 MHz (MK_TEST_SPI_10MHZ)");
#else
    display.setSPISpeed(40000000);
#endif
    display.setRotation(TFT_ROTATION);
    display.fillScreen(COL_BG);
    ledcAttach(PIN_TFT_BL, TFT_BL_FREQ, TFT_BL_RES);
    ledcWrite(PIN_TFT_BL, TFT_BL_LEVEL);
    dispOK = true;
    if (!adsOK)  Serial.println("[FEHLER] ADS1115 nicht gefunden");
    Serial.printf("[TFT] ST7789 %dx%d init, Rotation %d\n", TFT_W, TFT_H, TFT_ROTATION);
    if (!BAT_WIRED) Serial.println("[BAT] Teiler nicht verdrahtet (BAT_WIRED=false) — Akku wird als 100% gemeldet");
    // Nur beim Kaltstart: bei einem Warmstart mitten im Rennen koennte gerade
    // Rot (Licht) gedrueckt sein — das darf nicht die Einstellungen loeschen.
    if (!warm && !digitalRead(PIN_BTN_RED)) {
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
    lastBatCheck = nowMs();
    state = STATE_READY;
    initEspNow();
    ledPulseOrange();
    mkWatchdogBegin();     // Loop-WDT 50 ms + RTC-WDT 1 s, siehe mk_watchdog.h
}

// ──────────────────────────────────────────────
// Loop
// ──────────────────────────────────────────────
#define BTN_STABLE_ROUNDS 3
bool debounceButton(uint8_t idx, bool raw) {
    static bool    stable[4] = {};
    static uint8_t count[4]  = {};
    if (raw == stable[idx]) { count[idx] = 0; return stable[idx]; }
    if (++count[idx] >= BTN_STABLE_ROUNDS) { stable[idx] = raw; count[idx] = 0; }
    return stable[idx];
}

// Display-Wiederbelebung: der ST7789 bekam gelegentlich Fehlbefehle (Bild
// schwarz = SLPIN/DISPOFF, invertiert = INVOFF), vermutlich durch abgebrochene
// Uebertragungen oder Stoerungen auf DC/CS. Alle 2 s die Grundeinstellungen
// erneut senden — ein paar Bytes, unsichtbar, heilt genau diese Faelle.
// Ist der Bildspeicher selbst verloren, hilft erst das naechste Neuzeichnen.
#define DISPLAY_REVIVE_MS 2000
void reviveDisplay() {
    static uint32_t last = 0;
    if (!dispOK || nowMs() - last < DISPLAY_REVIVE_MS) return;
    last = nowMs();
    static const uint8_t colmod = 0x55;          // 16 bit/Pixel
    display.sendCommand(0x11);                   // SLPOUT
    display.sendCommand(0x13);                   // NORON
    display.sendCommand(0x21);                   // INVON (dieses Panel braucht es)
    display.sendCommand(0x3A, &colmod, 1);       // COLMOD
    display.setRotation(TFT_ROTATION);           // MADCTL + Fenster-Offsets
    display.sendCommand(0x29);                   // DISPON
}

void loop() {
    mkDiagTick();
    mkWatchdogFeed();
    static unsigned long lastLoopMs = 0;
    static bool prevYellow = false, prevGreen = false, prevBlue = false, prevRed = false;

    // Entprellt: ein Zustand gilt erst nach BTN_STABLE_ROUNDS gleichen
    // Lesungen in Folge (3 × 20 ms). Einzelne Stoerspitzen — am Auto ging
    // 2026-09-26 "zufaellig" das Licht an — fallen damit raus.
    bool bYellow = debounceButton(0, !digitalRead(PIN_BTN_YELLOW));
    bool bGreen  = debounceButton(1, !digitalRead(PIN_BTN_GREEN));
    bool bBlue   = debounceButton(2, !digitalRead(PIN_BTN_BLUE));
    bool bRed    = debounceButton(3, !digitalRead(PIN_BTN_RED));

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
    // false = ADS1115 hat in dieser Runde nicht sauber geantwortet → neutral
    bool i2cCycleOk = true;
    if (adsOK) {
        int16_t nLX = rawLX, nLY = rawLY, nRX = rawRX, nRY = rawRY;
        if (!adsAlive()) {
            i2cCycleOk = false;   // gar nicht erst lesen, Muell ist sicher
        } else if (state == STATE_READY) {
            // Nur die 2 aktiven Kanäle, dafür mit Oversampling
            uint8_t chA = swapSticks ? JS_RIGHT_Y : JS_LEFT_Y;
            uint8_t chB = swapSticks ? JS_LEFT_X  : JS_RIGHT_X;
            int32_t sumA = 0, sumB = 0;
            for (int i = 0; i < ADC_OVERSAMPLE; i++) {
                sumA += ads.readADC_SingleEnded(chA);
                sumB += ads.readADC_SingleEnded(chB);
            }
            if (swapSticks) { nRY = sumA / ADC_OVERSAMPLE; nLX = sumB / ADC_OVERSAMPLE; }
            else            { nLY = sumA / ADC_OVERSAMPLE; nRX = sumB / ADC_OVERSAMPLE; }
            // Gegencheck: einen der beiden ungenutzten Kanaele mitlesen (im
            // Wechsel). Drei Kanaele mit exakt gleichem Wert = ADS misst nicht.
            static bool alt = false;
            alt = !alt;
            uint8_t chC = swapSticks ? (alt ? JS_LEFT_Y : JS_RIGHT_X)
                                     : (alt ? JS_LEFT_X : JS_RIGHT_Y);
            int16_t vC = ads.readADC_SingleEnded(chC);
            int16_t vA = swapSticks ? nRY : nLY, vB = swapSticks ? nLX : nRX;
            if (vA == vB && vB == vC) {
                Serial.printf("[STICK] A%u/A%u/A%u liefern alle %d — ADS haengt\n", chA, chB, chC, vA);
                i2cCycleOk = false;
            }
        } else {
            // Menü/Kalibrierung: alle 4 Kanäle für Navigation und Kalibrierung
            nLX = ads.readADC_SingleEnded(JS_LEFT_X);
            nLY = ads.readADC_SingleEnded(JS_LEFT_Y);
            nRX = ads.readADC_SingleEnded(JS_RIGHT_X);
            nRY = ads.readADC_SingleEnded(JS_RIGHT_Y);
        }
        // Auch waehrend des Lesens kann der Bus ausfallen — dann gelten die
        // Werte nicht. Single-ended liefert nie deutlich negative Counts.
        if (i2cCycleOk && (!adsAlive() || nLX < -200 || nLY < -200 || nRX < -200 || nRY < -200))
            i2cCycleOk = false;
        // Nur im Fahrbetrieb: in der Kalibrierung duerfen Werte ueber den
        // bisherigen Anschlag hinausgehen, genau die sollen ja gemessen werden.
        if (i2cCycleOk && state == STATE_READY) {
            bool okA = swapSticks ? stickInRange(JS_RIGHT_Y, nRY) : stickInRange(JS_LEFT_Y, nLY);
            bool okB = swapSticks ? stickInRange(JS_LEFT_X,  nLX) : stickInRange(JS_RIGHT_X, nRX);
            if (!okA || !okB) i2cCycleOk = false;
        }
        if (i2cCycleOk) {
            rawLX = nLX; rawLY = nLY; rawRX = nRX; rawRY = nRY;
        } else {
            // Sticks auf Mitte: Fahrt, Menue-Navigation und Anzeige neutral
            rawLX = jsCenter[JS_LEFT_X];  rawLY = jsCenter[JS_LEFT_Y];
            rawRX = jsCenter[JS_RIGHT_X]; rawRY = jsCenter[JS_RIGHT_Y];
        }
        handleI2cHealth(i2cCycleOk);
        mkWatchdogCheckpoint();
    }

    // Red 10s halten im Normalbetrieb → EEPROM-Reset (ab 5s Countdown)
    static unsigned long bRedResetHold = 0;
    static int           lastCountdown = -1;
    bool countdownShowing = false;
    if (state == STATE_READY) {
        if (bRed) {
            if (bRedResetHold == 0) bRedResetHold = nowMs();
            unsigned long elapsed = nowMs() - bRedResetHold;
            if (elapsed >= 10000) {
                resetSettings();
                state = STATE_RESET;
                stateStart = nowMs();
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

    if (nowMs() - lastBatCheck >= BAT_CHECK_MS) {
        lastBatCheck = nowMs();
        updateBattery();
    }

    handleFeedback();
    if (pendingChannelSave > 0) {
        uint8_t ch = pendingChannelSave;
        pendingChannelSave = 0;
        EEPROM.put(EEPROM_ADDR_CHANNEL, ch);
        { MkWdtPause wdtPause; EEPROM.commit(); }
        Serial.printf("[ESPNOW] Kanal %d gespeichert\n", ch);
    }
    handlePairing();
    sendBeacon();

    // Suchzustand: LED pulsiert bis Pairing steht
    if (state == STATE_READY && (!paired || reconnecting)) {
        ledPulseOrange();
        displayConnecting();
    }

    // Display dimmen, wenn 60 s lang weder Stick noch Button bewegt wurde —
    // spart Akku, wenn die FB herumliegt. Jede Eingabe macht sofort wieder hell.
    // Im Menue bleibt es hell. Gezaehlt werden nur die im Fahrbetrieb gelesenen
    // Achsen; mapJS() liefert innerhalb der Dead Zone 0.
    {
        static unsigned long lastInputMs = 0;
        static bool dimmed = false;
        bool input = bYellow || bGreen || bBlue || bRed || state != STATE_READY
                  || mapJS(swapSticks ? rawRY : rawLY, swapSticks ? JS_RIGHT_Y : JS_LEFT_Y) != 0
                  || mapJS(swapSticks ? rawLX : rawRX, swapSticks ? JS_LEFT_X  : JS_RIGHT_X) != 0;
        if (input || lastInputMs == 0) lastInputMs = nowMs();
        bool wantDim = nowMs() - lastInputMs > TFT_DIM_AFTER_MS;
        if (wantDim != dimmed) {
            dimmed = wantDim;
            ledcWrite(PIN_TFT_BL, dimmed ? TFT_BL_DIM : TFT_BL_LEVEL);
        }
    }

    FBState stateBefore = state;   // Taste, die im Menue beginnt, geht nie ans Auto
    bool active = handleState(bYellowP, bGreenP, bBlueP, bRedP, bGreen, bBlue, rawLX, rawLY, rawRX, rawRY);

    // Tasten fuers Auto: nur einzeln gedrueckte, und erst beim Loslassen.
    // Sonst loest der Menue-Griff (Gruen + Blau) jedes Mal den Stern aus —
    // Gruen kommt fast immer einen Moment vor Blau an. Ein Griff zaehlt nicht,
    // sobald eine zweite Taste dazukommt oder er ausserhalb des Normalbetriebs
    // lag (Menue verlassen mit Rot schaltet sonst beim Loslassen das Licht).
    // Das Bit bleibt TAP_PACKETS Pakete gesetzt, damit ein verlorenes Paket
    // die Flanke nicht verschluckt.
    static const uint8_t TAP_PACKETS = 3;
    static uint8_t chordMask  = 0;       // alle Tasten seit dem ersten Druck
    static bool    chordValid = true;
    static uint8_t tapMask    = 0;
    static uint8_t tapLeft    = 0;
    uint8_t held = (bYellow ? MK_BTN_YELLOW : 0) | (bGreen ? MK_BTN_GREEN : 0)
                 | (bBlue ? MK_BTN_BLUE : 0) | (bRed ? MK_BTN_RED : 0);
    if (held) {
        if (!chordMask) chordValid = true;
        chordMask |= held;
        if (state != STATE_READY || stateBefore != STATE_READY) chordValid = false;
    } else if (chordMask) {
        bool single = (chordMask & (chordMask - 1)) == 0;
        if (single && chordValid && state == STATE_READY) { tapMask = chordMask; tapLeft = TAP_PACKETS; }
        chordMask = 0;
    }
    // Suche nach dem gespeicherten Auto direkt nach dem Boot per Taste abbrechen.
    // Ueber den Tap (einzelne Taste, beim Loslassen, im Normalbetrieb) — sonst
    // braeche schon das erste Gruen des Menue-Griffs Gruen+Blau die Suche ab.
    if (tapLeft == TAP_PACKETS && reconnecting && reconnectSkippable) {
        Serial.println("[ESPNOW] Suche per Taste abgebrochen → Reboot auf Kanal 1");
        forgetCarAndRestart();
    }
    if (tapLeft == TAP_PACKETS)
        Serial.printf("[TAP] Taste 0x%02X ans Auto (Zustand %d)\n", tapMask, (int)state);
    uint8_t sendMask = tapLeft ? tapMask : 0;
    if (tapLeft) tapLeft--;

    if (active) {
        int8_t throttle = swapSticks ? mapJS(rawRY, JS_RIGHT_Y) : mapJS(rawLY, JS_LEFT_Y);
        int8_t steering = swapSticks ? mapJS(rawLX, JS_LEFT_X)  : mapJS(rawRX, JS_RIGHT_X);

        // Sicherheitsgurt: ohne ADS1115 bleiben die Rohwerte auf 0, und mapJS()
        // rechnet gegen jsCenter (9994) → -100/-100, also Vollgas rueckwaerts
        // mit Lenkung voll links. Faellt der ADS im Betrieb aus waehrend die FB
        // gepairt ist, bekaeme das Auto genau das gesendet.
        if (!adsOK || !i2cCycleOk) { throttle = 0; steering = 0; }

#ifdef MK_LOG_INPUTS
        // Achsen/Buttons jede Loop-Runde (~50 Zeilen/s) — nur zum Debuggen.
        // Standardmaessig aus: der Dauerverkehr ueber USB-Serial/JTAG steht im
        // Verdacht, den SYSTIMER-Fehler des C6 (mk_clock_guard.h) zu haeufen.
        Serial.printf("LX:%6d | LY:%6d | RX:%6d | RY:%6d | Y:%d G:%d B:%d R:%d | thr:%4d str:%4d | swap:%d\n",
            rawLX, rawLY, rawRX, rawRY, bYellow, bGreen, bBlue, bRed, throttle, steering, swapSticks);
#endif

        sendControlInput(throttle, steering, sendMask & MK_BTN_YELLOW, sendMask & MK_BTN_GREEN,
                         sendMask & MK_BTN_BLUE, sendMask & MK_BTN_RED);

        static int8_t prevMappingSlot = -2;
        if (mappingSlot >= 1) {
            if (mappingSlot != prevMappingSlot) {
                prevMappingSlot = mappingSlot;
                displayMapping(mappingSlot);
            }
        } else if (paired && !reconnecting && !countdownShowing) {
            prevMappingSlot = -1;
            displayNormal(
                mapJS(rawLX, JS_LEFT_X), mapJS(rawLY, JS_LEFT_Y),
                mapJS(rawRX, JS_RIGHT_X), mapJS(rawRY, JS_RIGHT_Y),
                bYellow, bGreen, bBlue, bRed
            );
        }
    } else if (paired) {
        // Menue/Kalibrierung: neutrales Steuerpaket als Keepalive. Sonst hoert
        // das Auto nichts mehr, faellt nach 30s auf Kanal 1 zurueck und die
        // Zuordnung koennte wechseln. Nebeneffekt: das Auto steht sofort, statt
        // erst nach seinem 200ms-Timeout, und die Trim-Vorschau zeigt die
        // Raeder in Mittelstellung plus Trim.
        sendControlInput(0, 0, false, false, false, false);
    }

    if (state == STATE_DEBUG) {
        displayDebug(
            mapJS(rawLX, JS_LEFT_X), mapJS(rawLY, JS_LEFT_Y),
            mapJS(rawRX, JS_RIGHT_X), mapJS(rawRY, JS_RIGHT_Y),
            bYellow, bGreen, bBlue, bRed
        );
    }

    handleRumbleFb();
    reviveDisplay();

    unsigned long elapsed = nowMs() - lastLoopMs;
    if (LOOP_PERIOD_MS > elapsed) delay(LOOP_PERIOD_MS - elapsed);
    lastLoopMs = nowMs();
}
