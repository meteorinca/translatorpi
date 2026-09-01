#pragma once

#include "esp_err.h"
#include "button.h"
#include "ws_client.h"

typedef enum {
    SM_STATE_INIT,
    SM_STATE_WIFI_CONNECTING,
    SM_STATE_WS_CONNECTING,
    SM_STATE_IDLE,
    SM_STATE_RECORDING,
    SM_STATE_WAITING,
    SM_STATE_SPEAKING,
    SM_STATE_ERROR,
} sm_state_t;

esp_err_t state_machine_init(void);
void state_machine_on_wifi_event(bool connected);
void state_machine_on_ws_event(bool connected);
void state_machine_on_button_event(button_event_t event);
void state_machine_on_ws_text(const ws_msg_t *msg);
void state_machine_on_ws_binary(const uint8_t *data, size_t len);

// Web UI & Remote API Control hooks
void state_machine_cycle_target_lang(void);
void state_machine_trigger_swap(void);
void state_machine_set_languages(const char *src, const char *dst);
void state_machine_get_languages(char *src_out, char *dst_out);
const char* state_machine_get_current_state_str(void);
void state_machine_web_record_start(void);
void state_machine_web_record_stop(void);
