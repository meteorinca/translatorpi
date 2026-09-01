#include "oled_display.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DISPLAY";

/* ========================================================================= */
/*               PROFILE: DOGBOT SPI ST7789 160x80 WITH ANIMATED EYES        */
/* ========================================================================= */
#if USE_SPI_DISPLAY

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "font5x7.h"

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static volatile bool force_blink = false;
static volatile int eye_mood = 0; // 0=happy, 1=angry, 2=neutral, 3=sad
static char oled_text_msg[128] = {0};
static volatile int oled_text_timer = 0;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static uint32_t xor_state = 123456789;
static inline uint32_t fast_rand(void) {
    xor_state ^= xor_state << 13;
    xor_state ^= xor_state >> 17;
    xor_state ^= xor_state << 5;
    return xor_state;
}

static void dog_set_eye_mood(int mood) {
    if (mood >= 0 && mood <= 3) eye_mood = mood;
}

static void dog_set_oled_text(const char* msg, int duration_ms) {
    if (!msg) {
        oled_text_msg[0] = '\0';
        oled_text_timer = 0;
        return;
    }
    strncpy(oled_text_msg, msg, sizeof(oled_text_msg) - 1);
    oled_text_msg[sizeof(oled_text_msg) - 1] = '\0';
    if (duration_ms < 0) {
        oled_text_timer = -1;
    } else {
        oled_text_timer = duration_ms / 60; // ~60ms per frame
    }
}

static void dog_dismiss_oled(void) {
    oled_text_timer = 0;
    oled_text_msg[0] = '\0';
}

static void draw_text_autoscale(uint16_t *buffer, const char* text) {
    static char last_text[128] = {0};
    static uint32_t frame_tick = 0;
    
    if (strncmp(text, last_text, sizeof(last_text)) != 0) {
        strncpy(last_text, text, sizeof(last_text) - 1);
        frame_tick = 0;
    }
    frame_tick++;

    char temp[128];
    strncpy(temp, text, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    int num_lines = 1;
    for (int i = 0; temp[i]; i++) {
        if (temp[i] == '\n') num_lines++;
    }
    
    char *lines[6];
    int line_idx = 0;
    char *p = temp;
    lines[line_idx++] = p;
    for (int i = 0; p[i] && line_idx < 6; i++) {
        if (p[i] == '\n') {
            p[i] = '\0';
            lines[line_idx++] = &p[i + 1];
        }
    }
    
    int line_scale[6] = {1, 1, 1, 1, 1, 1};
    int total_h = 0;
    
    for (int l = 0; l < num_lines; l++) {
        int len = strlen(lines[l]);
        if (len > 0) {
            line_scale[l] = 160 / (6 * len - 1);
            if (line_scale[l] > 8) line_scale[l] = 8;
            if (line_scale[l] < 2) line_scale[l] = 2;
        }
        total_h += 8 * line_scale[l];
    }
    
    while (total_h > 80) {
        int max_scale = 0;
        int max_l = -1;
        for (int l = 0; l < num_lines; l++) {
            if (line_scale[l] > max_scale && line_scale[l] > 1) {
                max_scale = line_scale[l];
                max_l = l;
            }
        }
        if (max_l == -1) break;
        line_scale[max_l]--;
        total_h -= 8;
    }
    
    int current_y = (80 - total_h) / 2;
    if (current_y < 0) current_y = 0;
    
    for (int l = 0; l < num_lines; l++) {
        int len = strlen(lines[l]);
        int scale = line_scale[l];
        int total_w = len * 6 * scale - 1 * scale;
        int start_x = (160 - total_w) / 2;
        
        if (total_w > 160) {
            int extra_w = total_w - 160 + 8;
            int pause = 30;
            int cycle = frame_tick % ((extra_w + pause) * 2);
            int offset = 0;
            if (cycle < pause) offset = 0;
            else if (cycle < pause + extra_w) offset = cycle - pause;
            else if (cycle < pause * 2 + extra_w) offset = extra_w;
            else offset = extra_w - (cycle - (pause * 2 + extra_w));
            
            start_x = 4 - offset;
        }
        
        for (int i = 0; i < len; i++) {
            char c = lines[l][i];
            if (c < 32 || c > 126) c = 32;
            const uint8_t *glyph = &font5x7[(c - 32) * 8];
            int cx = start_x + i * 6 * scale;
            for (int gx = 0; gx < 5; gx++) {
                uint8_t col = glyph[gx];
                for (int gy = 0; gy < 7; gy++) {
                    if (col & (1 << gy)) {
                        for (int dx = 0; dx < scale; dx++) {
                            for (int dy = 0; dy < scale; dy++) {
                                int px = cx + gx * scale + dx;
                                int py = current_y + gy * scale + dy;
                                if (px >= 0 && px < 160 && py >= 0 && py < 80) {
                                    buffer[py * 160 + px] = 0xFFFF;
                                }
                            }
                        }
                    }
                }
            }
        }
        current_y += 8 * scale;
    }
}

static void dog_eyes_task(void *arg) {
    uint16_t *buffer = malloc(160 * 80 * sizeof(uint16_t));
    if (!buffer) {
        ESP_LOGE(TAG, "No mem for eyes buffer");
        vTaskDelete(NULL);
        return;
    }

    int blink_timer = 0;
    int next_blink = 50 + (rand() % 100);
    bool blinking = false;
    int frame_count = 0;

    static int pupil_dx = 0;
    static int pupil_dy = 0;
    static int pupil_target_dx = 0;
    static int pupil_target_dy = 0;

    static int color_phase = 0;
    static int target_phase = 0;

    const int eye_cx[2] = { 40, 120 };
    const int eye_cy = 40;
    const int eye_rx = 34;
    const int base_ry = 32;
    const int iris_r  = 18;
    const int base_pupil_r = 7;

    while (1) {
        frame_count++;

        blink_timer++;
        if (!blinking && blink_timer > next_blink) {
            blinking = true;
            blink_timer = 0;
        }
        if (force_blink) {
            blinking = true;
            blink_timer = 0;
            force_blink = false;
        }

        int max_ry = base_ry;
        if (blinking) {
            max_ry = 3;
            if (blink_timer > 1) {
                blinking = false;
                blink_timer = 0;
                next_blink = 40 + (rand() % 100);
            }
        }

        if (rand() % 20 == 0) {
            pupil_target_dx = (rand() % 14) - 7;
            pupil_target_dy = (rand() % 8) - 4;
        }
        if (pupil_dx < pupil_target_dx) pupil_dx++;
        if (pupil_dx > pupil_target_dx) pupil_dx--;
        if (pupil_dy < pupil_target_dy) pupil_dy++;
        if (pupil_dy > pupil_target_dy) pupil_dy--;

        if (rand() % 150 == 0) {
            target_phase = (rand() % 101) - 50;
        }
        if (frame_count % 4 == 0) {
            if (color_phase < target_phase) color_phase++;
            if (color_phase > target_phase) color_phase--;
        }
        int g_mult = 256 - color_phase;
        int b_mult = 256 + color_phase;

        int pupil_r = base_pupil_r + ((frame_count / 12) % 3) - 1;
        int pupil_r_sq = pupil_r * pupil_r;

        int rx_sq = eye_rx * eye_rx;
        int ry_sq = max_ry * max_ry;
        int ellipse_limit = rx_sq * ry_sq;
        int iris_r_sq = iris_r * iris_r;
        uint16_t warm_edge = rgb565(240, 230, 220);
        uint16_t faint_catchlight = rgb565(200, 220, 255);

        if (oled_text_timer > 0 || oled_text_timer == -1) {
            if (oled_text_timer > 0) oled_text_timer--;
            memset(buffer, 0, 160 * 80 * sizeof(uint16_t));
            draw_text_autoscale(buffer, oled_text_msg);
            vTaskDelay(1);
        } else {
            // Render beautiful eyes
            for (int y = 0; y < 80; y++) {
                int dy = y - eye_cy;
                int dy_sq_rx = dy * dy * rx_sq;

                for (int x = 0; x < 160; x++) {
                    uint16_t color = 0x0000;

                    int ei_start, ei_end;
                    if (x < 6) { ei_start = 2; ei_end = 2; }
                    else if (x < 75) { ei_start = 0; ei_end = 1; }
                    else if (x < 86) { ei_start = 2; ei_end = 2; }
                    else if (x < 155) { ei_start = 1; ei_end = 2; }
                    else { ei_start = 2; ei_end = 2; }

                    for (int ei = ei_start; ei < ei_end; ei++) {
                        int dx = x - eye_cx[ei];
                        int ex_val = dx * dx * ry_sq + dy_sq_rx;
                        if (ex_val > ellipse_limit)
                            continue;

                        int inner_dx = (ei == 0) ? dx : -dx;
                        int eye_lid_y = -1000;
                        if (eye_mood == 0) {
                            eye_lid_y = -1000; // happy
                        } else if (eye_mood == 1) {
                            eye_lid_y = eye_cy - 8 - (inner_dx * 3 / 7); // angry
                        } else if (eye_mood == 2) {
                            eye_lid_y = eye_cy - 14; // neutral
                        } else if (eye_mood == 3) {
                            eye_lid_y = eye_cy - 22 + (inner_dx * 3 / 7); // sad
                        }

                        if (y <= eye_lid_y && !blinking)
                            continue;

                        int dist_sq = dx * dx + dy * dy;
                        color = (dist_sq > rx_sq * 3 / 4) ? warm_edge : 0xFFFF;

                        int idx = dx - pupil_dx;
                        int idy = dy - pupil_dy;
                        int id_sq = idx * idx + idy * idy;

                        if (id_sq <= iris_r_sq) {
                            int frac = id_sq * 256 / iris_r_sq;
                            uint8_t r, g, b;
                            if (frac > 200) {
                                r = 100; g = 200; b = 255;
                            } else if (frac > 120) {
                                r = 80; g = 180; b = 240;
                            } else if (frac > 50) {
                                r = 60; g = 150; b = 220;
                            } else {
                                r = 40; g = 120; b = 200;
                            }

                            uint32_t rr = fast_rand();
                            if ((rr & 0xF) == 0) {
                                r = 255; g = 200; b = 100;
                            }
                            if ((rr & 0x1F) == 1) {
                                r = 100; g = 255; b = 200;
                            }

                            int ig = (g * g_mult) >> 8;
                            int ib = (b * b_mult) >> 8;
                            g = (ig > 255) ? 255 : ig;
                            b = (ib > 255) ? 255 : ib;
                            color = rgb565(r, g, b);

                            if (id_sq <= pupil_r_sq) {
                                color = 0x0000;
                            } else {
                                int cl_ox = (ei == 0) ? 6 : -6;
                                int hx = idx - cl_ox;
                                int hy = idy + 6;
                                if (hx * hx + hy * hy <= 9) {
                                    color = 0xFFFF;
                                }
                                int hx2 = idx + ((ei == 0) ? -3 : 3);
                                int hy2 = idy + 4;
                                if (hx2 * hx2 + hy2 * hy2 <= 4) {
                                    color = faint_catchlight;
                                }
                            }
                        }
                        break;
                    }
                    buffer[y * 160 + x] = color;
                }

                if ((y & 0xF) == 0xF) {
                    vTaskDelay(1);
                }
            }

            // Minimal cute mouth
            int mouth_w = 2 + ((frame_count / 12) % 2);
            int mouth_h = 1 + ((frame_count / 25) % 2);
            int mouth_y = 58; 
            for (int my = mouth_y; my < mouth_y + mouth_h; my++) {
                for (int mx = 80 - mouth_w; mx <= 80 + mouth_w; mx++) {
                    buffer[my * 160 + mx] = warm_edge;
                }
            }
        }

        if (s_panel_handle) {
            esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, 160, 80, buffer);
        }
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

esp_err_t oled_display_init(void)
{
    ESP_LOGI(TAG, "Initializing Dogbot ST7789 SPI Display (MOSI:%d, CLK:%d, DC:%d)",
             SPI_MOSI_PIN, SPI_CLK_PIN, SPI_DC_PIN);

    spi_bus_config_t buscfg = {
        .sclk_io_num = SPI_CLK_PIN,
        .mosi_io_num = SPI_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 160 * 80 * 2 + 8
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = SPI_DC_PIN,
        .cs_gpio_num = -1,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LCD panel IO: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_reset(s_panel_handle);
    esp_lcd_panel_init(s_panel_handle);
    esp_lcd_panel_set_gap(s_panel_handle, 0, 24);
    esp_lcd_panel_swap_xy(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, false, true);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    // Start background animated eyes task
    xTaskCreate(dog_eyes_task, "dog_eyes", 5120, NULL, 1, NULL);

    // Run identical Dogbot boot sequence
    dog_set_oled_text("'Ello\nHuman!", 2500);
    vTaskDelay(pdMS_TO_TICKS(1800));
    dog_set_oled_text(" Mr MoJ \nTranslate \nBot", 2500);
    vTaskDelay(pdMS_TO_TICKS(2200));

    return ESP_OK;
}

void oled_display_update(display_state_t state, const char *src_lang, const char *dst_lang, const char *msg)
{
    switch (state) {
    case DISPLAY_STATE_BOOT:
        dog_set_oled_text("'Ello\nHuman!", 2500);
        break;
    case DISPLAY_STATE_CONNECTING_WIFI:
        dog_set_oled_text("Connecting\nWiFi...", -1);
        break;
    case DISPLAY_STATE_CONNECTING_WS:
        dog_set_oled_text("Connecting\nHost...", -1);
        break;
    case DISPLAY_STATE_READY:
        dog_set_eye_mood(0); // Happy
        dog_dismiss_oled();  // Reveal animated eyes!
        break;
    case DISPLAY_STATE_RECORDING:
        dog_set_oled_text("Listening...\nSpeak Now", -1);
        break;
    case DISPLAY_STATE_TRANSCRIBING:
        dog_set_oled_text("Thinking...", -1);
        break;
    case DISPLAY_STATE_TRANSLATING:
        dog_set_oled_text("Translating...", -1);
        break;
    case DISPLAY_STATE_SPEAKING:
        dog_set_eye_mood(0);
        dog_set_oled_text(msg && strlen(msg) ? msg : "Speaking...", -1);
        break;
    case DISPLAY_STATE_ERROR:
        dog_set_eye_mood(3); // Sad
        dog_set_oled_text(msg ? msg : "Error", 3000);
        break;
    default:
        break;
    }
}

void oled_display_show_text(const char *stt_text, const char *trans_text)
{
    if (trans_text && strlen(trans_text)) {
        dog_set_eye_mood(0);
        dog_set_oled_text(trans_text, 6000);
    } else if (stt_text && strlen(stt_text)) {
        dog_set_oled_text(stt_text, 4000);
    }
}

void oled_display_clear(void)
{
    dog_dismiss_oled();
}

/* ========================================================================= */
/*               PROFILE: BREADBOARD I2C SSD1306 128x32                      */
/* ========================================================================= */
#else

#include "driver/i2c.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT 32
#define OLED_PAGES  (OLED_HEIGHT / 8)

static uint8_t s_framebuffer[OLED_WIDTH * OLED_PAGES];

static const uint8_t font6x8[][6] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // ' ' (0x20)
    {0x00, 0x00, 0x5F, 0x00, 0x00, 0x00}, // '!'
    {0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00}, // '0'
};

static esp_err_t oled_write_cmd(uint8_t cmd) {
    i2c_cmd_handle_t link = i2c_cmd_link_create();
    i2c_master_start(link);
    i2c_master_write_byte(link, (I2C_OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(link, 0x00, true);
    i2c_master_write_byte(link, cmd, true);
    i2c_master_stop(link);
    esp_err_t ret = i2c_master_cmd_begin(I2C_OLED_PORT, link, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(link);
    return ret;
}

esp_err_t oled_display_init(void) {
    return ESP_OK;
}

void oled_display_update(display_state_t state, const char *src_lang, const char *dst_lang, const char *msg) {
}

void oled_display_show_text(const char *stt_text, const char *trans_text) {
}

void oled_display_clear(void) {
}

#endif
