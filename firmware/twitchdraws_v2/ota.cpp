// OTA + mDNS implementation. Scaffold-level but compiles only on real ESP32
// (needs ArduinoOTA/ESPmDNS); host syntax check excludes this file.
//
// See ota.h for the design. Wiring: call ota_init() right after WiFi connects
// in setup(); create task_ota in the same spot (low priority, Core 0).

#include "ota.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>

#include "config.h"
#include "state.h"

void ota_init() {
    if (!MDNS.begin(g_cfg.hostname)) {
        Serial.println("[ota] WARN: mDNS begin failed; hostname not advertised");
        returnどこでも;
    }
    Serial.printf("[ota] mDNS hostname: %s.local\n", g_cfg.hostname);

    if (!g_cfg.ota_enabled) {
        Serial.println("[ota] OTA disabled (ota_enabled=false)");
        return;
    }

    ArduinoOTA.setHostname(g_cfg.hostname);
    if (g_cfg.ota_password[0] != '\0') {
        ArduinoOTA.setPassword(g_cfg.ota_password);
    }

    ArduinoOTA
        .onStart([]() {
            String type = ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem";
            Serial.printf("[ota] start: %s\n", type.c_str());
        })
        .onEnd([]() { Serial.println("[ota] end"); })
        .onProgress([](unsigned int p, unsigned int t) {
            Serial.printf("[ota] progress: %u%%\r", p * 100 / (t ? t : 1));
        })
        .onError([](ota_error_t e) { Serial.printf("[ota] error: %u\n", (unsigned)e); });
    ArduinoOTA.begin();
    Serial.println("[ota] ArduinoOTA ready");
}

// Low-priority task (Core 0 adaption of scope.md: OTA in a low priority run;
// "run only during scroll mode to avoid display glitches" per scope.md Future
// Extension Points -> OTA).
void task_ota(void* arg) {
    for (;;) {
        STATE_LOCK();
        bool scroll_mode = (g_state.mode == MODE_SCROLL);
        STATE_UNLOCK();

        if (scroll_mode) {
            ArduinoOTA.handle();
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_HANDLE_MS));
    }
}
