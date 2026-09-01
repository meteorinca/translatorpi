#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char *type;
    char *state;
    char *text;
    char *translation;
    char *message;
    int rate;
    int len;
} ws_msg_t;

typedef void (*ws_text_callback_t)(const ws_msg_t *msg);
typedef void (*ws_binary_callback_t)(const uint8_t *data, size_t len);
typedef void (*ws_conn_callback_t)(bool connected);

esp_err_t ws_client_init(ws_conn_callback_t conn_cb, ws_text_callback_t text_cb, ws_binary_callback_t bin_cb);
bool ws_client_is_connected(void);
esp_err_t ws_client_send_text(const char *json_str);
esp_err_t ws_client_send_binary(const uint8_t *data, size_t len);
esp_err_t ws_client_send_hello(const char *device, const char *src, const char *dst);
esp_err_t ws_client_send_record_start(void);
esp_err_t ws_client_send_record_stop(void);
esp_err_t ws_client_send_swap(void);
esp_err_t ws_client_send_ping(void);
