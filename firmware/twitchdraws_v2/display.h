#pragma once

// Scope.md "FreeRTOS Task Architecture": task_display runs on APP_CORE at
// priority DISPLAY_PRIO (see twitch.h). Owns the scroll/focus state machine and
// drives the LED matrix via FastLED.
void task_display(void* pv);

// FastLED init: addLeds + brightness + blank-on-boot (scope.md Startup seq 6-8).
void display_init();