#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    DISPLAY_STATE_BOOT,
    DISPLAY_STATE_CONNECTING_WIFI,
    DISPLAY_STATE_CONNECTING_WS,
    DISPLAY_STATE_READY,
    DISPLAY_STATE_RECORDING,
    DISPLAY_STATE_TRANSCRIBING,
    DISPLAY_STATE_TRANSLATING,
    DISPLAY_STATE_SPEAKING,
    DISPLAY_STATE_ERROR,
} display_state_t;

typedef enum {
    EYE_COLOR_NORMAL = 0,
    EYE_COLOR_RED = 1,
} eye_color_t;

esp_err_t oled_display_init(void);
void oled_display_update(display_state_t state, const char *src_lang, const char *dst_lang, const char *msg);
void oled_display_show_text(const char *stt_text, const char *trans_text);
void oled_display_show_languages(const char *src_lang, const char *dst_lang, const char *dst_name);
void oled_display_set_eye_color(eye_color_t color);
void oled_display_clear(void);
