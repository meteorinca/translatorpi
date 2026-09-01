#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t i2s_audio_init(void);
size_t i2s_audio_read(int16_t *buf, size_t samples, TickType_t timeout);
size_t i2s_audio_write(const int16_t *buf, size_t samples, TickType_t timeout);
void i2s_audio_clear_buffers(void);
void i2s_audio_set_amp(bool enable);
