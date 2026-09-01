#include "i2s_audio.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "AUDIO";

static i2s_chan_handle_t s_tx_chan = NULL;
static bool s_amp_enabled = false;

#if AUDIO_USE_ADC_MIC
#include "esp_adc/adc_continuous.h"
#include "soc/soc_caps.h"

static adc_continuous_handle_t s_adc_handle = NULL;
static int32_t s_dc_offset = 2048; // mid-point for 12-bit ADC
#else
static i2s_chan_handle_t s_rx_chan = NULL;
#endif

void i2s_audio_set_amp(bool enable)
{
#ifdef PIN_AMP_ENABLE
    if (enable != s_amp_enabled) {
        gpio_set_level(PIN_AMP_ENABLE, enable ? 1 : 0);
        s_amp_enabled = enable;
        if (enable) {
            vTaskDelay(pdMS_TO_TICKS(30)); // allow amp rail to stabilize
        }
        ESP_LOGD(TAG, "Audio Amp set to: %s", enable ? "ON" : "OFF");
    }
#endif
}

esp_err_t i2s_audio_init(void)
{
#ifdef PIN_AMP_ENABLE
    // Configure amplifier control pin as output, initialized OFF (0) to eliminate any idle hiss/whine
    gpio_config_t amp_io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_AMP_ENABLE),
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&amp_io_conf);
    gpio_set_level(PIN_AMP_ENABLE, 0); // Keep amp strictly OFF when idle!
    s_amp_enabled = false;
    ESP_LOGI(TAG, "Amplifier control initialized on GPIO%d (OFF)", PIN_AMP_ENABLE);
#endif

#if AUDIO_USE_PDM_TX
    // --- Dogbot PDM Speaker TX ---
    ESP_LOGI(TAG, "Initializing Dogbot PDM Audio TX on CLK:GPIO%d, DATA:GPIO%d @ %d Hz",
             AUDIO_PDM_CLK_PIN, AUDIO_PDM_DATA_PIN, AUDIO_SAMPLE_RATE);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = AUDIO_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = AUDIO_DMA_BUF_LEN;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate PDM TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_pdm_tx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = AUDIO_PDM_CLK_PIN,
            .dout = AUDIO_PDM_DATA_PIN,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ret = i2s_channel_init_pdm_tx_mode(s_tx_chan, &pdm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init PDM TX mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable PDM TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

#else
    // --- Standard I2S Full-Duplex (Breadboard INMP441 + MAX98357A) ---
    ESP_LOGI(TAG, "Initializing I2S_NUM_0 Full-Duplex STD mode @ %d Hz", AUDIO_SAMPLE_RATE);
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = AUDIO_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = AUDIO_DMA_BUF_LEN;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channels: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) return ret;
    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) return ret;
    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) return ret;
    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) return ret;
#endif

#if AUDIO_USE_ADC_MIC
    // --- Dogbot Continuous ADC Microphone Sampling (GPIO 2, ADC1 CH2) ---
    ESP_LOGI(TAG, "Initializing Continuous ADC1 CH%d Mic sampling @ %d Hz", MIC_ADC_CHANNEL, AUDIO_SAMPLE_RATE);
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 8192,
        .conv_frame_size = 512,
    };
    ret = adc_continuous_new_handle(&adc_config, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ADC continuous handle: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = AUDIO_SAMPLE_RATE,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    adc_digi_pattern_config_t adc_pattern = {
        .atten = ADC_ATTEN_DB_12,
        .channel = MIC_ADC_CHANNEL,
        .unit = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    dig_cfg.pattern_num = 1;
    dig_cfg.adc_pattern = &adc_pattern;

    ret = adc_continuous_config(s_adc_handle, &dig_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC continuous: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = adc_continuous_start(s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ADC continuous: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "ADC continuous mic sampling started successfully");
#endif

    return ESP_OK;
}

size_t i2s_audio_read(int16_t *buf, size_t samples, TickType_t timeout)
{
#if AUDIO_USE_ADC_MIC
    if (!s_adc_handle || !buf || samples == 0) return 0;

    // Use pre-allocated static buffer to avoid heap fragmentation during streaming
    static uint8_t s_raw_adc_buf[8192];
    uint32_t bytes_to_read = samples * SOC_ADC_DIGI_RESULT_BYTES;
    if (bytes_to_read > sizeof(s_raw_adc_buf)) {
        bytes_to_read = sizeof(s_raw_adc_buf);
    }
    uint8_t *raw_buf = s_raw_adc_buf;

    uint32_t out_len = 0;
    esp_err_t ret = adc_continuous_read(s_adc_handle, raw_buf, bytes_to_read, &out_len, timeout);
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "adc_continuous_read error: %s", esp_err_to_name(ret));
        return 0;
    }

    size_t samples_read = out_len / SOC_ADC_DIGI_RESULT_BYTES;
    for (size_t i = 0; i < samples_read; i++) {
        adc_digi_output_data_t *p = (adc_digi_output_data_t *)&raw_buf[i * SOC_ADC_DIGI_RESULT_BYTES];
        int32_t raw = (int32_t)p->type2.data;

        // Dynamic DC tracking filter (slowly adapts to mic bias)
        s_dc_offset = (s_dc_offset * 127 + raw) / 128;
        int32_t ac_val = raw - s_dc_offset;

        // Apply gain amplification for electret mic signal
        int32_t amplified = ac_val << 5; // ~32x gain boost
        if (amplified > 32767) amplified = 32767;
        if (amplified < -32768) amplified = -32768;

        buf[i] = (int16_t)amplified;
    }

    return samples_read;

#else
    if (!s_rx_chan) return 0;
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx_chan, buf, samples * sizeof(int16_t), &bytes_read, timeout);
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "i2s read error: %s", esp_err_to_name(ret));
    }
    return bytes_read / sizeof(int16_t);
#endif
}

size_t i2s_audio_write(const int16_t *buf, size_t samples, TickType_t timeout)
{
    if (!s_tx_chan || !buf || samples == 0) return 0;

    // Power gate amplifier: turn on when audio samples arrive
    i2s_audio_set_amp(true);

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(s_tx_chan, buf, samples * sizeof(int16_t), &bytes_written, timeout);
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "i2s write error: %s", esp_err_to_name(ret));
    }
    return bytes_written / sizeof(int16_t);
}

void i2s_audio_clear_buffers(void)
{
    if (s_tx_chan) {
        int16_t silence[256] = {0};
        size_t written = 0;
        i2s_channel_write(s_tx_chan, silence, sizeof(silence), &written, pdMS_TO_TICKS(50));
    }
    // Turn off amplifier to ensure complete silence without any hiss
    i2s_audio_set_amp(false);
}
