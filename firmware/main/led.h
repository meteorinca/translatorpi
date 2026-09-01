#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef enum {
    LED_MODE_OFF,
    LED_MODE_CONNECTING,
    LED_MODE_IDLE,
    LED_MODE_RECORDING,
    LED_MODE_WAITING,
    LED_MODE_SPEAKING,
    LED_MODE_SWAP_FLASH,
    LED_MODE_ERROR,
} led_mode_t;

esp_err_t led_init(void);
void led_set_mode(led_mode_t mode);
