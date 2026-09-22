#include "ntp.h"

#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#if defined(ESP32)
#include <esp_sntp.h>
#include <WiFi.h>
#endif

// ===========================================================================
// NTP / UTC gate — implements EXACTLY the five symbols in ntp.h:
//   ntp_time_valid()   gate: reached 2000-XX epoch this boot (RTC synced)
//   ntp_iso_date()     PURE time_t -> "YYYY-MM-DD" (UTC), host-unit-tested
//   ntp_today()        cached UTC date from the last valid epoch (for .date)
//   ntp_init()         ESP32-only configTime(); no-op on host builds
//   task_ntp()         FreeRTOS task: keepalive, re-seeds cache, never blocks
// ===========================================================================

static bool   s_rtc_valid = false;
static time_t s_last_valid_epoch = 0;
static bool epoch_post_2000(time_t t) {
    // ESP32 boots into 1970. 2000-01-01T00:00:00Z == 946684800; anything
    // below that means configTime()/RTC hasn't advanced past boot-time.
    return t >= (time_t)946684800;
}

// Gate. First call that observes a post-2000 epoch latches it (s_rtc_valid,
// s_last_valid_epoch); cheap, so storage/parser can call it on every .date
// write without a mutex cost that matters.
bool ntp_time_valid() {
    if (s_rtc_valid) {
        return true;
    }
#if defined(ESP32)
    time_t t = time(NULL);
#else
    time_t t = (time_t)s_last_valid_epoch;  // host tests seed this directly
#endif
    if (epoch_post_2000(t)) {
        s_rtc_valid = true;
        s_last_valid_epoch = t;
        return true;
    }
    return false;
}

// Pure UTC -> "YYYY-MM-DD" into out[11]. Returns false (and does not touch
// out) when t < 2000 so an unsynced RTC can never stamp a bogus .date
// (scope.md: only write .date when time is valid). NOT locked — formatter.
bool ntp_iso_date(time_t t, char out[11]) {
    if (!epoch_post_2000(t)) {
        return false;
    }
    struct tm g;
    gmtime_r(&t, &g);
    snprintf(out, 11, "%04d-%02d-%02d",
             g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    return true;
}

// "Today" from the last valid epoch seen by task_ntp. Returns false until the
// RTC has synced at least once this boot.
bool ntp_today(char out[11]) {
    if (!ntp_time_valid()) {
        return false;
    }
    return ntp_iso_date(s_last_valid_epoch, out);
}

void ntp_init() {
#if defined(ESP32)
    // UTC on purpose (see ntp.h): the .date format is timezone-free; a viewer
    // converts to its local day at render time (scope.md File Storage Layout).
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    Serial.println("[ntp] configTime (UTC pool.ntp.org / time.google.com)");
#else
    (void)0;  // host builds: no hardware time calls.
#endif
}

void task_ntp(void* arg) {
    (void)arg;
    bool logged = false;
    for (;;) {
        if (ntp_time_valid()) {
            char d[11];
            if (!logged && ntp_today(d)) {
                logged = true;
                Serial.printf("[ntp] RTC synced, today=%s\n", d);
            }
            // configTime() keeps the RTC fed in the background (lwIP sntp);
            // nothing more to do on each tick.
        }
        vTaskDelay(pdMS_TO_TICKS(60000));  // 1/min keepalive; never blocks IRC
    }
}
