#pragma once

#include <Arduino.h>

// OTA (ArduinoOTA) + mDNS hostname advertisement.
//
// Design (scope.md Future Extension Points -> "OTA updates", promoted to core
// build at user's direction; also the mDNS hostname referenced by scope.md
// Connectivity "WiFi station mode" + mDNS hostname bullet):
//   - mDNS: config `hostname=<name>` (default "twitchdraws") -> <name>.local.
//     Same wifi references as v1 — board is a station, mDNS hostname usable by
//     other LAN clients/resources exactly like v1's WLED mDNS presence.
//   - OTA serviced in a LOW-priority FreeRTOS task (prio 1 < display/parser)
//     and ONLY while the display is in scroll mode (MODE_SCROLL). That keeps an
//     in-flight flash from landing mid FastLED.show() -> no display glitches
//     (scope.md: "run only during scroll mode to avoid display glitches").
//   - Optional password from config `ota_password`; ArduinoOTA.setPassword()
//     only when non-empty (empty -> unpassworded default, acceptable for a
//     LAN-only ota_enabled=true; document in README).
//
// Wiring: ota_init() once after WiFi connects in setup(); xTaskCreatePinnedToCore
// for task_ota in the same block (Core 0 / APP_CORE, low prio).

void ota_init();      // after WiFi: MDNS.begin + ArduinoOTA.begin (+ password)
void task_ota(void*); // FreeRTOS task: ArduinoOTA.handle() only in scroll mode

#define OTA_HANDLE_MS 250
#define OTA_STACK     4096
#define OTA_PRIO      1      // below DISPLAY_PRIO / PARSER_PRIO (3)
