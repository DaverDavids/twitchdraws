#include "twitch.h"

#include <Arduino.h>
#include <WiFi.h>

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "state.h"

QueueHandle_t g_twitch_line_queue = NULL;

static WiFiClient s_client;
static SemaphoreHandle_t s_send_mutex = NULL;

// Zero-drop design: task_twitch_rx NEVER parses. It only:
//   - owns the socket lifecycle (connect / read / reconnect-backoff)
//   - drains bytes into a line buffer, splitting on \r\n
//   - pushes complete lines into g_twitch_line_queue
void task_twitch_rx(void* pv) {
    (void)pv;
    char* rx_buf = (char*)malloc(TWITCH_LINE_BUF);
    if (rx_buf == NULL) {
        Serial.println("[twitch] FATAL: could not allocate rx buffer");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    uint32_t backoff_ms = 1000;  // 1s start, x2 per retry, 60s cap

    for (;;) {
        // Wait until WiFi is up.
        while (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (!s_client.connect(TWITCH_HOST, TWITCH_PORT)) {
            Serial.println("[twitch] connect failed");
            s_client.stop();
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms * 2 > 60000) ? 60000 : backoff_ms * 2;
            continue;
        }

        // Send login (scope.md Authentication Modes).
        if (s_send_mutex) {
            xSemaphoreTake(s_send_mutex, portMAX_DELAY);
        }
        if (g_cfg.oauth_mode) {
            s_client.printf("PASS %s\r\n", g_cfg.oauth_token);
            s_client.printf("NICK %s\r\n", g_cfg.oauth_nick);
        } else {
            s_client.print("PASS SCHMOOPIIE\r\n");
            s_client.printf("NICK %s\r\n", g_cfg.nick);
        }
        s_client.printf("JOIN %s\r\n", g_cfg.channel);
        if (s_send_mutex) {
            xSemaphoreGive(s_send_mutex);
        }

        Serial.printf("[twitch] connected to %s\n", g_cfg.channel);
        backoff_ms = 1000;  // reset after a successful connect

        // --- Inner read loop ---
        size_t used = 0;
        while (s_client.connected() || s_client.available() > 0) {
            // If the WiFi link dropped, the socket is dead even if TCP has not
            // signaled it yet. Bail and reconnect (wifi_watch will re-associate).
            if (WiFi.status() != WL_CONNECTED) {
                break;
            }
            // Drain whatever is in the TCP window.
            while (s_client.available() > 0) {
                int n = s_client.available();
                if (n > (int)(TWITCH_LINE_BUF - used - 1)) {
                    n = TWITCH_LINE_BUF - used - 1;
                }
                if (n <= 0) {
                    // Receive buffer filled with no line terminator (pathological
                    // oversized line). Drop the partial data and keep the socket.
                    Serial.println("[twitch] line buffer overflow - dropping partial line");
                    used = 0;
                    break;
                }
                int rd = s_client.read((uint8_t*)&rx_buf[used], n);
                if (rd <= 0) {
                    break;
                }
                used += (size_t)rd;

                // Split complete \r\n lines out of the buffer and push them to the
                // queue (drop-oldest on overflow).
                size_t line_start = 0;
                size_t i = 0;
                while (i < used) {
                    if (rx_buf[i] == '\n') {
                        size_t len = i - line_start;
                        if (len > 0 && rx_buf[i - 1] == '\r') {
                            --len;
                        }
                        if (len > 0) {
                            char item[TWITCH_QUEUE_ITEM];
                            if (len > sizeof(item) - 1) {
                                len = sizeof(item) - 1;
                            }
                            memcpy(item, &rx_buf[line_start], len);
                            item[len] = '\0';

                            if (xQueueSend(g_twitch_line_queue, item, 0) != pdTRUE) {
                                Serial.println("[twitch] queue full - dropping oldest line");
                                char dropped[TWITCH_QUEUE_ITEM];
                                xQueueReceive(g_twitch_line_queue, dropped, 0);
                                xQueueSend(g_twitch_line_queue, item, 0);
                                STATE_LOCK();
                                g_state.stat_dropped_lines++;
                                STATE_UNLOCK();
                            }
                        }
                        line_start = i + 1;
                    }
                    ++i;
                }
                // Compact the remaining partial tail.
                used -= line_start;
                if (used > 0) {
                    memmove(rx_buf, &rx_buf[line_start], used);
                }
            }
            // Yield so more bytes can arrive and the WDT is fed.
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // Socket closed / empty / error -> reconnect with exponential backoff.
        s_client.stop();
        STATE_LOCK();
        g_state.stat_twitch_reconnects++;
        STATE_UNLOCK();
        Serial.printf("[twitch] disconnected; retry in %lus\n", backoff_ms / 1000);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = (backoff_ms * 2 > 60000) ? 60000 : backoff_ms * 2;
    }
}

void task_wifi_watch(void* pv) {
    (void)pv;
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[wifi] link down - reconnecting");
            WiFi.reconnect();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void twitch_create_queue() {
    if (g_twitch_line_queue == NULL) {
        g_twitch_line_queue = xQueueCreate(TWITCH_QUEUE_LEN, TWITCH_QUEUE_ITEM);
        s_send_mutex = xSemaphoreCreateMutex();
    }
}

bool twitch_send_line(const char* line) {
    if (line == NULL || s_send_mutex == NULL) return false;
    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    bool ok = s_client.connected();
    if (ok) {
        s_client.print(line);
        s_client.print("\r\n");
    }
    xSemaphoreGive(s_send_mutex);
    return ok;
}

bool twitch_parse_privmsg(const char* line,
                          char username[MAX_UNAME_LEN],
                          char message[TWITCH_QUEUE_ITEM]) {
    // Expected (scope.md):
    //   :username!username@username.tmi.twitch.tv PRIVMSG #channel :message
    if (line == NULL || strncmp(line, ":", 1) != 0) {
        return false;
    }
    const char* priv = strstr(line, " PRIVMSG ");
    if (priv == NULL) {
        return false;
    }

    // Username: between the leading ':' and '!'.
    const char* u = line + 1;
    const char* bang = strchr(u, '!');
    size_t ulen = (bang != NULL) ? (size_t)(bang - u) : (size_t)(priv - u);
    if (ulen == 0 || ulen >= MAX_UNAME_LEN) {
        return false;
    }
    memcpy(username, u, ulen);
    username[ulen] = '\0';

    // Message: everything after " :" following PRIVMSG.
    const char* m = priv + strlen(" PRIVMSG ");
    const char* sep = strstr(m, " :");
    if (sep != NULL) {
        m = sep + 2;
    }
    strncpy(message, m, TWITCH_QUEUE_ITEM - 1);
    message[TWITCH_QUEUE_ITEM - 1] = '\0';
    return true;
}