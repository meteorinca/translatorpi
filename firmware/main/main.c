#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "oled_display.h"
#include "led.h"
#include "i2s_audio.h"
#include "state_machine.h"
#include "button.h"
#include "wifi_manager.h"
#include "ws_client.h"
#include "mdns_manager.h"
#include "time_sync.h"
#include "web_server.h"

static const char *TAG = "MAIN";

static void on_wifi_status(bool connected)
{
    ESP_LOGI(TAG, "Wi-Fi status change: %s", connected ? "CONNECTED" : "DISCONNECTED");
    state_machine_on_wifi_event(connected);

    if (connected) {
        // Start mDNS service discovery
        mdns_manager_init();

        // Start SNTP background time sync
        time_sync_init();

        // Start embedded HTTP Web Dashboard & control API
        web_server_start();
    }
}

static void on_ws_conn(bool connected)
{
    ESP_LOGI(TAG, "WebSocket status change: %s", connected ? "CONNECTED" : "DISCONNECTED");
    state_machine_on_ws_event(connected);
}

static void on_button_event(button_event_t event)
{
    state_machine_on_button_event(event);
}

static void on_ws_text(const ws_msg_t *msg)
{
    state_machine_on_ws_text(msg);
}

static void on_ws_binary(const uint8_t *data, size_t len)
{
    state_machine_on_ws_binary(data, len);
}

void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  ESP32-C3 Portable Translator Starting          ");
    ESP_LOGI(TAG, "  Profile       : %s                             ", DEVICE_NAME_STR);
    ESP_LOGI(TAG, "  Device Number : #%d                            ", DEVICE_NUMBER);
    ESP_LOGI(TAG, "  mDNS Hostname : %s-%d.local                    ", DEVICE_NAME_PREFIX, DEVICE_NUMBER);
    ESP_LOGI(TAG, "==================================================");

    // 1. Initialize NVS (required for Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize Hardware Peripherals
    ESP_ERROR_CHECK(oled_display_init());
    ESP_ERROR_CHECK(led_init());
    ESP_ERROR_CHECK(i2s_audio_init());

    // 3. Initialize State Machine
    ESP_ERROR_CHECK(state_machine_init());

    // 4. Initialize Button Poller
    ESP_ERROR_CHECK(button_init(on_button_event));

    // 5. Initialize Wi-Fi (starts Web Server & mDNS upon connection)
    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_status));

    // 6. Initialize WebSocket Client to Host PC
    ESP_ERROR_CHECK(ws_client_init(on_ws_conn, on_ws_text, on_ws_binary));

    ESP_LOGI(TAG, "Firmware initialization complete. Running main event loops.");
}
