#pragma once

#include "twitch.h"

// Scope.md "FreeRTOS Task Architecture": task_cmd_parser runs on APP_CORE at
// priority PARSER_PRIO (see twitch.h). It owns NOTHING shared beyond AppState:
// - dequeues IRC lines from g_twitch_line_queue
// - answers PING with PONG (via twitch_send_line)
// - parses !pixel commands and updates disk + AppState
void task_cmd_parser(void* pv);