#include "config.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

AppConfig g_cfg;

static void trim(char* s) {
    char* p = s;
    while (isspace((unsigned char)*p)) {
        ++p;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

// Splits one config line into key and value. Returns false for blank/comment.
static bool parse_line(char* line, char key[], size_t key_len, char value[], size_t value_len) {
    trim(line);
    if (*line == '\0' || *line == '#') {
        return false;
    }
    char* eq = strchr(line, '=');
    if (eq == NULL) {
        return false;  // malformed line; ignore (matches key=value format)
    }
    *eq = '\0';
    strncpy(key, line, key_len - 1);
    key[key_len - 1] = '\0';
    trim(key);
    strncpy(value, eq + 1, value_len - 1);
    value[value_len - 1] = '\0';
    trim(value);
    return key[0] != '\0';
}

static int parse_int_clamped(const char* s, int fallback, int lo, int hi) {
    if (s == NULL || *s == '\0') {
        return fallback;
    }
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) {
        return fallback;
    }
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)v;
}

static void set_defaults() {
    memset(&g_cfg, 0, sizeof(g_cfg));
    strcpy(g_cfg.ssid, "");
    strcpy(g_cfg.password, "");
    strcpy(g_cfg.channel, "");
    g_cfg.oauth_mode = false;
    strcpy(g_cfg.nick, "justinfan12345");
    strcpy(g_cfg.oauth_token, "");
    strcpy(g_cfg.oauth_nick, "");
    g_cfg.scroll_speed = 6;
    g_cfg.led_brightness = 128;
}

bool config_load(const char* path) {
    set_defaults();

    File cfg = LittleFS.open(path, "r");
    if (!cfg) {
        Serial.printf("[config] ERROR: missing %s\n", path);
        return false;
    }

    char line[256];
    while (cfg.available()) {
        size_t n = cfg.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        if (n > 0 && line[n - 1] == '\r') {
            line[n - 1] = '\0';
        }

        char key[64];
        char value[256];
        if (!parse_line(line, key, sizeof(key), value, sizeof(value))) {
            continue;
        }

        if (strcasecmp(key, "ssid") == 0) {
            strncpy(g_cfg.ssid, value, sizeof(g_cfg.ssid) - 1);
        } else if (strcasecmp(key, "password") == 0) {
            strncpy(g_cfg.password, value, sizeof(g_cfg.password) - 1);
        } else if (strcasecmp(key, "channel") == 0) {
            strncpy(g_cfg.channel, value, sizeof(g_cfg.channel) - 1);
        } else if (strcasecmp(key, "twitch_mode") == 0) {
            g_cfg.oauth_mode = (strcasecmp(value, "oauth") == 0);
        } else if (strcasecmp(key, "nick") == 0) {
            strncpy(g_cfg.nick, value, sizeof(g_cfg.nick) - 1);
        } else if (strcasecmp(key, "oauth") == 0) {
            strncpy(g_cfg.oauth_token, value, sizeof(g_cfg.oauth_token) - 1);
        } else if (strcasecmp(key, "oauth_nick") == 0) {
            strncpy(g_cfg.oauth_nick, value, sizeof(g_cfg.oauth_nick) - 1);
        } else if (strcasecmp(key, "scroll_speed") == 0) {
            g_cfg.scroll_speed = parse_int_clamped(value, 6, 1, 60);
        } else if (strcasecmp(key, "led_brightness") == 0) {
            g_cfg.led_brightness = parse_int_clamped(value, 128, 0, 255);
        }
    }
    cfg.close();

    // Normalize channel into usable IRC form while keeping the display-ready copy.
    if (g_cfg.channel[0] != '\0' && g_cfg.channel[0] != '#') {
        char tmp[sizeof(g_cfg.channel)];
        strncpy(tmp, g_cfg.channel, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        snprintf(g_cfg.channel, sizeof(g_cfg.channel), "#%s", tmp);
    }

    // Validation (scope.md: halt with Serial error if required keys absent).
    if (g_cfg.ssid[0] == '\0' || g_cfg.password[0] == '\0' || g_cfg.channel[0] == '\0') {
        Serial.println("[config] ERROR: ssid, password and channel are required");
        return false;
    }
    if (g_cfg.oauth_mode && (g_cfg.oauth_token[0] == '\0' || g_cfg.oauth_nick[0] == '\0')) {
        Serial.println("[config] ERROR: twitch_mode=oauth requires 'oauth' and 'oauth_nick'");
        return false;
    }

    Serial.printf("[config] loaded: channel=%s mode=%s scroll=%d brightness=%d\n",
                  g_cfg.channel, g_cfg.oauth_mode ? "oauth" : "anonymous",
                  g_cfg.scroll_speed, g_cfg.led_brightness);
    return true;
}