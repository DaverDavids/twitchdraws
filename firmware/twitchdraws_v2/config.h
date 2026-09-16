#pragma once

#include <stdbool.h>

#include "state.h"

// Runtime configuration, parsed once from /config.txt at boot. Key=value lines,
// '#' comments, see scope.md Configuration section and config.txt.example.
typedef struct {
    // WiFi
    char ssid[MAX_UNAME_LEN];
    char password[MAX_UNAME_LEN];
    char hostname[MAX_UNAME_LEN];  // mDNS hostname, e.g. "twitchdraws" -> twitchdraws.local

    // OTA (ArduinoOTA; low-priority task, only serviced during scroll mode)
    bool ota_enabled;
    char ota_password[MAX_UNAME_LEN];

    // Twitch
    char channel[MAX_UNAME_LEN];  // without '#'; normalized to '#<channel>' internally
    bool oauth_mode;              // false -> anonymous justinfan login
    char nick[MAX_UNAME_LEN];     // anonymous login nick (justinfanNNNNN)
    char oauth_token[128];        // "oauth:<token>", only used when oauth_mode
    char oauth_nick[MAX_UNAME_LEN];

    // Display
    int scroll_speed;             // columns per second (clamped to [1, 60])
    int led_brightness;           // FastLED global brightness 0..255
} AppConfig;

extern AppConfig g_cfg;

// Loads and validates /config.txt (or the given path). Returns false and
// prints the reason to Serial when the file is missing or malformed;
// setup() halts in that case per scope.md.
bool config_load(const char* path);