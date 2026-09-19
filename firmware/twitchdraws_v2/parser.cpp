#include "parser.h"

#include <Arduino.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "colors.h"
#include "config.h"
#include "state.h"
#include "storage.h"
#include "twitch.h"
#include "ntp.h"

// Max coordinate pairs honored per single message (v1 piled up every pair).
#define MAX_COORDS 128

// The drawing working buffer lives at file scope (not on the 6 KB task stack);
// task_cmd_parser is the ONLY writer and is a single task.
static uint8_t s_drawing[PIXEL_BYTES];

// Mirrors v1 xy_to_index() exactly: x 1..64 left->right, y 1..16 bottom->top.
static int idx_from_xy(int x, int y) {
    return (HEIGHT - y) * WIDTH + (x - 1);
}

static bool starts_with_ignore_case(const char* s, const char* prefix) {
    return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// Converts a 6-hex-digit string to a packed 0x00RRGGBB value (m must be valid).
static uint32_t hex6_to_rgb(const char* h) {
    uint32_t v = 0;
    for (int i = 0; i < 6; ++i) {
        char c = h[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (uint32_t)(c - '0');
        } else {
            v |= (uint32_t)(c - 'a' + 10);
        }
    }
    return v;
}

// Scans for the FIRST color token in the (already lowercased) message, exactly
// like the v1 regex `#?[0-9a-f]{6}|\b(<names>)\b`. Returns false when absent.
static bool scan_color(const char* msg, uint32_t* out) {
    int best = -1;
    uint32_t best_val = 0;

    // Named colors, requiring word boundaries both sides.
    for (int i = 0; i < kNamedColorCount; ++i) {
        const char* name = kNamedColors[i].name;
        size_t len = strlen(name);
        const char* p = msg;
        while ((p = strstr(p, name)) != NULL) {
            bool prev_ok = (p == msg) || !isalnum((unsigned char)p[-1]);
            bool next_ok = !isalnum((unsigned char)p[len]);
            if (prev_ok && next_ok) {
                int pos = (int)(p - msg);
                if (best < 0 || pos < best) {
                    best = pos;
                    best_val = kNamedColors[i].value;
                }
                break;  // a given name can only match at its first occurrence
            }
            ++p;
        }
    }

    // Hex: optional '#', then 6 hex digits. Must not be part of a longer
    // hex run (keeps "1234567" from matching at the first 6 digits).
    size_t mlen = strlen(msg);
    for (size_t i = 0; i < mlen; ++i) {
        const char* h = &msg[i];
        const char* digits = h;
        if (*digits == '#') {
            ++digits;
        }
        if ((size_t)(digits - h) + 6 > mlen - i) {
            continue;
        }
        bool ok = true;
        for (int k = 0; k < 6; ++k) {
            if (!is_hex_digit(digits[k])) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;
        }
        // Ensure a full 6-digit run (guard the previous + next chars).
        int pos = (int)(digits - msg);
        bool prev_hex = (pos > 0) && is_hex_digit(msg[pos - 1]);
        bool next_hex = ((size_t)(pos + 6) < mlen) && is_hex_digit(msg[pos + 6]);
        if (prev_hex || next_hex) {
            continue;
        }
        if (best < 0 || pos < best) {
            best = pos;
            best_val = hex6_to_rgb(digits);
        }
    }

    if (best < 0) {
        return false;
    }
    *out = best_val;
    return true;
}

// Advances *p over a run of decimal digits, returning the value.
static int parse_int_at(const char** p) {
    int v = 0;
    const char* s = *p;
    while ((unsigned char)*s >= '0' && (unsigned char)*s <= '9') {
        v = v * 10 + (*s - '0');
        ++s;
    }
    *p = s;
    return v;
}

// Scans ALL "digits,digits" pairs, mirroring v1's `(\d+)\s*,\s*(\d+)` findall.
static int scan_coords(const char* msg, int xs[], int ys[], int maxc) {
    int n = 0;
    const char* p = msg;
    while (*p && n < maxc) {
        if (isdigit((unsigned char)*p)) {
            int a = parse_int_at(&p);
            while (isspace((unsigned char)*p)) ++p;
            if (*p == ',') {
                ++p;
                while (isspace((unsigned char)*p)) ++p;
                if (isdigit((unsigned char)*p)) {
                    int b = parse_int_at(&p);
                    if (a >= 1 && a <= WIDTH && b >= 1 && b <= HEIGHT) {
                        xs[n] = a;
                        ys[n] = b;
                        ++n;
                    }
                    // out-of-bounds pairs are silently ignored (matches v1)
                }
            }
        } else {
            ++p;
        }
    }
    return n;
}

// Handles one !pixel message. Returns true if disk + AppState were updated.
static bool handle_pixel_command(const char* username, const char* message) {
    char buf[TWITCH_QUEUE_ITEM];
    strncpy(buf, message, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // Command text is compared case-insensitively (v1 lowercases everything).
    for (char* c = buf; *c; ++c) {
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    }

    uint32_t color = 0;
    if (!scan_color(buf, &color)) {
        return false;
    }

    int xs[MAX_COORDS];
    int ys[MAX_COORDS];
    int ncoords = scan_coords(buf, xs, ys, MAX_COORDS);
    if (ncoords == 0) {
        return false;
    }

    // Load existing drawing, or start from black for a new user.
    bool existed = storage_read_drawing(username, s_drawing);
    if (!existed) {
        memset(s_drawing, 0, sizeof(s_drawing));
    }
    for (int i = 0; i < ncoords; ++i) {
        int idx = idx_from_xy(xs[i], ys[i]);
        s_drawing[idx * 3 + 0] = (uint8_t)(color >> 16);
        s_drawing[idx * 3 + 1] = (uint8_t)(color >> 8);
        s_drawing[idx * 3 + 2] = (uint8_t)(color);
    }

    if (!storage_write_drawing(username, s_drawing)) {
        Serial.printf("[parser] write failed for %s\n", username);
        // Keep in-RAM state intact; do not crash (scope.md Error Handling).
        return false;
    }

    // Touch last-interaction date — now REAL via ntp.cpp's pure UTC -> ISO
    // formatter (ntp.h/ntp.cpp, promoted from the storage.cpp seam; scope.md
    // File Storage Layout bullets 2 + Known Constraints 03). UTC day is on
    // purpose (ntp.h): the .date format is timezone-free; a viewer converts at
    // render time, so a draw at 23:30 UTC keeps its intended day.
    char iso[11];
    if (ntp_today(iso)) {
        storage_touch_date(username, iso);
        Serial.printf("[parser] .date=%s touching username=%s\n", iso, username);
    } else {
        // RTC not synced yet this boot — defer (scope.md: only write .date
        // when time is valid). Events still register; the .date back-fill can
        // be re-stamped later (storage_touch_date is idempotent per day).
        Serial.printf("[parser] .date deferred (RTC unsynced) username=%s\n",
                      username);
    }

    // Update userlist (remove existing, prepend, rewrite) and mirror into AppState.
    char newlist[MAX_USERS][MAX_UNAME_LEN];
    int newcount = 0;
    if (!storage_userlist_touch(username, newlist, &newcount)) {
        return false;
    }

    STATE_LOCK();
    g_state.user_count = newcount;
    for (int i = 0; i < newcount; ++i) {
        strncpy(g_state.userlist[i], newlist[i], MAX_UNAME_LEN - 1);
        g_state.userlist[i][MAX_UNAME_LEN - 1] = '\0';
    }
    strncpy(g_state.dirty_username, username, MAX_UNAME_LEN - 1);
    g_state.dirty_username[MAX_UNAME_LEN - 1] = '\0';
    g_state.drawing_dirty = true;
    STATE_UNLOCK();

    Serial.printf("[parser] %s (%06lx) -> %d pixel(s)\n",
                  username, (unsigned long)color, ncoords);
    return true;
}

void task_cmd_parser(void* pv) {
    (void)pv;
    char line[TWITCH_QUEUE_ITEM];

    for (;;) {
        if (xQueueReceive(g_twitch_line_queue, line, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // PING keepalive: respond immediately (scope.md).
        if (starts_with_ignore_case(line, "PING")) {
            twitch_send_line("PONG :tmi.twitch.tv");
            continue;
        }

        char username[MAX_UNAME_LEN];
        char message[TWITCH_QUEUE_ITEM];
        if (!twitch_parse_privmsg(line, username, message)) {
            continue;  // JOIN/NOTICE/etc. are not commands
        }

        // !pixel command trigger: v1 used `^!pixel\b`, so "!pixel" must be a
        // full word (followed by whitespace or end-of-message).
        if (strncasecmp(message, "!pixel", 6) == 0) {
            char c = message[6];
            if (c == '\0' || isspace((unsigned char)c)) {
                handle_pixel_command(username, message);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Host-only test seams (PIXEL_TEST). Not compiled on the device; used to drive
// the parser logic from a native host test that mirrors the v1 regex behavior.
// ---------------------------------------------------------------------------
#ifdef PIXEL_TEST
uint32_t test_scan_color(const char* msg) {
    uint32_t c = 0;
    return scan_color(msg, &c) ? c : 0xFFFFFFFFu;
}
int test_scan_coords(const char* msg, int* xs, int* ys, int maxc) {
    return scan_coords(msg, xs, ys, maxc);
}
int test_idx_from_xy(int x, int y) {
    return idx_from_xy(x, y);
}
bool test_handle_pixel_command(const char* username, const char* message) {
    return handle_pixel_command(username, message);
}
#endif