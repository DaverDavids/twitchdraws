#include "display.h"

#include <Arduino.h>
#include <FastLED.h>

#include <string.h>

#include "config.h"
#include "state.h"
#include "storage.h"

// One CRGB per logical LED, in PHYSICAL LED order (via xy_to_led_index).
CRGB leds[LED_COUNT];

// Draw caches. Heap/static size matters: 3 KB x3 buffers, kept off the 6 KB
// task stack.
static uint8_t s_buf_a[PIXEL_BYTES];    // current scroll drawing
static uint8_t s_buf_b[PIXEL_BYTES];    // next scroll drawing
static uint8_t s_render_tmp[PIXEL_BYTES]; // focus / single-user drawing

static int  s_cache_a = -1;             // userlist index cached in s_buf_a
static int  s_cache_b = -1;             // userlist index cached in s_buf_b
static char s_cached_single[MAX_UNAME_LEN] = "";
static uint32_t s_frame_cnt = 0;

// Refresh rate target (scope.md Frame Timing).
static const int kFps = 30;

// Physical LED index mapping (scope.md "Physical LED Index Mapping").
// TODO(scope.md open item): verify serpentine/wiring order with a test sketch.
// Placeholder matches v1 linear mapping.
static int xy_to_led_index(int x, int y) {
    return (HEIGHT - y) * WIDTH + (x - 1);
}

// Copies a 3072-byte R,G,B drawing (logical order) into g_state.framebuf.
static void render_drawing_to_fb(const uint8_t* drawing) {
    for (int i = 0; i < LED_COUNT; ++i) {
        uint32_t r = drawing[i * 3 + 0];
        uint32_t g = drawing[i * 3 + 1];
        uint32_t b = drawing[i * 3 + 2];
        g_state.framebuf[i] = (r << 16) | (g << 8) | b;
    }
}

static void fill_fb_black() {
    memset(g_state.framebuf, 0, sizeof(g_state.framebuf));
}

// Scroll composition (scope.md "Scroll Composition"): blends current drawing
// (scrolled left by col_off) with the next drawing entering from the right.
static void compose_scroll_fb(const uint8_t* cur, const uint8_t* next, int col_off) {
    for (int ly = 1; ly <= HEIGHT; ++ly) {
        for (int c = 0; c < WIDTH; ++c) {
            int logical_col = c + col_off;
            const uint8_t* src;
            if (logical_col < WIDTH) {
                src = cur;
            } else {
                src = next;
                logical_col -= WIDTH;
            }
            int idx = (HEIGHT - ly) * WIDTH + logical_col;
            uint32_t r = src[idx * 3 + 0];
            uint32_t g = src[idx * 3 + 1];
            uint32_t b = src[idx * 3 + 2];
            g_state.framebuf[(HEIGHT - ly) * WIDTH + c] = (r << 16) | (g << 8) | b;
        }
    }
}

// Pushes the logical framebuf out to the physical LEDs (FastLED.show() callers
// still need to call show()).
static void fb_to_leds() {
    for (int i = 0; i < LED_COUNT; ++i) {
        uint32_t c = g_state.framebuf[i];
        int x = (i % WIDTH) + 1;
        int y = HEIGHT - (i / WIDTH);
        leds[xy_to_led_index(x, y)] = CRGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }
}

void display_init() {
    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(leds, LED_COUNT);
    FastLED.setBrightness(g_cfg.led_brightness);
    FastLED.clear();
    FastLED.show();
    Serial.printf("[display] init: %d LEDs, brightness=%d\n", LED_COUNT, g_cfg.led_brightness);
}

void task_display(void* pv) {
    (void)pv;

    int scroll_speed = g_cfg.scroll_speed;
    if (scroll_speed < 1) scroll_speed = 1;
    int frames_per_col = kFps / scroll_speed;
    if (frames_per_col < 1) frames_per_col = 1;

    // Module-local mirrors of the shared state copied out under the mutex.
    DisplayMode mode;
    bool dirty;
    char dirty_user[MAX_UNAME_LEN];
    char focus_user[MAX_UNAME_LEN];
    char queued_user[MAX_UNAME_LEN];
    uint32_t focus_end_ms;
    int user_count, cur_idx, col_off;

    for (;;) {
        // ---- snapshot shared state (keep mutex hold short) ----
        STATE_LOCK();
        mode               = g_state.mode;
        dirty              = g_state.drawing_dirty;
        strncpy(dirty_user,    g_state.dirty_username, MAX_UNAME_LEN - 1); dirty_user[MAX_UNAME_LEN - 1] = '\0';
        strncpy(focus_user,    g_state.focus_username, MAX_UNAME_LEN - 1); focus_user[MAX_UNAME_LEN - 1] = '\0';
        strncpy(queued_user,   g_state.queued_username, MAX_UNAME_LEN - 1); queued_user[MAX_UNAME_LEN - 1] = '\0';
        focus_end_ms       = g_state.focus_end_ms;
        user_count         = g_state.user_count;
        cur_idx            = g_state.scroll_user_index;
        col_off            = g_state.scroll_col_offset;
        STATE_UNLOCK();

        if (user_count > 0) {
            cur_idx = ((cur_idx % user_count) + user_count) % user_count;
            if (col_off < 0) col_off = 0;
            if (col_off >= WIDTH) col_off = 0;
        } else {
            cur_idx = 0;
            col_off = 0;
        }

        // ---- state machine ----
        if (mode == MODE_FOCUS) {
            if (dirty) {
                if (strcasecmp(dirty_user, focus_user) == 0) {
                    // Same user re-painted: reload + reset focus timer.
                    if (storage_read_drawing(dirty_user, s_render_tmp)) {
                        render_drawing_to_fb(s_render_tmp);
                    }
                    STATE_LOCK();
                    g_state.focus_end_ms = millis() + FOCUS_DURATION_MS;
                    g_state.drawing_dirty = false;
                    focus_end_ms = g_state.focus_end_ms;
                    STATE_UNLOCK();
                } else {
                    // Different user during focus: overwrite the single queue slot.
                    STATE_LOCK();
                    strncpy(g_state.queued_username, dirty_user, MAX_UNAME_LEN - 1);
                    g_state.queued_username[MAX_UNAME_LEN - 1] = '\0';
                    g_state.drawing_dirty = false;
                    STATE_UNLOCK();
                    strncpy(queued_user, dirty_user, MAX_UNAME_LEN - 1);
                    queued_user[MAX_UNAME_LEN - 1] = '\0';
                }
            }

            if ((uint32_t)millis() >= focus_end_ms) {
                if (queued_user[0] != '\0') {
                    // Immediately focus the queued user before resuming scroll.
                    if (storage_read_drawing(queued_user, s_render_tmp)) {
                        render_drawing_to_fb(s_render_tmp);
                    } else {
                        fill_fb_black();
                    }
                    STATE_LOCK();
                    strncpy(g_state.focus_username, queued_user, MAX_UNAME_LEN - 1);
                    g_state.focus_username[MAX_UNAME_LEN - 1] = '\0';
                    g_state.queued_username[0] = '\0';
                    g_state.focus_end_ms = millis() + FOCUS_DURATION_MS;
                    STATE_UNLOCK();
                } else {
                    STATE_LOCK();
                    g_state.mode = MODE_SCROLL;
                    STATE_UNLOCK();
                }
            }
        } else {
            // MODE_SCROLL
            if (dirty) {
                // Save scroll position implicitly (cur_idx/col_off untouched).
                if (storage_read_drawing(dirty_user, s_render_tmp)) {
                    render_drawing_to_fb(s_render_tmp);
                } else {
                    fill_fb_black();
                }
                STATE_LOCK();
                g_state.mode = MODE_FOCUS;
                strncpy(g_state.focus_username, dirty_user, MAX_UNAME_LEN - 1);
                g_state.focus_username[MAX_UNAME_LEN - 1] = '\0';
                g_state.focus_end_ms = millis() + FOCUS_DURATION_MS;
                g_state.drawing_dirty = false;
                STATE_UNLOCK();
            } else if (user_count == 0) {
                fill_fb_black();
            } else if (user_count == 1) {
                // Single user: static display (scope.md Scroll Mode special case).
                char name[MAX_UNAME_LEN];
                STATE_LOCK();
                strncpy(name, g_state.userlist[0], MAX_UNAME_LEN - 1);
                name[MAX_UNAME_LEN - 1] = '\0';
                STATE_UNLOCK();
                if (strcasecmp(name, s_cached_single) != 0) {
                    strncpy(s_cached_single, name, MAX_UNAME_LEN - 1);
                    s_cached_single[MAX_UNAME_LEN - 1] = '\0';
                    if (storage_read_drawing(name, s_render_tmp)) {
                        render_drawing_to_fb(s_render_tmp);
                    } else {
                        fill_fb_black();
                    }
                }
            } else {
                // Scroll composition.
                ++s_frame_cnt;
                if (s_frame_cnt % frames_per_col == 0) {
                    ++col_off;
                    if (col_off >= WIDTH) {
                        col_off = 0;
                        cur_idx = (cur_idx + 1) % user_count;
                    }
                }

                char nA[MAX_UNAME_LEN], nB[MAX_UNAME_LEN];
                STATE_LOCK();
                strncpy(nA, g_state.userlist[cur_idx], MAX_UNAME_LEN - 1);
                nA[MAX_UNAME_LEN - 1] = '\0';
                strncpy(nB, g_state.userlist[(cur_idx + 1) % user_count], MAX_UNAME_LEN - 1);
                nB[MAX_UNAME_LEN - 1] = '\0';
                STATE_UNLOCK();

                if (s_cache_a != cur_idx) {
                    s_cache_a = cur_idx;
                    if (!storage_read_drawing(nA, s_buf_a)) {
                        memset(s_buf_a, 0, sizeof(s_buf_a));
                    }
                }
                int nb = (cur_idx + 1) % user_count;
                if (s_cache_b != nb) {
                    s_cache_b = nb;
                    if (!storage_read_drawing(nB, s_buf_b)) {
                        memset(s_buf_b, 0, sizeof(s_buf_b));
                    }
                }

                compose_scroll_fb(s_buf_a, s_buf_b, col_off);

                STATE_LOCK();
                g_state.scroll_user_index = cur_idx;
                g_state.scroll_col_offset = col_off;
                STATE_UNLOCK();
            }
        }

        // ---- push framebuf to LEDs ----
        fb_to_leds();
        FastLED.show();

        vTaskDelay(pdMS_TO_TICKS(1000 / kFps));  // ~33 ms target
    }
}