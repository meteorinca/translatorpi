#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    BTN_EVENT_NONE,
    BTN_EVENT_PTT_PRESS,      // Button held down (PTT start)
    BTN_EVENT_PTT_RELEASE,    // Button released after holding (PTT stop)
    BTN_EVENT_SINGLE_CLICK,   // Single tap
    BTN_EVENT_DOUBLE_CLICK,   // Double tap (cycle target language)
    BTN_EVENT_TRIPLE_CLICK,   // Triple tap (cycle target language)
    BTN_EVENT_SWAP_CLICK,     // Secondary button click (cycle target language)
} button_event_t;

typedef void (*button_callback_t)(button_event_t event);

esp_err_t button_init(button_callback_t cb);
