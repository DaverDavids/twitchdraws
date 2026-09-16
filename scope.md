# TwitchDraws v2 — ESP32 Technical Scope

## Overview

This document describes the full architecture and implementation plan for TwitchDraws v2: a self-contained firmware program running entirely on an ESP32. It replaces the Python + WLED server pipeline with a single embedded program that handles Twitch IRC chat, per-user pixel canvas storage, and LED matrix display rendering directly from the ESP32.

---

## Hardware (Confirmed)

- **MCU:** ESP32-WROOM-32
- **Display:** 4× 8×32 WS2812B addressable LED panels, arranged to form a single 64×16 logical display
- **Panel arrangement:** 4 panels tiled horizontally — each panel is 32 columns × 8 rows; two rows of two panels stacked to make 64 wide × 16 tall
- **LED driver library:** FastLED (WS2812B, `GRB` color order)
- **Data GPIO pin:** TBD — define as a named constant `LED_DATA_PIN` at the top of the firmware file. Confirm with hardware before flashing.
- **Total LEDs:** 64 × 16 = 1024
- **Storage:** Internal flash via LittleFS (preferred over SPIFFS — better wear leveling, actively maintained in ESP32 Arduino core)
- **Connectivity:** WiFi station mode (802.11 b/g/n), 2.4 GHz
- **Power:** External 5V supply powering LED panels; ESP32 powered via USB or 3.3V regulator from the same supply rail

### Panel Layout & Pixel Mapping

The 4 panels are physically wired in a specific serpentine or chain order. The LED index-to-pixel mapping must account for how the panels are chained together. The exact wiring order (which panel is first in the chain, direction of data flow through each panel) must be verified against the physical hardware and encoded in the `xy_to_led_index()` function.

**Logical coordinate system (matches v1 Python script):**
- `x`: 1–64, left to right
- `y`: 1–16, bottom to top
- `index = (HEIGHT - y) * WIDTH + (x - 1)`

**Important:** If the physical LED wiring order does not match this logical layout (e.g., panels are chained right-to-left, or rows are serpentine), the `xy_to_led_index()` function must remap logical `(x, y)` to the physical LED index. This remapping is the primary hardware-specific section of the firmware and should be tested first using a solid-color test pattern before any other logic is written.

---

## Pixel Color Representation

- Each pixel stored in RAM as a packed `uint32_t` RGB value (0x00RRGGBB)
- FastLED uses `CRGB` structs; convert from packed uint32 before calling `FastLED.show()`
- On-disk (LittleFS): compact binary — 3 bytes per pixel (R, G, B), 3072 bytes per user drawing file
- Named colors table (same set as v1 Python script) stored as a static `const` lookup table in firmware
- Hex string parsing: accept `#RRGGBB` or `RRGGBB` (case-insensitive)
- Color names must support at minimum the full set from v1: black, white, red, green, blue, yellow, cyan, magenta, purple, orange, pink, gray, off, grey, lime, teal, navy, maroon, olive, aqua, fuchsia, indigo, violet, gold, silver, brown, beige, coral, salmon, chocolate, crimson, turquoise, skyblue, lavender, mint, peach, hotpink, plum, orchid, khaki

---

## File Storage Layout (LittleFS)

```
/draws/
    <username>.bin       — current drawing for each user (3072 bytes: 1024 × 3 RGB bytes)
/meta/
    userlist.txt         — newline-separated list of known usernames, ordered most-recent-first
    <username>.date      — ISO date string (YYYY-MM-DD) of last interaction per user
/config.txt              — all runtime configuration (see Configuration section)
```

- `userlist.txt` is the canonical scroll order. Top of file = most recent. Rewritten on every pixel command that introduces a new user or updates an existing one's recency.
- Each user's `.bin` file is overwritten in-place on every pixel command from that user.
- Date files store the last-interaction date (used for display/metadata; not currently rendered on LEDs but retained for future use).
- **Storage estimate:** 100 users × 3072 bytes = ~300 KB for drawings. LittleFS on ESP32-WROOM-32 with a 1 MB partition supports ~330 users; 2 MB partition ~660 users. Well within practical limits.

---

## System Modes

The firmware operates in two modes, switching between them based on events:

### 1. Scroll Mode (default)

- Continuously scrolls all known user drawings across the display from right to left
- Scroll order: most-recent user interaction first (top of `userlist.txt`)
- Each drawing is 64 pixels wide — it scrolls fully across and off the left edge before the next drawing enters from the right
- **Single user special case:** If only one user exists, their drawing is displayed statically (no scrolling). Scroll resumes as soon as a second user's drawing exists.
- **No users:** Display a blank (all-black) screen
- When the last user's drawing finishes scrolling, loop back to the first user
- Scroll speed is configurable (see Configuration)

### 2. Focus Mode (triggered by pixel command)

- Interrupts scroll mode immediately
- Loads the affected user's full updated drawing into `framebuf` and renders it static (no scrolling) for **6 seconds**
- After 6 seconds, resumes scroll mode, restoring the scroll position to where it was before the interruption (same `scroll_user_index` and `scroll_pixel_offset`)
- **Same-user update during focus:** Reset the 6-second timer and re-render their updated drawing from disk
- **Different-user command during focus:** Store that username in `queued_username`. When the current focus ends, immediately start a new 6-second focus for the queued user before resuming scroll. Only one queued user is held at a time; if a third user sends a command while two are already active/queued, the queue is overwritten with the latest.
- **Custom command actions** (future): Any non-pixel command handler that triggers a display action uses the same Focus Mode mechanism — perform the action, hold or animate for a defined duration, then return to scroll.

---

## Twitch IRC Connection

### Authentication Modes

Two modes controlled by config:

**Anonymous (default):**
```
PASS SCHMOOPIIE\r\n
NICK justinfan<random 5-digit number>\r\n
JOIN #<channel>\r\n
```
- No OAuth token needed
- Read-only; cannot send chat messages
- No rate limits on reading
- `twitch_mode=anonymous` in config (or omitting `oauth` key)

**Authenticated (optional):**
```
PASS oauth:<token>\r\n
NICK <nick>\r\n
JOIN #<channel>\r\n
```
- Required if the firmware ever needs to send messages to chat
- Token stored in plaintext in `/config.txt` on LittleFS — acceptable for a single-owner device
- `twitch_mode=oauth` in config, with `oauth` and `nick` keys set

### Connection Protocol

- Connect to `irc.chat.twitch.tv:6667` via raw TCP (WiFiClient)
- Maintain a persistent receive buffer (heap-allocated, 2 KB); split on `\r\n` for complete lines
- Respond to `PING :tmi.twitch.tv` with `PONG :tmi.twitch.tv\r\n` immediately — Twitch disconnects if PONG is not sent within ~5 minutes
- On socket close, empty read, or timeout: close socket, wait for WiFi, reconnect with exponential backoff (1s start, 2× per retry, 60s cap)
- **Zero-drop design:** `task_twitch_rx` does nothing but read bytes and push complete lines into `twitch_line_queue`. It never parses or processes. This ensures the TCP receive window is always being drained.

**PRIVMSG line format:**
```
:username!username@username.tmi.twitch.tv PRIVMSG #channel :message text
```
- Username: split prefix on `!`, take left side, strip leading `:`
- Message: everything after the third `:` (third field)

---

## FreeRTOS Task Architecture

The firmware uses FreeRTOS (native on ESP32 Arduino framework) with the following tasks:

| Task Name          | Core | Priority | Stack  | Description |
|--------------------|------|----------|--------|-------------|
| `task_twitch_rx`   | 0    | 5 (High) | 8 KB   | Raw TCP read loop; pushes complete IRC lines into `twitch_line_queue` |
| `task_cmd_parser`  | 1    | 4        | 6 KB   | Dequeues IRC lines, parses commands, updates user drawing files, signals display |
| `task_display`     | 1    | 3        | 6 KB   | Manages scroll/focus state machine; drives LED matrix via FastLED |
| `task_wifi_watch`  | 0    | 2 (Low)  | 4 KB   | Monitors WiFi; calls reconnect on drop; signals `task_twitch_rx` to reconnect |

**Notes:**
- Core 0 handles networking tasks; Core 1 handles processing and display — avoids WiFi stack contention
- `twitch_line_queue`: FreeRTOS queue, 20 items × 512 bytes each (10 KB total). If queue fills (parser stalled), oldest item is dropped and `stat_dropped_lines` counter incremented. Overflow logged to Serial.
- Inter-task communication: only via queues and `AppState` struct guarded by a single `SemaphoreHandle_t` mutex
- No `delay()` anywhere — use `vTaskDelay(pdMS_TO_TICKS(n))` exclusively
- All tasks run indefinitely in `while(1)` loops and must yield regularly to feed the hardware WDT

---

## AppState Struct (shared, mutex-guarded)

```c
#define MAX_USERS     200
#define MAX_UNAME_LEN  64
#define LED_COUNT    1024

typedef enum {
    MODE_SCROLL,
    MODE_FOCUS
} DisplayMode;

typedef struct {
    DisplayMode mode;

    // --- Scroll state ---
    int  scroll_user_index;    // current user index being scrolled
    int  scroll_col_offset;    // columns scrolled so far for current user (0–63)

    // --- Focus state ---
    char focus_username[MAX_UNAME_LEN];
    uint32_t focus_end_ms;     // millis() when focus expires
    char queued_username[MAX_UNAME_LEN];  // empty string = no queued user

    // --- User registry ---
    char  userlist[MAX_USERS][MAX_UNAME_LEN];  // scroll order, [0] = most recent
    int   user_count;

    // --- Display frame buffer ---
    uint32_t framebuf[LED_COUNT];  // packed 0x00RRGGBB per pixel

    // --- Dirty flag (cmd_parser → display task) ---
    bool drawing_dirty;
    char dirty_username[MAX_UNAME_LEN];

    // --- Diagnostics ---
    uint32_t stat_dropped_lines;   // IRC lines dropped due to queue overflow
    uint32_t stat_twitch_reconnects;
} AppState;

extern AppState g_state;
extern SemaphoreHandle_t g_state_mutex;
```

Helper macros:
```c
#define STATE_LOCK()    xSemaphoreTake(g_state_mutex, portMAX_DELAY)
#define STATE_UNLOCK()  xSemaphoreGive(g_state_mutex)
```
Keep mutex hold time short — copy needed fields out, release, then process.

---

## Pixel Command Parsing

Command format (identical to v1 Python script):
```
!pixel <x>,<y> <color> [<x>,<y> ...]
!pixel <color> <x>,<y> [<x>,<y> ...]   (color may appear before coordinates)
```

- **Trigger:** Message text starts with `!pixel` (case-insensitive)
- **Color:** Named color string (from lookup table) OR `#RRGGBB` / `RRGGBB` hex (case-insensitive)
- **Coordinates:** One or more `x,y` pairs anywhere in the message after `!pixel`; parsed with regex or simple scanf-style scanning
- **Bounds:** x in [1, 64], y in [1, 16]; out-of-bounds pairs silently ignored
- **Batch apply:** All valid pairs applied together to the in-RAM drawing before writing to disk

**Processing steps (in `task_cmd_parser`):**
1. Parse color and all valid `(x, y)` pairs
2. Load `/draws/<username>.bin` into a local 3072-byte buffer (or use a zeroed buffer if file doesn't exist — new user)
3. Apply all pixel changes to the local buffer
4. Write buffer back to `/draws/<username>.bin`
5. Write today's date to `/meta/<username>.date`
6. Update `userlist.txt`: remove existing entry for username (if any), prepend to top, rewrite file
7. Acquire `g_state_mutex`; set `drawing_dirty = true`, copy username to `dirty_username`, update `userlist[]` array and `user_count`; release mutex
8. `task_display` detects `drawing_dirty` on next loop iteration and transitions to Focus Mode

---

## Display Task Logic

### Frame Timing

- Target: 30 fps refresh (FastLED.show() every ~33 ms)
- Scroll speed is specified in **columns per second** (configurable). Frame delay determines how many frames elapse per column advance:
  - `frames_per_col = fps / scroll_speed_cols_per_sec`
  - Example: 30 fps, scroll_speed = 6 cols/sec → advance 1 column every 5 frames
- A frame counter increments each loop; column advances when `frame_counter % frames_per_col == 0`

### State Machine

```
Each display task loop iteration (~33 ms):

  acquire mutex → copy mode, dirty flag, relevant fields → release mutex

  if mode == MODE_FOCUS:
    if dirty && dirty_username == focus_username:
      load /draws/<focus_username>.bin into local_drawing[]
      render local_drawing[] into framebuf (full screen, no scroll)
      reset focus_end_ms = millis() + 6000
      clear dirty flag in g_state

    if millis() >= focus_end_ms:
      if queued_username is not empty:
        load /draws/<queued_username>.bin into local_drawing[]
        render into framebuf
        acquire mutex → focus_username = queued_username, clear queued_username,
                         focus_end_ms = millis() + 6000 → release mutex
      else:
        acquire mutex → mode = MODE_SCROLL → release mutex
        // scroll resumes from saved scroll_user_index / scroll_col_offset

  if mode == MODE_SCROLL:
    if dirty:
      acquire mutex → save scroll position, mode = MODE_FOCUS,
                       focus_username = dirty_username,
                       focus_end_ms = millis() + 6000,
                       drawing_dirty = false → release mutex
      load /draws/<dirty_username>.bin → render into framebuf
    else if user_count == 1:
      load /draws/userlist[0].bin → render full screen static
    else if user_count == 0:
      fill framebuf with black
    else:
      advance frame_counter
      if frame_counter % frames_per_col == 0:
        scroll_col_offset++
        if scroll_col_offset >= WIDTH:
          scroll_col_offset = 0
          scroll_user_index = (scroll_user_index + 1) % user_count
      compose framebuf from two drawings (see Scroll Composition)

  push framebuf to FastLED LEDs
  FastLED.show()
  vTaskDelay(pdMS_TO_TICKS(33))
```

### Scroll Composition

At each frame during scrolling, the display shows a blend of two consecutive user drawings:

```
current_drawing  = load_drawing(userlist[scroll_user_index])
next_drawing     = load_drawing(userlist[(scroll_user_index + 1) % user_count])

for each row r in [0, HEIGHT):
  for each col c in [0, WIDTH):
    logical_col_in_current = c + scroll_col_offset
    if logical_col_in_current < WIDTH:
      framebuf[r][c] = current_drawing[r][logical_col_in_current]
    else:
      framebuf[r][c] = next_drawing[r][logical_col_in_current - WIDTH]
```

To avoid re-reading two files from LittleFS every frame (slow), the display task maintains two heap-allocated 3072-byte draw buffers (`draw_buf_a`, `draw_buf_b`) and only reloads them when `scroll_user_index` changes.

---

## Configuration

Stored in `/config.txt` (LittleFS) as `key=value` lines. Parsed once at boot. Lines beginning with `#` are comments.

```ini
# WiFi
ssid=YourWiFiSSID
password=YourWiFiPassword

# Twitch
channel=your_channel_name
twitch_mode=anonymous        # anonymous | oauth
nick=justinfan12345          # used only in anonymous mode (any justinfan#### works)
# oauth=oauth:your_token     # uncomment and set for oauth mode
# oauth_nick=your_bot_nick   # required if twitch_mode=oauth

# Display
scroll_speed=6               # columns per second (integer; recommended range: 2–30)
led_brightness=128           # FastLED global brightness, 0–255
```

**`scroll_speed` guidance:**
- 6 cols/sec at 30 fps = one column advance every 5 frames — smooth, readable
- 1 col/sec = very slow crawl; 30 col/sec = fast sweep
- Stored as integer; validated at boot (clamp to [1, 60])

**`led_brightness`:**
- Applied via `FastLED.setBrightness()` at startup
- Lower values reduce power draw significantly on fully-lit panels

---

## Startup Sequence

1. `Serial.begin(115200)` for debug output
2. Mount LittleFS; if mount fails, halt and print error to Serial
3. Create `/draws/` and `/meta/` directories if not present
4. Parse `/config.txt`; halt with Serial error if file missing or required keys absent
5. Load `userlist.txt` into `g_state.userlist[]` and `g_state.user_count`
6. Initialize FastLED: `FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(leds, LED_COUNT)`
7. Set brightness from config: `FastLED.setBrightness(cfg.led_brightness)`
8. Fill display black, call `FastLED.show()` (blank on boot)
9. Connect WiFi (blocking, 30s timeout; restart ESP32 via `ESP.restart()` on failure)
10. Create `g_state_mutex = xSemaphoreCreateMutex()`
11. Start tasks: `task_wifi_watch`, `task_twitch_rx`, `task_cmd_parser`, `task_display`, `task_ota` (OTA task added at user direction; see OTA Updates section)
12. Main loop: feed WDT, print stats to Serial every 60s, sleep

---

## Error Handling

| Condition | Behavior |
|---|---|
| WiFi drop | `task_wifi_watch` calls `WiFi.reconnect()`; `task_twitch_rx` detects loss, closes socket, waits for WiFi up, then reconnects to Twitch |
| Twitch socket close / empty read | Close socket, increment `stat_twitch_reconnects`, exponential backoff reconnect |
| PING not answered in time | Socket closes; handled by reconnect logic above |
| LittleFS write failure | Log to Serial; keep in-RAM state intact; do not crash |
| IRC line queue overflow | Drop oldest item; increment `stat_dropped_lines`; log to Serial |
| Config file missing / malformed | Halt in `setup()` with Serial error — do not start tasks |
| Hardware WDT | All tasks must call `vTaskDelay` at least once per 8 seconds; display task at 33 ms loop naturally satisfies this |

---

## Physical LED Index Mapping (Hardware-Specific)

This section must be confirmed against the actual wiring before the display task is written.

The 4× (8 row × 32 col) panels are chained. The chain order and serpentine direction within each panel determines how logical `(x, y)` maps to the physical WS2812B LED index.

**Implement as:**
```c
int xy_to_led_index(int x, int y) {
    // x: 1–64, y: 1–16
    // TODO: implement based on confirmed physical wiring
    // Placeholder (matches v1 linear mapping — verify against hardware):
    return (HEIGHT - y) * WIDTH + (x - 1);
}
```

**Testing protocol before full firmware:**
Write a minimal sketch that lights a single known `(x, y)` coordinate and visually verify the correct physical LED illuminates. Sweep a few coordinates across all 4 panels to confirm the mapping is correct before integrating into the full firmware.

---

## Future Extension Points

- **Custom chat commands:** Add handlers to a dispatch table in `task_cmd_parser`. Each handler receives `(username, message)`, executes its action (may write to `framebuf` directly or trigger a mode transition), then returns. The display task's Focus Mode mechanism is available to any handler.
- **SD card storage:** If LittleFS partition fills, replace file I/O layer with SD/SPI — only file open/read/write calls change.
- **Brightness control:** A chat command could call `FastLED.setBrightness()` live.
- **Per-panel effects:** Future panel-level animations (wipe, flash) can be implemented in the display task as transient modes alongside SCROLL and FOCUS.

---

## Known Constraints & Open Items

- [ ] **`LED_DATA_PIN`:** GPIO pin for WS2812B data line — confirm with hardware wiring before first flash
- [ ] **Panel chain order & serpentine:** Confirm physical LED index mapping with a test sketch (see Physical LED Index Mapping section)
- [ ] **LittleFS partition size:** Set in Arduino IDE partition scheme or `partitions.csv`. OTA is now in the core build (see OTA Updates section), so the scheme must allow a 2nd app slot for ArduinoOTA. Recommended: **"Huge APP (3MB No OTA / 1MB SPIFFS)" is NOT sufficient** — pick a scheme with an OTA slot: Arduino IDE **"Minimal SPIFFS (1.9MB APP / 190KB SPIFFS / 1.9MB OTA)"** or PlatformIO `-D CONFIG_PARTITION_TABLE_CUSTOM` with `partitions.csv` (app0, app1, spiffs). Document chosen scheme in code header.
- [ ] **OAuth token storage:** Plaintext in LittleFS — acceptable for single-owner device; noted here for awareness
- [ ] **Scroll speed tuning:** Default of 6 cols/sec is a starting point; tune after running on hardware
- [ ] **Max user scroll time:** With 100 users at 6 cols/sec, one full scroll cycle = 100 × (64 cols / 6) ≈ 17 minutes. Consider a configurable max-users-in-rotation cap if this grows too long.
- [ ] **WLED config export:** Owner has existing WLED config available. Review it to confirm LED count, color order, and any gamma correction settings currently in use, and replicate equivalent settings in FastLED (`FastLED.setCorrection()`, `FastLED.setTemperature()`).
