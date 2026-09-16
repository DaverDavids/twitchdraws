#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "state.h"

// File layout (scope.md "File Storage Layout (LittleFS)"):
//   /draws/<username>.bin   - 3072-byte R,G,B drawing per user
//   /meta/userlist.txt      - newline-separated usernames, most-recent-first
//   /meta/<username>.date   - ISO date (YYYY-MM-DD) of last interaction
//   /config.txt             - runtime config (parsed by config.cpp)
#define DIR_DRAWS       "/draws"
#define DIR_META        "/meta"
#define PATH_CONFIG     "/config.txt"
#define PATH_USERLIST   "/meta/userlist.txt"

// Mounts LittleFS (auto-format on first boot). Returns false if mounting and
// formatting both fail; setup() halts in that case per scope.md.
bool storage_mount();

// Ensures /draws and /meta exist.
bool storage_ensure_dirs();

// Reads a user's drawing into `out` (PIXEL_BYTES bytes, R G B per LED in
// LOGICAL index order). Returns false if the file does not exist; in that case
// `out` is left untouched so callers can decide to start from a zeroed buffer.
bool storage_read_drawing(const char* username, uint8_t out[PIXEL_BYTES]);

// Writes a full drawing (PIXEL_BYTES bytes). Returns false on write error.
bool storage_write_drawing(const char* username, const uint8_t data[PIXEL_BYTES]);

// Reads /meta/userlist.txt into `list`/`count`. Missing file => count = 0.
bool storage_load_userlist(char list[MAX_USERS][MAX_UNAME_LEN], int* count);

// Removes an existing entry for `username` (if any), prepends it to the top,
// rewrites /meta/userlist.txt, and fills `out_list`/`out_count`.
// Returns false only on file-write failure.
bool storage_userlist_touch(const char* username,
                            char out_list[MAX_USERS][MAX_UNAME_LEN],
                            int* out_count);

// Records the last-interaction date (YYYY-MM-DD) for a user.
// NOTE: date source (NTP) is not yet implemented; writes are deferred until
// time sync exists. No-op for now.
bool storage_touch_date(const char* username, const char* iso_date);

// Serial log helper for diagnostics used by multiple tasks.
void storage_logStats();