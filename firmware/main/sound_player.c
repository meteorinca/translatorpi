#include "sound_player.h"
#include "i2s_audio.h"
#include "dogbark_audio_8bit.h"
#include "yes_audio_8bit.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "SOUND";

void sound_player_init(void)
{
    ESP_LOGI(TAG, "Sound player initialized");
}

void sound_play_8bit(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return;

    int16_t buf[256];
    size_t processed = 0;
    while (processed < len) {
        size_t chunk = (len - processed > 256) ? 256 : (len - processed);
        for (size_t i = 0; i < chunk; i++) {
            int8_t s8 = (int8_t)data[processed + i];
            buf[i] = ((int16_t)s8) << 8;
        }
        i2s_audio_write(buf, chunk, pdMS_TO_TICKS(100));
        processed += chunk;
    }
    i2s_audio_clear_buffers();
}

static void generate_tone(int freq_hz, int duration_ms, float volume)
{
    if (freq_hz <= 0 || duration_ms <= 0) return;
    int samples = (16000 * duration_ms) / 1000;
    int16_t buf[256];
    int generated = 0;
    float phase = 0.0f;
    float phase_inc = (2.0f * 3.14159265f * freq_hz) / 16000.0f;

    while (generated < samples) {
        int chunk = (samples - generated > 256) ? 256 : (samples - generated);
        for (int i = 0; i < chunk; i++) {
            // Apply slight envelope (attack / decay)
            float env = 1.0f;
            if (generated + i < 160) {
                env = (float)(generated + i) / 160.0f;
            } else if (samples - (generated + i) < 160) {
                env = (float)(samples - (generated + i)) / 160.0f;
            }
            float val = sinf(phase) * volume * env * 24000.0f;
            buf[i] = (int16_t)val;
            phase += phase_inc;
            if (phase > 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }
        i2s_audio_write(buf, chunk, pdMS_TO_TICKS(100));
        generated += chunk;
    }
}

void sound_play_bark(void)
{
    ESP_LOGI(TAG, "Playing dog bark...");
    sound_play_8bit(dogbark_audio_8bit, dogbark_audio_8bit_len);
}

void sound_play_boot(void)
{
    ESP_LOGI(TAG, "Playing boot melody...");
    // Upbeat boot chime: C5 (523Hz) -> E5 (659Hz) -> G5 (784Hz) -> C6 (1046Hz)
    generate_tone(523, 70, 0.45f);
    vTaskDelay(pdMS_TO_TICKS(15));
    generate_tone(659, 70, 0.45f);
    vTaskDelay(pdMS_TO_TICKS(15));
    generate_tone(784, 70, 0.50f);
    vTaskDelay(pdMS_TO_TICKS(15));
    generate_tone(1046, 140, 0.55f);
    vTaskDelay(pdMS_TO_TICKS(100));
    sound_play_bark();
}

void sound_play_chirp(void)
{
    generate_tone(880, 50, 0.4f);
    vTaskDelay(pdMS_TO_TICKS(20));
    generate_tone(1320, 70, 0.4f);
    i2s_audio_clear_buffers();
}

void sound_play_yes(void)
{
    ESP_LOGI(TAG, "Playing YES sound (%d bytes)...", (int)yes_audio_8bit_len);
    sound_play_8bit(yes_audio_8bit, yes_audio_8bit_len);
}

void sound_play_named(const char *name)
{
    if (!name) return;
    if (strcmp(name, "bark") == 0) {
        sound_play_bark();
    } else if (strcmp(name, "yes") == 0) {
        sound_play_yes();
    } else if (strcmp(name, "boot") == 0) {
        sound_play_boot();
    } else if (strcmp(name, "chirp") == 0 || strcmp(name, "ding") == 0) {
        sound_play_chirp();
    } else if (strcmp(name, "hi") == 0 || strcmp(name, "hello") == 0) {
        generate_tone(587, 80, 0.4f); // D5
        generate_tone(880, 120, 0.5f); // A5
        vTaskDelay(pdMS_TO_TICKS(50));
        sound_play_bark();
    } else {
        sound_play_bark();
    }
}
