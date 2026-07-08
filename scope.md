# TwitchDraws v2 — ESP32 Technical Scope

## Overview

This document describes the full architecture and implementation plan for TwitchDraws v2: a self-contained firmware program running entirely on an ESP32. It replaces the Python + WLED server pipeline with a single embedded program that handles Twitch IRC chat, per-user pixel canvas storage, and LED matrix display rendering directly from the ESP32.

---

## Hardware Assumptions

- **MCU:** ESP32 (e.g. ESP32-WROOM-32 or equivalent)
- **Display:** 64×16 addressable LED matrix (WS2812B or compatible), driven via ESP32 GPIO using the FastLED or NeoPixel library
- **Storage:** Internal flash via SPIFFS or LittleFS (no SD card assumed; revisit if storage needs grow)
- **Connectivity:** WiFi (station mode) for Twitch IRC over TCP
- **Power:** External 5V supply; ESP32 powered separately or via 3.3V rail

---

## Display Geometry

- Width: 64 pixels, Height: 16 pixels
- Total pixels: 1024
- Coordinate system matches v1: `(x, y)` where `x` is 1–64 (left to right), `y` is 1–16 (bottom to top)
- Pixel index formula: `index = (HEIGHT - y) * WIDTH + (x - 1)` — same as v1 Python script
- LED strip is driven as a flat 1D array of 1024 RGB values

---

## Pixel Color Representation

- Each pixel stored as a packed `uint32_t` RGB value (0xRRGGBB) in RAM
- On-disk (SPIFFS): stored as compact binary — 3 bytes per pixel (R, G, B), 3072 bytes per drawing file
- Named colors table (same set as v1) stored as a static lookup array in firmware
- Hex string parsing: accept `#RRGGBB` or `RRGGBB` (case-insensitive)

---

## File Storage Layout (SPIFFS / LittleFS)

```
/draws/
    <username>.bin       — current drawing for each user (3072 bytes: 1024 × RGB)
/meta/
    userlist.txt         — newline-separated list of known usernames, ordered most-recent-first
    <username>.date      — ISO date string (YYYY-MM-DD) of last interaction per user
/config.txt              — WiFi SSID, password, Twitch channel, Twitch nick, Twitch OAuth token
```

- `userlist.txt` is the scroll order. Top of file = most recent. Updated on every pixel command.
- Each user's `.bin` file is overwritten in-place on every pixel command from that user.
- Date files store the last-interaction date for display purposes.
- Total worst-case storage: 50 users × 3072 bytes ≈ 150 KB for drawings; well within ESP32 SPIFFS partition (typically 1–3 MB).

---

## System Modes

The firmware operates in two modes, switching between them as events occur:

### 1. Scroll Mode (default)

- Continuously scrolls all known user drawings across the display, right to left
- Scroll order: most-recent user interaction first (from top of `userlist.txt`)
- Each drawing is 64 pixels wide; it scrolls fully across the 64-wide display before the next one begins
- Scroll speed: configurable (default: ~20 pixels/second; tune for aesthetics)
- When the last user's drawing finishes scrolling, loop back to the beginning
- If no users exist yet, display a blank screen or a static default image

### 2. Focus Mode (triggered by pixel command)

- Interrupts scroll mode
- Loads the affected user's current drawing into the display buffer and renders it full-screen (no scrolling)
- Holds for **6 seconds**
- After 6 seconds, resumes scroll mode from where it left off (same position in the user list, same scroll offset)
- If another pixel command arrives during the 6-second focus window from the same user, reset the 6-second timer and re-render their updated drawing
- If a pixel command arrives during focus from a **different** user, queue it: after the current focus ends, immediately begin a new 6-second focus for the queued user before resuming scroll

---

## Twitch IRC Connection

- Connect to `irc.chat.twitch.tv:6667` via raw TCP socket (WiFiClient)
- Authentication sequence:
  ```
  PASS oauth:<token>\r\n
  NICK <nick>\r\n
  JOIN #<channel>\r\n
  ```
- Maintain a persistent receive buffer; split on `\r\n` to get complete lines
- Respond to `PING :tmi.twitch.tv` with `PONG :tmi.twitch.tv\r\n` immediately — failure to do so results in disconnection
- On socket close, empty response, or timeout: close socket and reconnect with exponential backoff (start 1s, cap at 60s)
- **No message drops:** the TCP receive loop runs in its own FreeRTOS task at high frequency; incoming data is buffered in a FreeRTOS queue before processing, so the parser never blocks the network read
- PRIVMSG format: `:username!username@username.tmi.twitch.tv PRIVMSG #channel :message text`
  - Extract username from prefix (split on `!`)
  - Extract message text from third `:` component

---

## FreeRTOS Task Architecture

The firmware uses FreeRTOS (native on ESP32 Arduino framework) with the following tasks:

| Task Name          | Core | Priority | Stack    | Description |
|--------------------|------|----------|----------|-------------|
| `task_twitch_rx`   | 0    | High (5) | 8 KB     | Raw TCP read loop; feeds lines into `twitch_line_queue` |
| `task_cmd_parser`  | 1    | Medium (4)| 6 KB    | Dequeues lines, parses `!pixel` commands, updates user files |
| `task_display`     | 1    | Medium (3)| 6 KB    | Manages scroll/focus mode; drives LED matrix via FastLED |
| `task_wifi_watch`  | 0    | Low (2)  | 4 KB     | Monitors WiFi connection; reconnects if dropped |

- `twitch_line_queue`: FreeRTOS queue of fixed-size line buffers (e.g. 20 items × 512 bytes each). If the queue fills (parser stalled), oldest item is dropped and an error counter incremented.
- Tasks communicate only via queues and a shared `AppState` struct guarded by a FreeRTOS mutex.
- No `delay()` in any task — use `vTaskDelay()` with appropriate tick counts.

---

## AppState Struct (shared, mutex-guarded)

```c
typedef struct {
    // Mode
    enum { MODE_SCROLL, MODE_FOCUS } mode;

    // Scroll state
    int scroll_user_index;       // index into userlist
    int scroll_pixel_offset;     // current horizontal scroll offset (0 = drawing fully right, 64 = fully scrolled off left)

    // Focus state
    char focus_username[64];     // username currently in focus
    uint32_t focus_end_ms;       // millis() timestamp when focus ends
    char queued_username[64];    // next username to focus after current focus (empty if none)

    // User list
    char userlist[MAX_USERS][64];
    int  user_count;

    // Display frame buffer
    uint32_t framebuf[1024];     // current frame, written by display task, read by FastLED

    // Dirty flag: set by cmd_parser when a drawing file is updated
    bool drawing_dirty;
    char dirty_username[64];
} AppState;
```

`MAX_USERS` can start at 100; adjust based on `userlist.txt` growth.

---

## Pixel Command Parsing

Command format (identical to v1): `!pixel <x>,<y> <color> [<x>,<y> ...]`

- Trigger: message starts with `!pixel` (case-insensitive)
- Color: named color string OR `#RRGGBB` / `RRGGBB` hex
- Coordinates: one or more `x,y` pairs; each validated against bounds (1–64, 1–16)
- Out-of-bounds coordinates are silently ignored
- Multiple coordinate pairs in one command are all applied atomically to the user's `.bin` file
- After applying changes:
  1. Write updated pixel array to `/draws/<username>.bin`
  2. Update `/meta/<username>.date` with today's date
  3. Update `userlist.txt`: move `<username>` to top (remove existing entry if present, prepend)
  4. Signal `AppState` with `drawing_dirty = true` and `dirty_username = <username>`
  5. `task_display` picks up the dirty flag and transitions to Focus Mode for that user

---

## Display Task Logic

```
loop:
  if mode == FOCUS:
    if millis() >= focus_end_ms:
      if queued_username is set:
        load queued user drawing into framebuf
        focus_username = queued_username
        clear queued_username
        focus_end_ms = millis() + 6000
      else:
        mode = SCROLL
        // restore scroll position
    else:
      // check dirty flag for current focus user
      if drawing_dirty && dirty_username == focus_username:
        reload framebuf from file
        clear dirty flag
        focus_end_ms = millis() + 6000  // reset timer
      // render framebuf to LEDs
      FastLED.show()

  if mode == SCROLL:
    if drawing_dirty:
      // update scroll order from userlist
      reload userlist into AppState
      clear dirty flag
      enter FOCUS mode for dirty_username
    else:
      // advance scroll_pixel_offset by scroll speed
      // compose framebuf: current user's drawing scrolled left,
      //   next user's drawing entering from right
      // when scroll_pixel_offset reaches 64:
      //   advance scroll_user_index
      //   reset scroll_pixel_offset = 0
      FastLED.show()
      vTaskDelay(scroll_frame_delay)
```

### Scroll Composition

At any given frame, the display shows parts of up to two consecutive drawings:
- Left portion: `current_drawing` pixels from column `scroll_pixel_offset` onward
- Right portion: `next_drawing` pixels from column 0 to `scroll_pixel_offset - 1`

This creates a smooth left-scroll transition between drawings.

---

## Configuration

Stored in `/config.txt` as simple `key=value` lines:

```
ssid=YourWiFiName
password=YourWiFiPassword
channel=your_twitch_channel
nick=your_bot_nick
oauth=oauth:your_token
scroll_speed=20
```

- Parsed once at boot
- No runtime config changes (no web server in v2 — pure embedded)
- `scroll_speed` = pixels per second the display scrolls

---

## Startup Sequence

1. Mount SPIFFS/LittleFS; create directories if not present
2. Parse `/config.txt`
3. Load `userlist.txt` into `AppState.userlist`
4. Load most-recent user's drawing into `framebuf` (or blank if no users)
5. Initialize FastLED with pin, LED count, and LED type
6. Connect WiFi (block with timeout; restart ESP on repeated failure)
7. Start FreeRTOS tasks: `task_twitch_rx`, `task_cmd_parser`, `task_display`, `task_wifi_watch`
8. Enter scroll mode

---

## Error Handling

- **WiFi drop:** `task_wifi_watch` calls `WiFi.reconnect()`; `task_twitch_rx` detects disconnected socket and waits for WiFi before reconnecting to Twitch
- **Twitch disconnect:** exponential backoff reconnect in `task_twitch_rx`
- **SPIFFS write failure:** log to Serial; continue with in-RAM state (don't crash)
- **Queue overflow:** drop oldest unprocessed line; increment a `dropped_lines` counter (readable via Serial)
- **Watchdog:** ESP32 hardware WDT — all tasks must not block indefinitely; use `vTaskDelay` to yield

---

## Future Extension Points

- **Custom commands:** Additional chat commands (beyond `!pixel`) should be dispatched from `task_cmd_parser` via a command handler table. Each handler receives the username and message, performs its action, and optionally triggers Focus Mode or a custom display sequence before returning to scroll.
- **Hardware upgrade:** If flash storage becomes insufficient (many users), swap SPIFFS for an SD card; only the file I/O layer needs changing.
- **OTA updates:** ArduinoOTA can be added as an additional FreeRTOS task with low priority.
- **Brightness/effects:** FastLED brightness and per-pixel effects can be applied in the display task without touching storage.

---

## Known Constraints / Open Questions

- [ ] **OAuth token on device:** The Twitch OAuth token must be stored in plaintext in `/config.txt` on SPIFFS. This is acceptable for a single-owner device but should be noted.
- [ ] **Max users:** SPIFFS partition size limits total user count. With default 1MB SPIFFS: ~330 users max (drawings only). In practice, `userlist.txt` scroll time grows linearly — a cap of 50–100 active users is recommended for a reasonable scroll experience.
- [ ] **Scroll speed feel:** The `scroll_speed` parameter will need tuning once running on hardware.
- [ ] **Display wiring:** GPIO pin for LED data line TBD — document in code header.
- [ ] **Twitch IRC rate limits:** Anonymous read (justinfan) has no rate limits for reading. If the bot nick sends any messages in the future, rate limiting must be handled.
