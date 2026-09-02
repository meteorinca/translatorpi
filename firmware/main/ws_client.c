#include "ws_client.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "WS_CLIENT";

static esp_websocket_client_handle_t s_client = NULL;
static bool s_is_connected = false;
static SemaphoreHandle_t s_send_mutex = NULL;
static ws_conn_callback_t s_conn_cb = NULL;
static ws_text_callback_t s_text_cb = NULL;
static ws_binary_callback_t s_bin_cb = NULL;

static void parse_and_dispatch_json(const char *json_data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json_data, len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse incoming JSON frame");
        return;
    }

    ws_msg_t msg = {0};
    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type_item)) msg.type = type_item->valuestring;

    cJSON *state_item = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(state_item)) msg.state = state_item->valuestring;

    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text_item)) msg.text = text_item->valuestring;

    cJSON *trans_item = cJSON_GetObjectItem(root, "translation");
    if (cJSON_IsString(trans_item)) msg.translation = trans_item->valuestring;

    cJSON *disp_item = cJSON_GetObjectItem(root, "display_text");
    if (cJSON_IsString(disp_item)) msg.display_text = disp_item->valuestring;

    cJSON *msg_item = cJSON_GetObjectItem(root, "message");
    if (cJSON_IsString(msg_item)) msg.message = msg_item->valuestring;

    cJSON *rate_item = cJSON_GetObjectItem(root, "rate");
    if (cJSON_IsNumber(rate_item)) msg.rate = rate_item->valueint;

    cJSON *len_item = cJSON_GetObjectItem(root, "len");
    if (cJSON_IsNumber(len_item)) msg.len = len_item->valueint;

    if (s_text_cb) {
        s_text_cb(&msg);
    }

    cJSON_Delete(root);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to host bridge (%s)", CONFIG_WS_SERVER_URI);
        s_is_connected = true;
        if (s_conn_cb) s_conn_cb(true);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from host bridge, reconnecting...");
        s_is_connected = false;
        if (s_conn_cb) s_conn_cb(false);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            parse_and_dispatch_json((const char *)data->data_ptr, data->data_len);
        } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
            if (s_bin_cb) {
                s_bin_cb((const uint8_t *)data->data_ptr, data->data_len);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error occurred");
        break;
    }
}

esp_err_t ws_client_init(ws_conn_callback_t conn_cb, ws_text_callback_t text_cb, ws_binary_callback_t bin_cb)
{
    s_conn_cb = conn_cb;
    s_text_cb = text_cb;
    s_bin_cb = bin_cb;

    if (!s_send_mutex) {
        s_send_mutex = xSemaphoreCreateMutex();
    }

    esp_websocket_client_config_t websocket_cfg = {
        .uri = CONFIG_WS_SERVER_URI,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 10000,
        .buffer_size = 4096,
    };

    ESP_LOGI(TAG, "Initializing WebSocket client to %s", CONFIG_WS_SERVER_URI);
    s_client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)s_client);
    return esp_websocket_client_start(s_client);
}

bool ws_client_is_connected(void)
{
    return s_is_connected && s_client && esp_websocket_client_is_connected(s_client);
}

esp_err_t ws_client_send_text(const char *json_str)
{
    if (!ws_client_is_connected()) {
        ESP_LOGW(TAG, "ws_client_send_text: client not connected (payload: %s)", json_str ? json_str : "NULL");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_send_mutex) xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    int len = strlen(json_str);
    int ret = esp_websocket_client_send_text(s_client, json_str, len, pdMS_TO_TICKS(2000));
    if (s_send_mutex) xSemaphoreGive(s_send_mutex);
    if (ret < 0) {
        ESP_LOGE(TAG, "esp_websocket_client_send_text failed (%d) for %s", ret, json_str);
    }
    return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t ws_client_send_binary(const uint8_t *data, size_t len)
{
    if (!ws_client_is_connected()) return ESP_ERR_INVALID_STATE;
    if (s_send_mutex) xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    int ret = esp_websocket_client_send_bin(s_client, (const char *)data, len, pdMS_TO_TICKS(1000));
    if (s_send_mutex) xSemaphoreGive(s_send_mutex);
    return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t ws_client_send_hello(const char *device, const char *src, const char *dst)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"type\":\"hello\",\"device\":\"%s\",\"src\":\"%s\",\"dst\":\"%s\"}",
             device ? device : DEVICE_NAME_STR, src, dst);
    return ws_client_send_text(buf);
}

esp_err_t ws_client_send_record_start(void)
{
    return ws_client_send_text("{\"type\":\"record_start\"}");
}

esp_err_t ws_client_send_record_stop(void)
{
    return ws_client_send_text("{\"type\":\"record_stop\"}");
}

esp_err_t ws_client_send_swap(void)
{
    return ws_client_send_text("{\"type\":\"swap\"}");
}

esp_err_t ws_client_send_set_lang(const char *src, const char *dst)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"type\":\"set_lang\",\"src\":\"%s\",\"dst\":\"%s\"}",
             src ? src : "en", dst ? dst : "es");
    return ws_client_send_text(buf);
}

esp_err_t ws_client_send_ping(void)
{
    return ws_client_send_text("{\"type\":\"ping\"}");
}
