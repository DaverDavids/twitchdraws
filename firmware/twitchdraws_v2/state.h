#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ---------------------------------------------------------------------------
// Global display geometry / limits (matches scope.md and v1 Python script)
// ---------------------------------------------------------------------------
#define WIDTH              64
#define HEIGHT             16
#define LED_COUNT          (WIDTH * HEIGHT)      // 1024
#define PIXEL_BYTES        (WIDTH * HEIGHT * 3)  // 3072, R G B per LED

#define MAX_USERS          200
#define MAX_UNAME_LEN      64

#define FOCUS_DURATION_MS  6000

// Physical WS2812B data pin. TODO(scope.md open item): confirm with hardware
// wiring before first flash.
#ifndef LED_DATA_PIN
#define LED_DATA_PIN       26
#endif

// ---------------------------------------------------------------------------
// Display modes
// ---------------------------------------------------------------------------
typedef enum {
    MODE_SCROLL = 0,
    MODE_FOCUS
} DisplayMode;

// ---------------------------------------------------------------------------
// AppState: the single mutex-guarded shared state blob (scope.md spec)
// ---------------------------------------------------------------------------
typedef struct {
    DisplayMode mode;

    // --- Scroll state ---
    int scroll_user_index;    // current user index being scrolled
    int scroll_col_offset;    // columns scrolled so far for current user (0..63)

    // --- Focus state ---
    char focus_username[MAX_UNAME_LEN];
    uint32_t focus_end_ms;                    // millis() when focus expires
    char queued_username[MAX_UNAME_LEN];      // empty string = no queued user

    // --- User registry ---
    char userlist[MAX_USERS][MAX_UNAME_LEN];  // scroll order, [0] = most recent
    int  user_count;

    // --- Display frame buffer ---
    // Packed 0x00RRGGBB per pixel, indexed by LOGICAL pixel order
    // ((HEIGHT - y) * WIDTH + (x - 1)). Written only by task_display.
    uint32_t framebuf[LED_COUNT];

    // --- Dirty flag (cmd_parser -> display task) ---
    bool drawing_dirty;
    char dirty_username[MAX_UNAME_LEN];

    // --- Diagnostics ---
    uint32_t stat_dropped_lines;    // IRC lines dropped due to queue overflow
    uint32_t stat_twitch_reconnects;
} AppState;

extern AppState g_state;
extern SemaphoreHandle_t g_state_mutex;

#define STATE_LOCK()     xSemaphoreTake(g_state_mutex, portMAX_DELAY)
#define STATE_UNLOCK()   xSemaphoreGive(g_state_mutex)