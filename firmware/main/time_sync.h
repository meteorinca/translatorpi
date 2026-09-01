#pragma once

#include <time.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t time_sync_init(void);
void time_sync_set_epoch(time_t epoch_sec);
time_t time_sync_get_epoch(void);
bool time_sync_is_synced(void);
void time_sync_get_time_str(char *buf, size_t max_len);
