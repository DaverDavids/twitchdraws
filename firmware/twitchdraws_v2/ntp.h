#pragma once

#include <Arduino.h>

#include <stdbool.h>
#include <time.h>

// NTP / RTC seam — promotes the `.date` file writes from "deferred until a
// time source exists" (storage.cpp historic TODO; scope.md Known Constraints
// item 03 "NTP time source"). This module is deliberately tiny and split the
// same way as parser/colors/storage so the pure date formatter is
// host-unit-testable (tests/ntp_test.cpp) while the ESP32-only configTime()
// call hides in ntp.cpp behind `#if defined(ESP32)`.

// Becomes true once `time()` returns an epoch >= 2000-01-01T00:00:00Z
// (ESP32 boots into 1970; configTime() with a synced pool pushes it past
// 2000). Cheap; called whenever a .date write is about to happen.
bool ntp_time_valid();

// Pure UTC -> "YYYY-MM-DD" into out[11]. Returns false (and does not touch
// out) when t is before year 2000 so an unsynced RTC can never stamp a bogus
// .date. NOT locked — formatter only.
bool ntp_iso_date(time_t t, char out[11]);

// Convenience: "today" from the last valid epoch seen by task_ntp. Returns
// false until the RTC has synced at least once this boot.
bool ntp_today(char out[11]);

// ESP32-only: configTime(UTC, pool.ntp.org). No-op on host builds.
void ntp_init();

// FreeRTOS task (low prio, see scope.md task architecture / ntp.cpp header):
// waits for a post-2000 epoch, caches it for ntp_today(), then polls hourly.
// Never blocks the IRC/parser path.
void task_ntp(void*);
