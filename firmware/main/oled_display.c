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
static volatile eye_color_t s_eye_color = EYE_COLOR_NORMAL;
static char oled_text_msg[512] = {0};
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

void oled_display_set_eye_color(eye_color_t color) {
    s_eye_color = color;
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

// Convert UTF-8 characters (especially Spanish accents, inverted punctuation, curly quotes) to clean ASCII
static void sanitize_utf8_to_ascii(const char *src, char *dst, size_t dst_max)
{
    if (!src || !dst || dst_max == 0) return;
    size_t di = 0;
    const uint8_t *p = (const uint8_t *)src;
    while (*p && di + 1 < dst_max) {
        if (*p < 0x80) {
            dst[di++] = (char)*p++;
        } else if (*p == 0xC3) {
            p++;
            if (!*p) break;
            uint8_t c2 = *p++;
            switch (c2) {
            case 0xA1: case 0x81: case 0xA0: case 0x80: case 0xA4: case 0x84: dst[di++] = 'a'; break; // á, Á, à, À, ä, Ä
            case 0xA9: case 0x89: case 0xA8: case 0x88: case 0xAB: case 0x8B: dst[di++] = 'e'; break; // é, É, è, È, ë, Ë
            case 0xAD: case 0x8D: case 0xAC: case 0x8C: case 0xAF: case 0x8F: dst[di++] = 'i'; break; // í, Í, ì, Ì, ï, Ï
            case 0xB3: case 0x93: case 0xB2: case 0x92: case 0xB6: case 0x96: dst[di++] = 'o'; break; // ó, Ó, ò, Ò, ö, Ö
            case 0xBA: case 0x9A: case 0xB9: case 0x99: case 0xBC: case 0x9C: dst[di++] = 'u'; break; // ú, Ú, ù, Ù, ü, Ü
            case 0xB1: dst[di++] = 'n'; break; // ñ
            case 0x91: dst[di++] = 'N'; break; // Ñ
            case 0xA7: case 0x87: dst[di++] = 'c'; break; // ç, Ç
            default: dst[di++] = ' '; break;
            }
        } else if (*p == 0xC2) {
            p++;
            if (!*p) break;
            uint8_t c2 = *p++;
            switch (c2) {
            case 0xBF: dst[di++] = '?'; break; // ¿
            case 0xA1: dst[di++] = '!'; break; // ¡
            case 0xAB: case 0xBB: dst[di++] = '"'; break; // «, »
            default: dst[di++] = ' '; break;
            }
        } else if (*p == 0xE2) {
            p++;
            if (*p == 0x80) {
                p++;
                if (!*p) break;
                uint8_t c3 = *p++;
                switch (c3) {
                case 0x98: case 0x99: dst[di++] = '\''; break; // ‘, ’
                case 0x9C: case 0x9D: dst[di++] = '"'; break;  // “, ”
                case 0x93: case 0x94: dst[di++] = '-'; break;  // –, —
                default: dst[di++] = ' '; break;
                }
            } else {
                p++;
                if (*p) p++;
            }
        } else {
            // Skip multibyte continuation bytes
            if ((*p & 0xE0) == 0xC0) { p += 2; }
            else if ((*p & 0xF0) == 0xE0) { p += 3; }
            else if ((*p & 0xF8) == 0xF0) { p += 4; }
            else { p++; }
            dst[di++] = ' ';
        }
    }
    dst[di] = '\0';
}

static void wrap_text_two_lines(const char *input, char *line1, size_t max1, char *line2, size_t max2)
{
    line1[0] = '\0';
    line2[0] = '\0';
    if (!input || !*input) return;

    const char *nl = strchr(input, '\n');
    if (nl) {
        size_t l1_len = nl - input;
        if (l1_len >= max1) l1_len = max1 - 1;
        strncpy(line1, input, l1_len);
        line1[l1_len] = '\0';
        strncpy(line2, nl + 1, max2 - 1);
        line2[max2 - 1] = '\0';
        char *nl2 = strchr(line2, '\n');
        if (nl2) *nl2 = '\0';
        return;
    }

    size_t total_len = strlen(input);
    if (total_len <= 13) {
        strncpy(line1, input, max1 - 1);
        line1[max1 - 1] = '\0';
        return;
    }

    // Find space closest to midpoint
    size_t mid = total_len / 2;
    int best_split = -1;
    int min_dist = 9999;
    for (size_t i = 0; i < total_len; i++) {
        if (input[i] == ' ') {
            int dist = abs((int)i - (int)mid);
            if (dist < min_dist) {
                min_dist = dist;
                best_split = (int)i;
            }
        }
    }

    if (best_split > 0) {
        size_t l1_len = best_split;
        if (l1_len >= max1) l1_len = max1 - 1;
        strncpy(line1, input, l1_len);
        line1[l1_len] = '\0';

        const char *p2 = input + best_split + 1;
        while (*p2 == ' ') p2++;
        strncpy(line2, p2, max2 - 1);
        line2[max2 - 1] = '\0';
    } else {
        size_t l1_len = mid;
        if (l1_len >= max1) l1_len = max1 - 1;
        strncpy(line1, input, l1_len);
        line1[l1_len] = '\0';
        strncpy(line2, input + mid, max2 - 1);
        line2[max2 - 1] = '\0';
    }
}

static void draw_glyph(uint16_t *buffer, char c, int x, int y, int scale, uint16_t color) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = &font5x7[(c - 32) * 8];
    for (int gx = 0; gx < 5; gx++) {
        uint8_t col = glyph[gx];
        for (int gy = 0; gy < 7; gy++) {
            if (col & (1 << gy)) {
                for (int dx = 0; dx < scale; dx++) {
                    for (int dy = 0; dy < scale; dy++) {
                        int px = x + gx * scale + dx;
                        int py = y + gy * scale + dy;
                        if (px >= 0 && px < 160 && py >= 0 && py < 80) {
                            buffer[py * 160 + px] = color;
                        }
                    }
                }
            }
        }
    }
}

static void draw_text_autoscale(uint16_t *buffer, const char* text) {
    static char last_rendered_text[512] = {0};
    static uint32_t frame_tick = 0;

    if (strncmp(text, last_rendered_text, sizeof(last_rendered_text)) != 0) {
        strncpy(last_rendered_text, text, sizeof(last_rendered_text) - 1);
        frame_tick = 0;
    }
    frame_tick++;

    char clean_text[512];
    sanitize_utf8_to_ascii(text, clean_text, sizeof(clean_text));

    char line1[256] = {0};
    char line2[256] = {0};
    wrap_text_two_lines(clean_text, line1, sizeof(line1), line2, sizeof(line2));

    const int scale = 2;
    const int char_w = 6 * scale; // 12px per char
    int len1 = strlen(line1);
    int len2 = strlen(line2);
    int w1 = len1 * char_w;
    int w2 = len2 * char_w;

    // If only one short line, center it vertically
    if (len2 == 0 && w1 <= 160) {
        int sx = (160 - w1) / 2;
        int sy = (80 - 14) / 2; // ~33
        for (int i = 0; i < len1; i++) {
            draw_glyph(buffer, line1[i], sx + i * char_w, sy, scale, 0xFFFF);
        }
        return;
    }

    int max_w = (w1 > w2) ? w1 : w2;
    int y1 = 18;
    int y2 = 44;

    if (max_w <= 160) {
        // Fits on screen without scrolling
        int sx1 = (160 - w1) / 2;
        int sx2 = (160 - w2) / 2;
        for (int i = 0; i < len1; i++) {
            draw_glyph(buffer, line1[i], sx1 + i * char_w, y1, scale, 0xFFFF);
        }
        for (int i = 0; i < len2; i++) {
            draw_glyph(buffer, line2[i], sx2 + i * char_w, y2, scale, 0xFFFF);
        }
    } else {
        // Smooth horizontal marquee scrolling
        int extra_w = max_w - 160 + 16;
        int pause_start = 35; // ~2.1s initial read pause
        int pause_end = 35;   // ~2.1s end read pause
        int pause_loop = 12;  // ~0.7s before repeat
        int cycle_len = pause_start + extra_w + pause_end + pause_loop;
        int phase = frame_tick % cycle_len;

        int offset = 0;
        if (phase < pause_start) {
            offset = 0;
        } else if (phase < pause_start + extra_w) {
            offset = phase - pause_start;
        } else {
            offset = extra_w;
        }

        int sx1 = (w1 <= 160) ? (160 - w1) / 2 : (8 - (int)((long long)offset * (w1 - 160 + 16) / extra_w));
        int sx2 = (w2 <= 160) ? (160 - w2) / 2 : (8 - (int)((long long)offset * (w2 - 160 + 16) / extra_w));

        for (int i = 0; i < len1; i++) {
            draw_glyph(buffer, line1[i], sx1 + i * char_w, y1, scale, 0xFFFF);
        }
        for (int i = 0; i < len2; i++) {
            draw_glyph(buffer, line2[i], sx2 + i * char_w, y2, scale, 0xFFFF);
        }
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

                            if (s_eye_color == EYE_COLOR_RED) {
                                // Glowing vivid robot red iris
                                if (frac > 200) {
                                    r = 255; g = 50; b = 50;
                                } else if (frac > 120) {
                                    r = 240; g = 30; b = 30;
                                } else if (frac > 50) {
                                    r = 210; g = 15; b = 15;
                                } else {
                                    r = 170; g = 0; b = 0;
                                }

                                uint32_t rr = fast_rand();
                                if ((rr & 0xF) == 0) {
                                    r = 255; g = 140; b = 40; // fiery sparkle
                                }
                                color = rgb565(r, g, b);

                                if (id_sq <= pupil_r_sq) {
                                    color = 0x0000;
                                } else {
                                    int cl_ox = (ei == 0) ? 6 : -6;
                                    int hx = idx - cl_ox;
                                    int hy = idy + 6;
                                    if (hx * hx + hy * hy <= 9) {
                                        color = rgb565(255, 210, 210);
                                    }
                                    int hx2 = idx + ((ei == 0) ? -3 : 3);
                                    int hy2 = idy + 4;
                                    if (hx2 * hx2 + hy2 * hy2 <= 4) {
                                        color = rgb565(255, 120, 120);
                                    }
                                }
                            } else {
                                // Normal cyan/blue iris
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
        // Do not cover eyes if already revealed with red status
        if (oled_text_timer <= 0) {
            dog_dismiss_oled();
        }
        break;
    case DISPLAY_STATE_READY:
        dog_set_eye_mood(0); // Happy
        // DO NOT dismiss active translation or language switch before its timer expires!
        if (oled_text_timer <= 0) {
            dog_dismiss_oled();  // Reveal animated eyes!
        }
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
        // Do not overwrite scrolling translation text if currently active
        if (oled_text_timer <= 0) {
            dog_set_oled_text(msg && strlen(msg) ? msg : "Speaking...", -1);
        }
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
        // Extended readable duration (~13 seconds) to allow seamless 2-line scrolling
        dog_set_oled_text(trans_text, 13000);
    } else if (stt_text && strlen(stt_text)) {
        dog_set_oled_text(stt_text, 4000);
    }
}

void oled_display_show_languages(const char *src_lang, const char *dst_lang, const char *dst_name)
{
    char buf[128];
    char src_u[8] = {0};
    char dst_u[8] = {0};
    int i;
    for (i = 0; src_lang && src_lang[i] && i < 7; i++) {
        src_u[i] = (src_lang[i] >= 'a' && src_lang[i] <= 'z') ? (src_lang[i] - 'a' + 'A') : src_lang[i];
    }
    for (i = 0; dst_lang && dst_lang[i] && i < 7; i++) {
        dst_u[i] = (dst_lang[i] >= 'a' && dst_lang[i] <= 'z') ? (dst_lang[i] - 'a' + 'A') : dst_lang[i];
    }

    snprintf(buf, sizeof(buf), "%s -> %s\n%s", src_u, dst_u, (dst_name && dst_name[0]) ? dst_name : dst_u);
    dog_set_oled_text(buf, 2600); // 2.6 seconds display
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

void oled_display_show_languages(const char *src_lang, const char *dst_lang, const char *dst_name) {
}

void oled_display_set_eye_color(eye_color_t color) {
}

void oled_display_clear(void) {
}

#endif
