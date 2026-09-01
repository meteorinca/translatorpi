#pragma once

#include "driver/gpio.h"

/* ========================================================================= */
/*                          DEVICE NUMBER & IDENTITY                         */
/* ========================================================================= */
#ifndef DEVICE_NUMBER
#define DEVICE_NUMBER                1
#endif

/* ========================================================================= */
/*                          HARDWARE PROFILE SELECTION                       */
/* ========================================================================= */
// Select active profile: BOARD_PROFILE_BREADBOARD_C3 or BOARD_PROFILE_DOGBOT_C3
// #define BOARD_PROFILE_BREADBOARD_C3
#define BOARD_PROFILE_DOGBOT_C3

/* ========================================================================= */
/*                             NETWORK CONFIGURATION                         */
/* ========================================================================= */
#if __has_include("secrets.h")
#include "secrets.h"
#define CONFIG_WIFI_SSID             WIFI_SSID_1
#define CONFIG_WIFI_PASSWORD         WIFI_PASS_1
#else
#define CONFIG_WIFI_SSID             "YOUR_WIFI_SSID"
#define CONFIG_WIFI_PASSWORD         "YOUR_WIFI_PASSWORD"
#endif
#define CONFIG_WIFI_MAXIMUM_RETRY    10

// Host PC Go Bridge / Python WebSocket Endpoint
#define CONFIG_WS_SERVER_URI         "ws://10.0.0.45:8765"

// Default Language Pair
#define CONFIG_DEFAULT_SRC_LANG      "en"
#define CONFIG_DEFAULT_DST_LANG      "zh"

/* ========================================================================= */
/*                             AUDIO CONFIGURATION                           */
/* ========================================================================= */
#define AUDIO_SAMPLE_RATE            16000
#define AUDIO_CHUNK_SAMPLES          1600       // 100 ms chunk @ 16 kHz mono
#define AUDIO_CHUNK_BYTES            (AUDIO_CHUNK_SAMPLES * sizeof(int16_t)) // 3200 bytes
#define AUDIO_DMA_BUF_COUNT          8
#define AUDIO_DMA_BUF_LEN            512

/* ========================================================================= */
/*                   PROFILE 1: BREADBOARD_C3 (Standard Thin Client)          */
/* ========================================================================= */
#ifdef BOARD_PROFILE_BREADBOARD_C3

#define DEVICE_NAME_PREFIX           "translator"
#define DEVICE_NAME_STR              "breadboard_c3"

// I2S Bus (Single I2S_NUM_0, Full-Duplex Shared Clock)
#define I2S_BCLK_PIN                 GPIO_NUM_4
#define I2S_WS_PIN                   GPIO_NUM_5
#define I2S_DIN_PIN                  GPIO_NUM_6   // INMP441 Mic SD
#define I2S_DOUT_PIN                 GPIO_NUM_7   // MAX98357A Amp DIN

// Buttons
#define PIN_BUTTON_PTT               GPIO_NUM_10  // Hold to talk, release to send
#define PIN_BUTTON_SWAP              GPIO_NUM_3   // Click to swap languages
#define HAS_SECONDARY_BUTTON         1

// OLED (I2C SSD1306 0.91" 128x32)
#define USE_I2C_OLED                 1
#define I2C_OLED_SDA_PIN             GPIO_NUM_0
#define I2C_OLED_SCL_PIN             GPIO_NUM_1
#define I2C_OLED_PORT                0
#define I2C_OLED_ADDR                0x3C

// Status LED (WS2812B NeoPixel on GPIO8)
#define STATUS_LED_PIN               GPIO_NUM_8
#define USE_WS2812B_LED              1

#endif

/* ========================================================================= */
/*                   PROFILE 2: DOGBOT_C3 (Dogbot v1 Quadruped Platform)     */
/* ========================================================================= */
#ifdef BOARD_PROFILE_DOGBOT_C3

#define DEVICE_NAME_PREFIX           "dogbot"
#define DEVICE_NAME_STR              "dogbot_c3"

// Audio PDM Speaker & ADC Microphone
#define AUDIO_USE_PDM_TX             1
#define AUDIO_USE_ADC_MIC            1
#define AUDIO_PDM_CLK_PIN            GPIO_NUM_7   // Audio PDM Clock
#define AUDIO_PDM_DATA_PIN           GPIO_NUM_6   // Audio PDM Data
#define PIN_AMP_ENABLE               GPIO_NUM_3   // Audio amplifier control (active HIGH)
#define MIC_ADC_CHANNEL              ADC_CHANNEL_2 // GPIO 2 (ADC1 Channel 2)

// Dogbot Single Button on Top (Multi-click: double click to speak, triple to swap)
#define PIN_BUTTON_PTT               GPIO_NUM_9   // Boot button on Dogbot
#define HAS_SECONDARY_BUTTON         0

// Display (Dogbot onboard LCD / OLED)
#define USE_I2C_OLED                 0
#define USE_SPI_DISPLAY              1
#define SPI_MOSI_PIN                 GPIO_NUM_4
#define SPI_CLK_PIN                  GPIO_NUM_5
#define SPI_DC_PIN                   GPIO_NUM_10

// LED Strip (WS2812B on GPIO8)
#define STATUS_LED_PIN               GPIO_NUM_8
#define USE_WS2812B_LED              1

#endif
