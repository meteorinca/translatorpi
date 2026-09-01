#include "button.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BUTTON";
static button_callback_t s_callback = NULL;

#define POLL_INTERVAL_MS     20
#define LONG_PRESS_MS        450
#define MULTI_CLICK_GAP_MS   320

static void button_task(void *arg)
{
    // Configure main PTT / multifunction button
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BUTTON_PTT),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };

#if HAS_SECONDARY_BUTTON
    io_conf.pin_bit_mask |= (1ULL << PIN_BUTTON_SWAP);
#endif

    gpio_config(&io_conf);

    bool btn_state_prev = false;
    bool swap_state_prev = false;
    bool is_holding_ptt = false;
    uint32_t press_duration_ms = 0;
    uint32_t release_duration_ms = 0;
    int click_count = 0;

    ESP_LOGI(TAG, "Button monitoring task started (PTT GPIO%d)", PIN_BUTTON_PTT);

    while (1) {
        // Active-low: 0 means pressed
        bool btn_pressed = (gpio_get_level(PIN_BUTTON_PTT) == 0);

#if HAS_SECONDARY_BUTTON
        bool swap_pressed = (gpio_get_level(PIN_BUTTON_SWAP) == 0);
        if (swap_pressed && !swap_state_prev) {
            ESP_LOGI(TAG, "Secondary Swap button pressed");
            if (s_callback) s_callback(BTN_EVENT_SWAP_CLICK);
        }
        swap_state_prev = swap_pressed;
#endif

        if (btn_pressed) {
            press_duration_ms += POLL_INTERVAL_MS;
            release_duration_ms = 0;

            // Detect hold (PTT mode)
            if (press_duration_ms >= LONG_PRESS_MS && !is_holding_ptt) {
                is_holding_ptt = true;
                click_count = 0; // Cancel multi-click sequence when held
                ESP_LOGI(TAG, "Button Long-Press / PTT Start");
                if (s_callback) s_callback(BTN_EVENT_PTT_PRESS);
            }
        } else {
            // Button is currently released
            if (btn_state_prev) {
                // Just released
                if (is_holding_ptt) {
                    is_holding_ptt = false;
                    ESP_LOGI(TAG, "Button PTT Release");
                    if (s_callback) s_callback(BTN_EVENT_PTT_RELEASE);
                } else if (press_duration_ms >= 30 && press_duration_ms < LONG_PRESS_MS) {
                    // Valid quick tap detected
                    click_count++;
                }
                press_duration_ms = 0;
            }

            if (click_count > 0) {
                release_duration_ms += POLL_INTERVAL_MS;
                // Wait for multi-click window to expire
                if (release_duration_ms >= MULTI_CLICK_GAP_MS) {
                    if (click_count == 1) {
                        ESP_LOGI(TAG, "Single click detected");
                        if (s_callback) s_callback(BTN_EVENT_SINGLE_CLICK);
                    } else if (click_count == 2) {
                        ESP_LOGI(TAG, "Double click detected (Dogbot talk toggle)");
                        if (s_callback) s_callback(BTN_EVENT_DOUBLE_CLICK);
                    } else if (click_count >= 3) {
                        ESP_LOGI(TAG, "Triple click detected (Language swap)");
                        if (s_callback) s_callback(BTN_EVENT_TRIPLE_CLICK);
                    }
                    click_count = 0;
                    release_duration_ms = 0;
                }
            }
        }

        btn_state_prev = btn_pressed;
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t button_init(button_callback_t cb)
{
    s_callback = cb;
    xTaskCreate(button_task, "btn_task", 3072, NULL, 5, NULL);
    return ESP_OK;
}
