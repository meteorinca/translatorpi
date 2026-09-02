#include "state_machine.h"
#include "config.h"
#include "i2s_audio.h"
#include "oled_display.h"
#include "led.h"
#include "ws_client.h"
#include "dog_actions.h"
#include "sound_player.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"

static const char *TAG = "STATE_MACHINE";

typedef enum {
    EV_WIFI_STATE_CHANGED,
    EV_WS_STATE_CHANGED,
    EV_BUTTON_TRIGGERED,
    EV_SERVER_STATUS,
    EV_SERVER_STT,
    EV_SERVER_TRANSLATION,
    EV_SERVER_TTS_START,
    EV_SERVER_DONE,
    EV_SERVER_ERROR,
    EV_CMD_SWAP,
    EV_CMD_SET_LANG,
    EV_CMD_RECORD_START,
    EV_CMD_RECORD_STOP,
} sm_event_type_t;

typedef struct {
    sm_event_type_t type;
    bool bool_val;
    button_event_t btn_val;
    char *str_val1;
    char *str_val2;
    int int_val;
} sm_event_t;

static QueueHandle_t s_event_queue = NULL;
static RingbufHandle_t s_playback_ringbuf = NULL;

static sm_state_t s_state = SM_STATE_INIT;
static char s_src_lang[8] = CONFIG_DEFAULT_SRC_LANG;
static char s_dst_lang[8] = CONFIG_DEFAULT_DST_LANG;
static bool s_is_toggle_recording = false;
static char s_last_translation[256] = {0};

// Background Audio RX Task (Mic -> WebSocket streaming)
static void audio_rx_task(void *arg)
{
    int16_t *rx_buf = malloc(AUDIO_CHUNK_BYTES);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio RX buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Audio RX task started (16 kHz mono)");

    while (1) {
        if (s_state == SM_STATE_RECORDING) {
            size_t read_samples = i2s_audio_read(rx_buf, AUDIO_CHUNK_SAMPLES, pdMS_TO_TICKS(150));
            if (read_samples > 0 && s_state == SM_STATE_RECORDING) {
                // Send binary frame
                ws_client_send_binary((const uint8_t *)rx_buf, read_samples * sizeof(int16_t));
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// Background Audio TX Task (RingBuffer -> I2S Speaker playback)
static void audio_tx_task(void *arg)
{
    ESP_LOGI(TAG, "Audio TX playback task started");

    while (1) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(s_playback_ringbuf, &item_size, pdMS_TO_TICKS(50));
        if (item) {
            if (s_state != SM_STATE_SPEAKING && s_state != SM_STATE_RECORDING) {
                s_state = SM_STATE_SPEAKING;
                led_set_mode(LED_MODE_SPEAKING);
                oled_display_update(DISPLAY_STATE_SPEAKING, s_src_lang, s_dst_lang,
                                    s_last_translation[0] ? s_last_translation : "Speaking...");
            }

            size_t written = 0;
            while (written < item_size && s_state != SM_STATE_RECORDING) {
                size_t chunk = (item_size - written) / sizeof(int16_t);
                if (chunk > 512) chunk = 512;
                size_t s = i2s_audio_write((const int16_t *)(item + written), chunk, pdMS_TO_TICKS(100));
                if (s == 0) {
                    vTaskDelay(pdMS_TO_TICKS(2));
                } else {
                    written += s * sizeof(int16_t);
                }
            }
            vRingbufferReturnItem(s_playback_ringbuf, (void *)item);
        } else {
            if (s_state == SM_STATE_SPEAKING) {
                // Playback finished
                i2s_audio_clear_buffers();
                s_state = SM_STATE_IDLE;
                led_set_mode(LED_MODE_IDLE);
                s_last_translation[0] = '\0';
                oled_display_update(DISPLAY_STATE_READY, s_src_lang, s_dst_lang, "Ready");
            }
        }
    }
}

typedef struct {
    const char *code;
    const char *name;
} target_lang_t;

static const target_lang_t s_target_languages[] = {
    { "es", "Spanish" },
    { "zh", "Chinese" },
    { "ja", "Japanese" },
    { "ar", "Arabic" },
    { "ko", "Korean" },
};
#define NUM_TARGET_LANGUAGES (sizeof(s_target_languages) / sizeof(s_target_languages[0]))

static void cycle_target_language(void)
{
    // Always keep source language EN as requested
    strncpy(s_src_lang, "en", sizeof(s_src_lang));

    int cur_idx = -1;
    for (int i = 0; i < (int)NUM_TARGET_LANGUAGES; i++) {
        if (strcasecmp(s_dst_lang, s_target_languages[i].code) == 0) {
            cur_idx = i;
            break;
        }
    }

    int next_idx = (cur_idx + 1) % NUM_TARGET_LANGUAGES;
    strncpy(s_dst_lang, s_target_languages[next_idx].code, sizeof(s_dst_lang));

    ESP_LOGI(TAG, "Target language switched: %s -> %s (%s)",
             s_src_lang, s_dst_lang, s_target_languages[next_idx].name);

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"type\":\"set_lang\",\"src\":\"%s\",\"dst\":\"%s\"}", s_src_lang, s_dst_lang);
    ws_client_send_text(buf);

    led_set_mode(LED_MODE_SWAP_FLASH);
    sound_play_chirp();

    // Show what language to what on OLED: e.g. "EN -> ES\nSpanish"
    oled_display_show_languages(s_src_lang, s_dst_lang, s_target_languages[next_idx].name);
}

static void set_languages_internal(const char *src, const char *dst)
{
    // Always keep source language EN as requested
    strncpy(s_src_lang, "en", sizeof(s_src_lang));
    if (dst && strlen(dst) > 0) strncpy(s_dst_lang, dst, sizeof(s_dst_lang));
    ESP_LOGI(TAG, "Languages updated: %s -> %s", s_src_lang, s_dst_lang);

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"type\":\"set_lang\",\"src\":\"%s\",\"dst\":\"%s\"}", s_src_lang, s_dst_lang);
    ws_client_send_text(buf);

    led_set_mode(LED_MODE_SWAP_FLASH);

    const char *target_name = s_dst_lang;
    for (int i = 0; i < (int)NUM_TARGET_LANGUAGES; i++) {
        if (strcasecmp(s_dst_lang, s_target_languages[i].code) == 0) {
            target_name = s_target_languages[i].name;
            break;
        }
    }
    oled_display_show_languages(s_src_lang, s_dst_lang, target_name);
}

static void start_recording(void)
{
    if (s_state == SM_STATE_IDLE || s_state == SM_STATE_SPEAKING) {
        s_state = SM_STATE_RECORDING;
        led_set_mode(LED_MODE_RECORDING);
        oled_display_update(DISPLAY_STATE_RECORDING, s_src_lang, s_dst_lang, "Speak now...");
        ws_client_send_record_start();
        ESP_LOGI(TAG, "Recording started...");
    }
}

static void stop_recording(void)
{
    if (s_state == SM_STATE_RECORDING) {
        s_state = SM_STATE_WAITING;
        led_set_mode(LED_MODE_WAITING);
        oled_display_update(DISPLAY_STATE_TRANSCRIBING, s_src_lang, s_dst_lang, "Processing...");
        
        // Wait 30ms to allow in-flight audio chunk to finish network transmission before sending stop control frame
        vTaskDelay(pdMS_TO_TICKS(30));

        esp_err_t err = ws_client_send_record_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Record stop send failed (%s), retrying...", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(50));
            ws_client_send_record_stop();
        }
        ESP_LOGI(TAG, "Recording stopped, waiting for translation...");
    }
}

// Central State Task
static void state_task(void *arg)
{
    sm_event_t ev;
    oled_display_update(DISPLAY_STATE_BOOT, s_src_lang, s_dst_lang, "Initializing...");

    while (1) {
        if (xQueueReceive(s_event_queue, &ev, portMAX_DELAY)) {
            switch (ev.type) {
            case EV_WIFI_STATE_CHANGED:
                if (ev.bool_val) {
                    // Connected to WiFi but waiting for bridge: show animated red eyes!
                    oled_display_set_eye_color(EYE_COLOR_RED);
                    oled_display_clear(); // Reveal eyes immediately
                    led_set_mode(LED_MODE_CONNECTING);
                } else {
                    oled_display_set_eye_color(EYE_COLOR_NORMAL);
                    oled_display_update(DISPLAY_STATE_CONNECTING_WIFI, s_src_lang, s_dst_lang, "Connecting WiFi...");
                    led_set_mode(LED_MODE_CONNECTING);
                }
                break;

            case EV_WS_STATE_CHANGED:
                if (ev.bool_val) {
                    // Connected to bridge: play sound YES and eyes go back to normal!
                    s_state = SM_STATE_IDLE;
                    led_set_mode(LED_MODE_IDLE);
                    oled_display_set_eye_color(EYE_COLOR_NORMAL);
                    oled_display_update(DISPLAY_STATE_READY, s_src_lang, s_dst_lang, "Ready");
                    sound_play_yes();
                    ws_client_send_hello(DEVICE_NAME_STR, s_src_lang, s_dst_lang);
                } else {
                    s_state = SM_STATE_WS_CONNECTING;
                    led_set_mode(LED_MODE_CONNECTING);
                    // Disconnected from bridge: eyes turn red
                    oled_display_set_eye_color(EYE_COLOR_RED);
                    oled_display_clear();
                }
                break;

            case EV_CMD_SWAP:
                cycle_target_language();
                break;

            case EV_CMD_SET_LANG:
                set_languages_internal(ev.str_val1, ev.str_val2);
                break;

            case EV_CMD_RECORD_START:
                start_recording();
                break;

            case EV_CMD_RECORD_STOP:
                stop_recording();
                break;

            case EV_BUTTON_TRIGGERED:
                switch (ev.btn_val) {
                case BTN_EVENT_PTT_PRESS:
                    start_recording();
                    break;

                case BTN_EVENT_PTT_RELEASE:
                    stop_recording();
                    break;

                case BTN_EVENT_DOUBLE_CLICK:
                    cycle_target_language();
                    break;

                case BTN_EVENT_TRIPLE_CLICK:
                case BTN_EVENT_SWAP_CLICK:
                    cycle_target_language();
                    break;

                case BTN_EVENT_SINGLE_CLICK:
                    if (s_is_toggle_recording && s_state == SM_STATE_RECORDING) {
                        s_is_toggle_recording = false;
                        stop_recording();
                    }
                    break;

                default:
                    break;
                }
                break;

            case EV_SERVER_STATUS:
                if (ev.str_val1) {
                    if (strcmp(ev.str_val1, "transcribing") == 0) {
                        oled_display_update(DISPLAY_STATE_TRANSCRIBING, s_src_lang, s_dst_lang, ev.str_val2);
                        led_set_mode(LED_MODE_WAITING);
                    } else if (strcmp(ev.str_val1, "translating") == 0 || strcmp(ev.str_val1, "synthesizing") == 0) {
                        oled_display_update(DISPLAY_STATE_TRANSLATING, s_src_lang, s_dst_lang, ev.str_val2);
                        led_set_mode(LED_MODE_WAITING);
                    } else if (strcmp(ev.str_val1, "ready") == 0 || strcmp(ev.str_val1, "idle") == 0) {
                        s_state = SM_STATE_IDLE;
                        led_set_mode(LED_MODE_IDLE);
                        oled_display_update(DISPLAY_STATE_READY, s_src_lang, s_dst_lang, ev.str_val2 ? ev.str_val2 : "Ready");
                    }
                }
                break;

            case EV_SERVER_STT:
                if (ev.str_val1) {
                    oled_display_show_text(ev.str_val1, NULL);
                }
                break;

            case EV_SERVER_TRANSLATION:
                if (ev.str_val1) {
                    strncpy(s_last_translation, ev.str_val1, sizeof(s_last_translation) - 1);
                    s_last_translation[sizeof(s_last_translation) - 1] = '\0';
                    oled_display_show_text(NULL, s_last_translation);
                }
                break;

            case EV_SERVER_TTS_START:
                s_state = SM_STATE_SPEAKING;
                led_set_mode(LED_MODE_SPEAKING);
                oled_display_update(DISPLAY_STATE_SPEAKING, s_src_lang, s_dst_lang,
                                    s_last_translation[0] ? s_last_translation : "Speaking...");
                break;

            case EV_SERVER_DONE:
                if (s_state != SM_STATE_SPEAKING) {
                    s_state = SM_STATE_IDLE;
                    led_set_mode(LED_MODE_IDLE);
                }
                break;

            case EV_SERVER_ERROR:
                s_state = SM_STATE_ERROR;
                led_set_mode(LED_MODE_ERROR);
                oled_display_update(DISPLAY_STATE_ERROR, s_src_lang, s_dst_lang, ev.str_val1 ? ev.str_val1 : "Error");
                vTaskDelay(pdMS_TO_TICKS(1500));
                s_state = SM_STATE_IDLE;
                led_set_mode(LED_MODE_IDLE);
                oled_display_update(DISPLAY_STATE_READY, s_src_lang, s_dst_lang, "Ready");
                break;
            }

            // Cleanup dynamically allocated strings
            if (ev.str_val1) free(ev.str_val1);
            if (ev.str_val2) free(ev.str_val2);
        }
    }
}

esp_err_t state_machine_init(void)
{
    s_event_queue = xQueueCreate(25, sizeof(sm_event_t));
    s_playback_ringbuf = xRingbufferCreate(32768, RINGBUF_TYPE_NOSPLIT);

    if (!s_event_queue || !s_playback_ringbuf) {
        ESP_LOGE(TAG, "Failed to create state machine queues/ringbuffer");
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(state_task, "state_task", 4096, NULL, 5, NULL);
    xTaskCreate(audio_rx_task, "audio_rx", 4096, NULL, 6, NULL);
    xTaskCreate(audio_tx_task, "audio_tx", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "State machine initialized");
    return ESP_OK;
}

void state_machine_on_wifi_event(bool connected)
{
    sm_event_t ev = {.type = EV_WIFI_STATE_CHANGED, .bool_val = connected};
    xQueueSend(s_event_queue, &ev, 0);
}

void state_machine_on_ws_event(bool connected)
{
    sm_event_t ev = {.type = EV_WS_STATE_CHANGED, .bool_val = connected};
    xQueueSend(s_event_queue, &ev, 0);
}

void state_machine_on_button_event(button_event_t event)
{
    sm_event_t ev = {.type = EV_BUTTON_TRIGGERED, .btn_val = event};
    xQueueSend(s_event_queue, &ev, 0);
}

void state_machine_on_ws_text(const ws_msg_t *msg)
{
    if (!msg || !msg->type) return;

    if (strcmp(msg->type, "status") == 0) {
        sm_event_t ev = {
            .type = EV_SERVER_STATUS,
            .str_val1 = msg->state ? strdup(msg->state) : NULL,
            .str_val2 = msg->message ? strdup(msg->message) : NULL,
        };
        xQueueSend(s_event_queue, &ev, 0);
    } else if (strcmp(msg->type, "stt") == 0) {
        sm_event_t ev = {
            .type = EV_SERVER_STT,
            .str_val1 = msg->text ? strdup(msg->text) : NULL,
        };
        xQueueSend(s_event_queue, &ev, 0);
    } else if (strcmp(msg->type, "translation") == 0) {
        const char *trans_str = (msg->display_text && strlen(msg->display_text)) ? msg->display_text : (msg->translation ? msg->translation : msg->text);
        sm_event_t ev = {
            .type = EV_SERVER_TRANSLATION,
            .str_val1 = trans_str ? strdup(trans_str) : NULL,
        };
        xQueueSend(s_event_queue, &ev, 0);
    } else if (strcmp(msg->type, "tts_audio") == 0) {
        sm_event_t ev = {.type = EV_SERVER_TTS_START};
        xQueueSend(s_event_queue, &ev, 0);
    } else if (strcmp(msg->type, "done") == 0) {
        sm_event_t ev = {.type = EV_SERVER_DONE};
        xQueueSend(s_event_queue, &ev, 0);
    } else if (strcmp(msg->type, "error") == 0) {
        sm_event_t ev = {
            .type = EV_SERVER_ERROR,
            .str_val1 = msg->message ? strdup(msg->message) : NULL,
        };
        xQueueSend(s_event_queue, &ev, 0);
    }
}

void state_machine_on_ws_binary(const uint8_t *data, size_t len)
{
    if (s_playback_ringbuf && data && len > 0) {
        xRingbufferSend(s_playback_ringbuf, data, len, pdMS_TO_TICKS(150));
    }
}

void state_machine_cycle_target_lang(void)
{
    sm_event_t ev = {.type = EV_CMD_SWAP};
    xQueueSend(s_event_queue, &ev, 0);
}

void state_machine_trigger_swap(void)
{
    state_machine_cycle_target_lang();
}

void state_machine_set_languages(const char *src, const char *dst)
{
    sm_event_t ev = {
        .type = EV_CMD_SET_LANG,
        .str_val1 = src ? strdup(src) : NULL,
        .str_val2 = dst ? strdup(dst) : NULL,
    };
    xQueueSend(s_event_queue, &ev, 0);
}

void state_machine_get_languages(char *src_out, char *dst_out)
{
    if (src_out) strcpy(src_out, s_src_lang);
    if (dst_out) strcpy(dst_out, s_dst_lang);
}

const char* state_machine_get_current_state_str(void)
{
    switch (s_state) {
    case SM_STATE_INIT:             return "INIT";
    case SM_STATE_WIFI_CONNECTING:  return "WIFI_CONNECTING";
    case SM_STATE_WS_CONNECTING:    return "HOST_CONNECTING";
    case SM_STATE_IDLE:             return "READY";
    case SM_STATE_RECORDING:        return "RECORDING";
    case SM_STATE_WAITING:          return "PROCESSING";
    case SM_STATE_SPEAKING:         return "SPEAKING";
    case SM_STATE_ERROR:            return "ERROR";
    default:                        return "UNKNOWN";
    }
}

void state_machine_web_record_start(void)
{
    sm_event_t ev = {.type = EV_CMD_RECORD_START};
    xQueueSend(s_event_queue, &ev, 0);
}

void state_machine_web_record_stop(void)
{
    sm_event_t ev = {.type = EV_CMD_RECORD_STOP};
    xQueueSend(s_event_queue, &ev, 0);
}
