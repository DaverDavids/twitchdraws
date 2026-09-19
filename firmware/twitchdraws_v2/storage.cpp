#include "storage.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>

#include <stdio.h>
#include <string.h>

// Builds "/draws/<username>.bin" into a caller-provided buffer.
static void draw_path(const char* username, char* out, size_t out_len) {
    snprintf(out, out_len, "%s/%s.bin", DIR_DRAWS, username);
}

static void date_path(const char* username, char* out, size_t out_len) {
    snprintf(out, out_len, "%s/%s.date", DIR_META, username);
}

bool storage_mount() {
    // begin(true) formats the volume on first boot; without this a brand-new
    // device would fail to mount forever and brick at startup.
    if (!LittleFS.begin(true)) {
        Serial.println("[storage] ERROR: failed to mount LittleFS");
        return false;
    }
    return storage_ensure_dirs();
}

bool storage_ensure_dirs() {
    if (!LittleFS.exists(DIR_DRAWS)) {
        if (!LittleFS.mkdir(DIR_DRAWS)) {
            Serial.println("[storage] ERROR: cannot create /draws");
            return false;
        }
    }
    if (!LittleFS.exists(DIR_META)) {
        if (!LittleFS.mkdir(DIR_META)) {
            Serial.println("[storage] ERROR: cannot create /meta");
            return false;
        }
    }
    return true;
}

bool storage_read_drawing(const char* username, uint8_t out[PIXEL_BYTES]) {
    char path[64 + sizeof(DIR_DRAWS) + 4];
    draw_path(username, path, sizeof(path));

    File f = LittleFS.open(path, "r");
    if (!f) {
        return false;  // new user => caller starts from a zeroed buffer
    }
    size_t got = f.read(out, PIXEL_BYTES);
    f.close();
    if (got != PIXEL_BYTES) {
        Serial.printf("[storage] WARN: short read on %s (%u/%d)\n",
                      path, (unsigned)got, PIXEL_BYTES);
        // Treat as missing; caller will zero-fill.
        return false;
    }
    return true;
}

bool storage_write_drawing(const char* username, const uint8_t data[PIXEL_BYTES]) {
    char path[64 + sizeof(DIR_DRAWS) + 4];
    draw_path(username, path, sizeof(path));

    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf("[storage] ERROR: cannot open %s for write\n", path);
        return false;
    }
    size_t wrote = f.write(data, PIXEL_BYTES);
    f.close();
    if (wrote != PIXEL_BYTES) {
        Serial.printf("[storage] ERROR: short write on %s (%u/%d)\n",
                      path, (unsigned)wrote, PIXEL_BYTES);
        return false;
    }
    return true;
}

bool storage_load_userlist(char list[MAX_USERS][MAX_UNAME_LEN], int* count) {
    *count = 0;
    if (!LittleFS.exists(PATH_USERLIST)) {
        return true;
    }
    File f = LittleFS.open(PATH_USERLIST, "r");
    if (!f) {
        return true;
    }
    char line[MAX_UNAME_LEN];
    int n = 0;
    while (f.available() && n < MAX_USERS) {
        size_t got = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[got] = '\0';
        // strip trailing \r
        while (got > 0 && (line[got - 1] == '\r' || line[got - 1] == '\n')) {
            line[--got] = '\0';
        }
        if (got == 0) {
            continue;
        }
        strncpy(list[n], line, MAX_UNAME_LEN - 1);
        list[n][MAX_UNAME_LEN - 1] = '\0';
        ++n;
    }
    f.close();
    *count = n;
    return true;
}

// Returns true if name_a matches name_b case-insensitively (Twitch usernames
// are case-insensitive).
static bool name_eq(const char* a, const char* b) {
    return strcasecmp(a, b) == 0;
}

bool storage_userlist_touch(const char* username,
                            char out_list[MAX_USERS][MAX_UNAME_LEN],
                            int* out_count) {
    // 1. Load current list.
    int count = 0;
    char cur[MAX_USERS][MAX_UNAME_LEN];
    storage_load_userlist(cur, &count);

    // 2. Prepend the user, then carry over the rest (without duplicates).
    int n = 0;
    strncpy(out_list[n], username, MAX_UNAME_LEN - 1);
    out_list[n][MAX_UNAME_LEN - 1] = '\0';
    ++n;
    for (int i = 0; i < count && n < MAX_USERS; ++i) {
        if (name_eq(cur[i], username)) {
            continue;
        }
        strncpy(out_list[n], cur[i], MAX_UNAME_LEN - 1);
        out_list[n][MAX_UNAME_LEN - 1] = '\0';
        ++n;
    }

    // 3. Rewrite the file.
    File f = LittleFS.open(PATH_USERLIST, "w");
    if (!f) {
        Serial.println("[storage] ERROR: cannot open userlist for write");
        return false;
    }
    for (int i = 0; i < n; ++i) {
        f.print(out_list[i]);
        f.print('\n');
    }
    f.close();

    *out_count = n;
    return true;
}

bool storage_touch_date(const char* username, const char* iso_date) {
    if (iso_date == NULL || iso_date[0] == '\0') {
        Serial.println("[storage] WARN: touch_date() called with empty iso_date (NTP not synced yet)");
        return true;
    }

    char path[64 + sizeof(DIR_META) + 4];
    date_path(username, path, sizeof(path));
    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf("[storage] ERROR: cannot write %s\n", path);
        return false;
    }
    f.print(iso_date);
    f.close();
    Serial.printf("[storage] date touched: %s -> %s\n", username, iso_date);
    return true;
}

void storage_logStats() {
    // Simple filesystem sanity for the periodic stats print.
    Serial.printf("[storage] total_bytes=%lu used_bytes=%lu\n",
                  (unsigned long)LittleFS.totalBytes(),
                  (unsigned long)LittleFS.usedBytes());
}