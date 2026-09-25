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

// Prüfabstand. Bis zur Reparatur sieht millis() den falschen Wert — kurz halten.
static constexpr uint32_t MK_CLOCK_CHECK_MS     = 50;
// Kleinster möglicher Sprung ist Bit 26 = 2^26 / 16 MHz ≈ 4.2 s. Der Tick selbst
// verliert nach langen Interrupt-Sperren höchstens ein paar hundert ms.
static constexpr int32_t  MK_CLOCK_THRESHOLD_MS = 1000;

static volatile uint32_t gClockRepairs = 0;

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

static void mkClockGuardBegin() {
    // Priorität über der Arduino-Loop (1), damit die Reparatur nicht wartet.
    xTaskCreate(mkClockGuardTask, "clock_guard", 3072, nullptr, 5, nullptr);
}
