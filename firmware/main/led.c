#include "led.h"
#include "config.h"
#include "esp_log.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "NEOPIXEL";
static volatile led_mode_t s_current_mode = LED_MODE_CONNECTING;
static led_strip_handle_t s_strip = NULL;

#define NUM_LEDS 4

static void set_all_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    for (int i = 0; i < NUM_LEDS; i++) {
        led_strip_set_pixel(s_strip, i, r, g, b);
    }
    led_strip_refresh(s_strip);
}

static void led_task(void *arg)
{
    uint32_t tick = 0;

    while (1) {
        tick++;

        switch (s_current_mode) {
        case LED_MODE_OFF:
            if (s_strip) led_strip_clear(s_strip);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case LED_MODE_CONNECTING: {
            // Pulsing cyan/blue to indicate Wi-Fi / Host connecting
            float phase = (tick % 40) / 40.0f; // 0.0 to 1.0
            float brightness = 0.5f * (1.0f + sinf(phase * 2.0f * 3.14159f));
            uint8_t b = (uint8_t)(brightness * 50.0f);
            uint8_t g = (uint8_t)(brightness * 20.0f);
            set_all_rgb(0, g, b);
            vTaskDelay(pdMS_TO_TICKS(30));
            break;
        }

        case LED_MODE_IDLE: {
            // Heartbeat / gentle breathing green: Confirms connected to host!
            float phase = (tick % 60) / 60.0f; // ~1.8s cycle
            float breath = (expf(sinf(phase * 2.0f * 3.14159f)) - 0.36787944f) * 0.425459f;
            uint8_t g = 4 + (uint8_t)(breath * 28.0f); // 4 to 32 (gentle, doesn't blind)
            uint8_t b = 1 + (uint8_t)(breath * 6.0f);
            set_all_rgb(0, g, b);
            vTaskDelay(pdMS_TO_TICKS(30));
            break;
        }

        case LED_MODE_RECORDING: {
            // Solid vibrant red/purple: User is speaking, audio streaming!
            for (int i = 0; i < NUM_LEDS; i++) {
                led_strip_set_pixel(s_strip, i, 50, 0, 10);
            }
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(60));
            break;
        }

        case LED_MODE_WAITING: {
            // Thinking / transcribing: Chaser amber/yellow dot around the 4 LEDs
            int active_led = (tick / 2) % NUM_LEDS;
            for (int i = 0; i < NUM_LEDS; i++) {
                if (i == active_led) {
                    led_strip_set_pixel(s_strip, i, 45, 30, 0); // Bright amber
                } else {
                    led_strip_set_pixel(s_strip, i, 6, 3, 0);  // Dim amber glow
                }
            }
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }

        case LED_MODE_SPEAKING: {
            // Speaking: Cyan / teal ripple wave across LEDs
            for (int i = 0; i < NUM_LEDS; i++) {
                float wave = sinf((tick * 0.3f) + (i * 1.5f));
                uint8_t bright = (uint8_t)(fmaxf(0.0f, wave) * 45.0f);
                led_strip_set_pixel(s_strip, i, 0, bright, bright + 5);
            }
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(40));
            break;
        }

        case LED_MODE_SWAP_FLASH:
            // Double purple flash then return to IDLE
            set_all_rgb(40, 0, 50); vTaskDelay(pdMS_TO_TICKS(120));
            set_all_rgb(0, 0, 0);   vTaskDelay(pdMS_TO_TICKS(80));
            set_all_rgb(40, 0, 50); vTaskDelay(pdMS_TO_TICKS(120));
            set_all_rgb(0, 0, 0);   vTaskDelay(pdMS_TO_TICKS(50));
            s_current_mode = LED_MODE_IDLE;
            break;

        case LED_MODE_ERROR: {
            // Red rapid flash
            bool on = (tick % 2) == 0;
            set_all_rgb(on ? 60 : 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(80));
            break;
        }
        }
    }
}

esp_err_t led_init(void)
{
    ESP_LOGI(TAG, "Initializing NeoPixel strip (%d LEDs on GPIO %d)", NUM_LEDS, STATUS_LED_PIN);

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = STATUS_LED_PIN,
        .max_leds = NUM_LEDS,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000, // 10 MHz
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create led_strip RMT device: %s", esp_err_to_name(ret));
        return ret;
    }

    led_strip_clear(s_strip);
    xTaskCreate(led_task, "led_task", 3072, NULL, 2, NULL);
    return ESP_OK;
}

void led_set_mode(led_mode_t mode)
{
    s_current_mode = mode;
}
