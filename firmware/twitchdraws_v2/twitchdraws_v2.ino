// TwitchDraws v2 — ESP32 firmware.
// Self-contained replacement for the Python + WLED v1 server. Runs entirely on
// an ESP32-WROOM-32: Twitch IRC chat in, per-user pixel drawing stored on
// LittleFS, rendered to a 64x16 (1024 LED) WS2812B matrix via FastLED.
//
// Design reference: scope.md (see git branch esp32-v2). This file wires the
// startup sequence and the four FreeRTOS tasks defined there.
//
// Open items deferred to hardware bring-up (scope.md "Known Constraints"):
//   * LED_DATA_PIN          — confirm GPIO before first flash
//   * panel chain order     — verify xy_to_led_index() with a test sketch
//   * LittleFS partition    — set in Arduino IDE / partitions.csv
//   * NTP/time              — .date files written once time sync exists
//   * WLED config review    — gamma/temperature equivalents in FastLED

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>

#include <string.h>

#include "config.h"
#include "display.h"
#include "ota.h"
#include "parser.h"
#include "state.h"
#include "storage.h"
#include "twitch.h"

AppState g_state;
SemaphoreHandle_t g_state_mutex = NULL;

static void halt(const char* msg) {
    Serial.println(msg);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void setup() {
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(500));  // allow USB serial to attach for boot logs

    // 1. Filesystem (scope.md Startup 2-3).
    if (!storage_mount()) {
        halt("[boot] FATAL: filesystem unavailable - halting");
    }

    // 2. Config (scope.md Startup 4). Missing/malformed config halts.
    if (!config_load(PATH_CONFIG)) {
        halt("[boot] FATAL: config invalid - halting");
    }

    // 3. Initial AppState (scope.md Startup 5).
    memset(&g_state, 0, sizeof(g_state));
    g_state.mode = MODE_SCROLL;
    storage_load_userlist(g_state.userlist, &g_state.user_count);

    g_state_mutex = xSemaphoreCreateMutex();
    if (g_state_mutex == NULL) {
        halt("[boot] FATAL: cannot create state mutex");
    }

    // 4. LED matrix init + blank-on-boot (scope.md Startup 6-8).
    display_init();

    // 5. WiFi (scope.md Startup 9): blocking connect, 30s timeout, restart.
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_cfg.ssid, g_cfg.password);
    Serial.print("[boot] connecting to WiFi");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > 30000) {
            Serial.println();
            Serial.println("[boot] FATAL: WiFi timeout - restarting");
            vTaskDelay(pdMS_TO_TICKS(500));
            ESP.restart();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print('.');
    }
    Serial.println();
    Serial.printf("[boot] WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());

    // 5b. mDNS hostname + OTA (scope.md OTA bullet, promoted from Future).
    ota_init();

    // 5c. NTP/RTC (scope.md Known Constraints item 03 "NTP/time" promoted to
    // core). Seeds UTC time after WiFi; task_ntp feeds keepalive so .date
    // writes below only ever stamp real interaction days.
    ntp_init();
    xTaskCreatePinnedToCore(task_ntp, "ntp",
                            NTP_STACK, NULL, NTP_PRIO, NULL, APP_CORE);

    // 6. IRC line queue + tasks (scope.md Startup 10-11, Task Architecture).
    twitch_create_queue();

    xTaskCreatePinnedToCore(task_wifi_watch, "wifi_watch",
                            WIFI_WATCH_STACK, NULL, WIFI_WATCH_PRIO, NULL, RX_CORE);
    xTaskCreatePinnedToCore(task_twitch_rx, "twitch_rx",
                            TWITCH_STACK_RX, NULL, TWITCH_PRIO_RX, NULL, RX_CORE);
    xTaskCreatePinnedToCore(task_cmd_parser, "cmd_parser",
                            PARSER_STACK, NULL, PARSER_PRIO, NULL, APP_CORE);
    xTaskCreatePinnedToCore(task_display, "display",
                            DISPLAY_STACK, NULL, DISPLAY_PRIO, NULL, APP_CORE);
    xTaskCreatePinnedToCore(task_ota, "ota",
                            OTA_STACK, NULL, OTA_PRIO, NULL, APP_CORE);

    Serial.println("[boot] tasks started");
}

// Background loop: periodic diagnostics. Task WDT feeding happens inside each
// task's own vTaskDelay; here we just pace the stats prints (scope.md Startup
// step 12).
void loop() {
    static uint32_t s_last_stats = 0;
    if (millis() - s_last_stats >= 60000) {
        s_last_stats = millis();
        STATE_LOCK();
        uint32_t dropped = g_state.stat_dropped_lines;
        uint32_t reconns = g_state.stat_twitch_reconnects;
        int nusers = g_state.user_count;
        STATE_UNLOCK();
        storage_logStats();
        Serial.printf("[stats] users=%d dropped_lines=%lu reconnects=%lu\n",
                      nusers, (unsigned long)dropped, (unsigned long)reconns);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}