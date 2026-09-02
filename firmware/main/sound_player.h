#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void sound_player_init(void);
void sound_play_bark(void);
void sound_play_boot(void);
void sound_play_chirp(void);
void sound_play_yes(void);
void sound_play_named(const char *name);
void sound_play_8bit(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
