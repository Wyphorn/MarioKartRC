#pragma once
// ── Loop-Watchdog + RTC-Watchdog ─────────────────────────────────────────────
//
// Anlass: FB 2026-09-28 00:29 — die Loop blieb stehen, der Tick lief weiter.
// Weder Interrupt-WDT noch Tick-Wache (mk_clock_guard.h) greifen in dem Fall;
// die FB hing bis zum Ausschalten. Zwei Stufen:
//
// 1. Loop-Watchdog (Task-WDT, 50 ms): die Loop meldet sich jede Runde. Bleibt
//    das aus, Panic mit Aufrufkette im Log (bei USB) und Neustart (Warmstart).
//    Eine normale Runde dauert FB ~15–20 ms, Auto weniger.
// 2. RTC-Watchdog (1 s): laeuft mit eigenem Takt unabhaengig von CPU und
//    Interrupts. Holt den Chip auch aus Faellen, in denen selbst die
//    Absturzbehandlung haengt (FB 2026-09-25 22:57: Endlosschleife im Panic-
//    Handler mit kaputtem Stackpointer).
//
// Lange, aber gewollte Ablaeufe (EEPROM schreiben, Kalibrieren, Pairing,
// Abschaltung) melden sich mit MkWdtPause ab — als lokale Variable, der
// Destruktor meldet wieder an:
//     { MkWdtPause p; EEPROM.commit(); }
//
// In setup() am Ende: mkWatchdogBegin();  In loop() am Anfang: mkWatchdogFeed();
// Vor esp_deep_sleep_start(): mkWatchdogStop() — der RTC-WDT liefe sonst weiter.

#include <Arduino.h>
#include "esp_task_wdt.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"

static constexpr uint32_t MK_LOOP_WDT_MS = 50;
static constexpr uint32_t MK_RTC_WDT_MS  = 1000;

static wdt_hal_context_t gRtcWdt = RWDT_HAL_CONTEXT_DEFAULT();
static bool gWdtActive = false;
static int  gWdtPauseDepth = 0;
static uint32_t gWdtLastFeedMs = 0;
static uint32_t gWdtMaxGapMs = 0;   // laengster Abstand zwischen zwei Loop-Runden

static void mkRtcWdtStart() {
    uint32_t ticks = (uint32_t)((uint64_t)MK_RTC_WDT_MS * rtc_clk_slow_freq_get_hz() / 1000);
    wdt_hal_write_protect_disable(&gRtcWdt);
    wdt_hal_init(&gRtcWdt, WDT_RWDT, 0, false);
    wdt_hal_config_stage(&gRtcWdt, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&gRtcWdt);
    wdt_hal_write_protect_enable(&gRtcWdt);
}

static void mkRtcWdtStop() {
    wdt_hal_write_protect_disable(&gRtcWdt);
    wdt_hal_disable(&gRtcWdt);
    wdt_hal_write_protect_enable(&gRtcWdt);
}

static void mkWatchdogBegin() {
    // Nur die Loop beobachten, nicht den Idle-Task: der kaeme bei 50 ms und
    // kurzen Rechenspitzen sonst faelschlich in Verdacht.
    esp_task_wdt_config_t cfg = { .timeout_ms = MK_LOOP_WDT_MS, .idle_core_mask = 0,
                                  .trigger_panic = true };
    esp_task_wdt_reconfigure(&cfg);
    enableLoopWDT();          // Arduino-Core: meldet die Loop bei jedem Durchlauf
    esp_task_wdt_reset();
    mkRtcWdtStart();
    gWdtActive = true;
}

static void mkWatchdogFeed() {
    if (!gWdtActive || gWdtPauseDepth) return;
    // Laengste Runde mitschreiben — zeigt, wie viel Luft bis MK_LOOP_WDT_MS bleibt.
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (gWdtLastFeedMs) {
        uint32_t gap = now - gWdtLastFeedMs;
        if (gap > gWdtMaxGapMs && gap > 25) {
            gWdtMaxGapMs = gap;
            Serial.printf("[LOOP] neue laengste Runde: %lu ms (Grenze %lu)\n",
                          (unsigned long)gap, (unsigned long)MK_LOOP_WDT_MS);
        }
    }
    gWdtLastFeedMs = now;
    wdt_hal_write_protect_disable(&gRtcWdt);
    wdt_hal_feed(&gRtcWdt);
    wdt_hal_write_protect_enable(&gRtcWdt);
    // Der Loop-WDT wird vom Arduino-Core vor jedem loop() gefuettert.
}

// Zwischenmeldung in gewollt laengeren Abschnitten einer Runde (z.B. ganzen
// Bildschirm neu zeichnen, ~25 ms). Jeder Abschnitt bekommt so wieder
// MK_LOOP_WDT_MS, ohne die Grenze fuer echte Haenger aufzuweichen.
static void mkWatchdogCheckpoint() {
    if (!gWdtActive || gWdtPauseDepth) return;
    esp_task_wdt_reset();
    wdt_hal_write_protect_disable(&gRtcWdt);
    wdt_hal_feed(&gRtcWdt);
    wdt_hal_write_protect_enable(&gRtcWdt);
}

static void mkWatchdogStop() {
    if (!gWdtActive) return;
    disableLoopWDT();
    mkRtcWdtStop();
    gWdtActive = false;
}

// Abmelden fuer gewollt lange Ablaeufe, verschachtelbar.
struct MkWdtPause {
    MkWdtPause() {
        if (gWdtActive && gWdtPauseDepth++ == 0) { disableLoopWDT(); mkRtcWdtStop(); }
    }
    ~MkWdtPause() {
        if (gWdtActive && --gWdtPauseDepth == 0) {
            gWdtLastFeedMs = 0;   // Pause nicht als lange Runde werten
            mkRtcWdtStart();
            enableLoopWDT();
        }
    }
};
