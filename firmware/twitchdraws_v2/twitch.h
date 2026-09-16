#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "state.h"

// Scope.md "FreeRTOS Task Architecture":
//   task_twitch_rx              - core 0, priority 5, 8 KB stack
//   task_cmd_parser             - core 1, priority 4, 6 KB stack
//   task_display                - core 1, priority 3, 6 KB stack
//   task_wifi_watch             - core 0, priority 2, 4 KB stack
#define TWITCH_STACK_RX      8192
#define TWITCH_PRIO_RX       5
#define PARSER_STACK         6144
#define PARSER_PRIO          4
#define DISPLAY_STACK        6144
#define DISPLAY_PRIO         3
#define WIFI_WATCH_STACK     4096
#define WIFI_WATCH_PRIO      2
#define RX_CORE              0
#define APP_CORE             1

#define TWITCH_HOST          "irc.chat.twitch.tv"
#define TWITCH_PORT          6667

// Complete IRC lines flow rx -> parser through this queue.
// 20 items x 512 bytes (scope.md). On overflow the oldest item is dropped and
// stat_dropped_lines is incremented.
#define TWITCH_QUEUE_LEN     20
#define TWITCH_QUEUE_ITEM    512
#define TWITCH_LINE_BUF      2048  // heap-allocated receive buffer

extern QueueHandle_t g_twitch_line_queue;

void twitch_create_queue();

// FreeRTOS tasks.
void task_twitch_rx(void* pv);
void task_wifi_watch(void* pv);

// Sends a raw line (with trailing CRLF appended). Thread-safe for use by any
// task (parser responds to PING). No-op when not connected.
bool twitch_send_line(const char* line);

// IRC prefix parsing: given a PRIVMSG line, extracts the author username and
// message text. Returns false if the line is not a PRIVMSG we understand.
bool twitch_parse_privmsg(const char* line,
                          char username[MAX_UNAME_LEN],
                          char message[TWITCH_QUEUE_ITEM]);