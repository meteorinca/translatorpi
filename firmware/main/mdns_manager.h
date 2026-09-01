#pragma once

#include "esp_err.h"

esp_err_t mdns_manager_init(void);
const char* mdns_manager_get_hostname(void);
