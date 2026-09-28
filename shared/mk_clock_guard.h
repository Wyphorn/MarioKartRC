#pragma once
// ── Clock-Guard gegen den SYSTIMER-Fehler des ESP32-C6 rev v0.2 ──────────────
//
// Hardwarefehler: Im Zähler SYSTIMER UNIT0, aus dem esp_timer_get_time() und
// damit millis() kommen, fallen gelegentlich einzelne Bits von selbst auf 0.
// Die Uhr springt dann rückwärts (bei uns gemessen: 33 s und 516 s) und zählt
// ab dem niedrigeren Wert weiter. Tritt nur auf, wenn Funk und ein I2C-Master
// gleichzeitig laufen — FB (ADS1115) und Auto (IMU) tun beides. Beide Boards
// sind rev v0.2 (per esptool geprüft 2026-09-25). Folgen bei uns: das Auto hielt
// 50 Pakete/s für "30 s Funkstille" und startete neu, die FB meldete kurzzeitig
// "Verbindung verloren".
//   https://github.com/espressif/esp-idf/issues/19036
//
// Abhilfe: Der FreeRTOS-Tick läuft auf SYSTIMER UNIT1, den der Fehler nicht
// trifft. Ein eigener Task vergleicht beide Uhren; hängt esp_timer plötzlich
// hinterher, wird er um genau die Differenz vorgesetzt — mit demselben Aufruf,
// mit dem IDF die Uhr nach Light-Sleep nachführt. Eigene Umsetzung nach der
// Idee aus github.com/danjurgens/esp32c6-systimer-backstep (dort GPL-3.0,
// deshalb kein übernommener Code).
//
// Einmal in setup() nach Serial.begin() aufrufen: mkClockGuardBegin();
// Zeitmessung im eigenen Code immer über nowMs(), nie millis().

#include <Arduino.h>
#include "esp_timer.h"
#include "esp_private/esp_timer_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef MK_TEST_TICK_STALL
#include "hal/systimer_ll.h"
#include "soc/systimer_struct.h"
#endif

// Prüfabstand. Bis zur Reparatur sieht millis() den falschen Wert — kurz halten.
static constexpr uint32_t MK_CLOCK_CHECK_MS     = 50;
// Beobachtet wurden Sprünge ab 1.5 s (2026-09-26) — die Annahme "nie unter
// Bit 26 ≈ 4.2 s" aus dem Fehlerbericht stimmt nicht. Kleinere Sprünge bleiben
// unrepariert; unser Code nutzt nowMs() und sieht sie nicht. Der Tick selbst
// verliert nach langen Interrupt-Sperren höchstens ein paar hundert ms.
static constexpr int32_t  MK_CLOCK_THRESHOLD_MS = 1000;

static volatile uint32_t gClockRepairs = 0;

// ── Ereigniszaehler seit dem Einschalten ──────────────────────────────────
// Liegen im RTC-Speicher und ueberstehen damit jeden Neustart ohne
// Stromunterbrechung (Watchdog, Tick-Wache, Absturz, USB-Reset). Beim echten
// Einschalten beginnen sie bei 0. So laesst sich auch ohne angeschlossenes
// USB messen: FB laufen lassen, danach USB anstecken — der Reset beim
// Anstecken gibt die Zaehler im Log aus, das Display zeigt sie laufend.
struct MkDiag {
    uint32_t magic;
    uint32_t clock;       // Uhr-Ruecksprung repariert (UNIT0)
    uint32_t tick;        // Neustart durch Tick-Wache (UNIT1 stand)
    uint32_t wdt;         // Neustart durch Watchdog
    uint32_t panic;       // Neustart nach Absturz
    uint32_t i2c;         // I2C-Stoerungen (FB)
    uint32_t i2cReinit;   // davon mit Bus-Neuinitialisierung
    uint32_t upSec;       // Laufzeit seit Einschalten in s (ueber Neustarts summiert)
};
static RTC_NOINIT_ATTR MkDiag gDiag;
static constexpr uint32_t MK_DIAG_MAGIC = 0xD1A6C0DEu;

// Laufzeit aufsummieren — regelmaessig aus der Loop aufrufen.
static void mkDiagTick() {
    static uint32_t lastMs = 0;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - lastMs >= 1000) { gDiag.upSec += (now - lastMs) / 1000; lastMs = now - (now - lastMs) % 1000; }
}

// Millisekunden seit dem Start aus dem FreeRTOS-Tick (UNIT1, 1000 Hz) statt aus
// esp_timer (UNIT0). Unsere Firmware nutzt ausschließlich diese Uhr statt
// millis(): sie springt nie zurück, auch nicht in den ≤50 ms zwischen einem
// Rücksprung und der Reparatur durch den Guard. Läuft wie millis() nach
// 49.7 Tagen über, Differenzen "nowMs() - t" bleiben dabei korrekt.
static inline uint32_t nowMs() {
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// Positiv = esp_timer hängt hinter dem Tick zurück.
static int32_t mkClockDivergenceMs() {
    uint32_t tickMs = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t espMs  = (uint32_t)(esp_timer_get_time() / 1000);
    return (int32_t)(tickMs - espMs);
}

static void mkClockGuardTask(void*) {
    int32_t baseline = mkClockDivergenceMs();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MK_CLOCK_CHECK_MS));
        int32_t div    = mkClockDivergenceMs();
        int32_t excess = div - baseline;
        if (excess > MK_CLOCK_THRESHOLD_MS) {
            // Mehr als die Laufzeit seit dem Start kann der Zähler nicht
            // verlieren — alles darüber ist ein Messfehler, nicht anfassen.
            int64_t uptimeMs = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
            if ((int64_t)excess <= uptimeMs + 60000) {
                esp_timer_private_lock();
                esp_timer_private_advance((int64_t)excess * 1000);
                esp_timer_private_unlock();
                gClockRepairs = gClockRepairs + 1;
                gDiag.clock = gDiag.clock + 1;
                Serial.printf("[CLOCK] esp_timer %ld ms zurueckgesprungen – repariert (#%lu)\n",
                              (long)excess, (unsigned long)gClockRepairs);
            } else {
                Serial.printf("[CLOCK] unplausibler Sprung %ld ms – nicht korrigiert\n", (long)excess);
                baseline = div;
            }
        } else {
            // Gesund oder der Tick hat etwas verloren: Bezugswert nachziehen.
            baseline = div;
        }
    }
}

// ── Tick-Wache ────────────────────────────────────────────────────────────
// Zweite Fehlerform desselben C6-rev-v0.2-Problems: der FreeRTOS-Tick (UNIT1)
// bleibt stehen. Wartet die Loop gerade per delay(), greift der Interrupt-WDT
// (FB 2026-09-26 12:35). Wartet sie nie, dreht sie sich weiter, aber nowMs()
// steht und kein Timer wird mehr faellig — das Geraet ist tot, ohne neu zu
// starten (Auto 2026-09-26 14:26: LED stand auf gruen, kein Feedback mehr).
//
// Die Wache laeuft als esp_timer-Callback: der wird von UNIT0 ausgeloest und
// braucht keinen Tick. Steht der Tick laenger als MK_TICK_STALL_MS, Neustart
// (Warmstart, ~1 s). Normale Tick-Verzoegerungen durch Interrupt-Sperren
// (Funk, Flash) liegen darunter; der Interrupt-WDT liegt bei 300 ms.
static constexpr uint32_t MK_TICK_CHECK_MS = 20;
static constexpr uint32_t MK_TICK_STALL_MS = 250;

// Ueberlebt den Software-Neustart (nicht das Ausschalten): so kann die Firmware
// nach dem Neustart melden, dass die Tick-Wache ausgeloest hat.
static RTC_NOINIT_ATTR uint32_t gTickWatchMagic;
static RTC_NOINIT_ATTR uint32_t gTickWatchStallMs;
static constexpr uint32_t MK_TICK_WATCH_MAGIC = 0x7157A11Cu;

static void mkTickWatchCb(void*) {
    static TickType_t lastTick    = 0;
    static int64_t    lastChange  = 0;
    static uint32_t   lastRepairs = 0;
    TickType_t t = xTaskGetTickCount();
    int64_t    e = esp_timer_get_time();
    // Tick laeuft, oder der Guard hat esp_timer gerade vorgesetzt (dann waere
    // die gemessene Dauer kuenstlich lang): Bezugspunkt neu setzen.
    if (t != lastTick || gClockRepairs != lastRepairs || e < lastChange) {
        lastTick    = t;
        lastChange  = e;
        lastRepairs = gClockRepairs;
        return;
    }
    int64_t stallMs = (e - lastChange) / 1000;
    if (stallMs > MK_TICK_STALL_MS) {
        gTickWatchMagic   = MK_TICK_WATCH_MAGIC;
        gTickWatchStallMs = (uint32_t)stallMs;
        esp_restart();
    }
}

// In setup() nach Serial.begin(): meldet einen vorherigen Tick-Neustart und
// startet die Wache.
static void mkTickWatchBegin() {
    // Zaehler: beim echten Einschalten (oder ungueltigem Inhalt) auf 0, sonst
    // den Grund des letzten Neustarts mitzaehlen.
    esp_reset_reason_t rr = esp_reset_reason();
    bool cold = rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT || rr == ESP_RST_UNKNOWN;
    if (cold || gDiag.magic != MK_DIAG_MAGIC) {
        memset(&gDiag, 0, sizeof(gDiag));
        gDiag.magic = MK_DIAG_MAGIC;
        gTickWatchMagic = 0;
    } else if (rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) {
        gDiag.wdt++;
    } else if (rr == ESP_RST_PANIC) {
        gDiag.panic++;
    }
    if (gTickWatchMagic == MK_TICK_WATCH_MAGIC) gDiag.tick++;
    Serial.printf("[DIAG] seit Einschalten (%lu min): Uhr %lu  Tick %lu  WDT %lu  Absturz %lu  I2C %lu (Neuinit %lu)\n",
                  (unsigned long)(gDiag.upSec / 60), (unsigned long)gDiag.clock,
                  (unsigned long)gDiag.tick, (unsigned long)gDiag.wdt,
                  (unsigned long)gDiag.panic, (unsigned long)gDiag.i2c,
                  (unsigned long)gDiag.i2cReinit);
    if (gTickWatchMagic == MK_TICK_WATCH_MAGIC)
        Serial.printf("[TICK] Letzter Neustart durch Tick-Wache: Tick stand %lu ms\n",
                      (unsigned long)gTickWatchStallMs);
    gTickWatchMagic = 0;
    const esp_timer_create_args_t args = {
        .callback = mkTickWatchCb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "tick_watch",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t h;
    if (esp_timer_create(&args, &h) == ESP_OK)
        esp_timer_start_periodic(h, MK_TICK_CHECK_MS * 1000);
#ifdef MK_TEST_TICK_STALL
    // Test: nach 20 s den Tick-Zaehler UNIT1 anhalten — genau das Fehlerbild,
    // das die Wache abfangen soll. Nur per Build-Flag, nie im normalen Build.
    const esp_timer_create_args_t stallArgs = {
        .callback = [](void*) { systimer_ll_enable_counter(&SYSTIMER, 1, false); },
        .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
        .name = "tick_stall_test", .skip_unhandled_events = false,
    };
    esp_timer_handle_t st;
    if (esp_timer_create(&stallArgs, &st) == ESP_OK) esp_timer_start_once(st, 20 * 1000000);
    Serial.println("[TEST] Tick wird in 20 s angehalten (MK_TEST_TICK_STALL)");
#endif
}

static void mkClockGuardBegin() {
    // Priorität über der Arduino-Loop (1), damit die Reparatur nicht wartet.
    xTaskCreate(mkClockGuardTask, "clock_guard", 3072, nullptr, 5, nullptr);
}
