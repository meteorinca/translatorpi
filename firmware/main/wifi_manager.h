#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*wifi_event_callback_t)(bool connected);

esp_err_t wifi_manager_init(wifi_event_callback_t cb);
bool wifi_manager_is_connected(void);
