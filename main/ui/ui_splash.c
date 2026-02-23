/**
 * @file ui_splash.c
 * @brief Splash screen and SD card error screen
 *
 * Contains the JPEG splash image loader (writes directly to the LCD
 * framebuffer, pre-LVGL) and the SD-card-missing error screen (uses LVGL).
 */

#include "ui_common.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_splash";

/* ========================================================================= */
/* Splash Image (direct framebuffer, no LVGL)                                */
/* ========================================================================= */

esp_err_t ui_splash_show_image(esp_lcd_panel_handle_t panel,
                               const char *filepath)
{
    ESP_LOGI(TAG, "Loading splash image: %s", filepath);

    FILE *file = fopen(filepath, "rb");
    if (!file) {
        ESP_LOGW(TAG, "Splash image not found: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(file, 0, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    uint8_t *jpeg_buf = heap_caps_malloc(file_size,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for JPEG", (int)file_size);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t read_size = fread(jpeg_buf, 1, file_size, file);
    fclose(file);
    if (read_size != file_size) {
        free(jpeg_buf);
        return ESP_FAIL;
    }

    /* Validate JPEG header (SOI marker) */
    if (jpeg_buf[0] != 0xFF || jpeg_buf[1] != 0xD8) {
        ESP_LOGE(TAG, "Invalid JPEG — missing SOI marker");
        free(jpeg_buf);
        return ESP_FAIL;
    }

    /* Reject progressive JPEG (SOF2 marker 0xFFC2) — not supported by TinyJPEG */
    for (size_t i = 0; i < file_size - 1; i++) {
        if (jpeg_buf[i] == 0xFF && jpeg_buf[i + 1] == 0xC2) {
            ESP_LOGE(TAG, "Progressive JPEG not supported — convert to baseline");
            free(jpeg_buf);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    /* Allocate RGB565 output buffer */
    size_t out_buf_size = CONFIG_LCD_H_RES * CONFIG_LCD_V_RES * 2;
    uint8_t *out_buf = heap_caps_malloc(out_buf_size,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_buf) {
        free(jpeg_buf);
        return ESP_ERR_NO_MEM;
    }

    /* TinyJPEG working buffer */
    size_t work_buf_size = 3100;
    uint8_t *work_buf = heap_caps_malloc(work_buf_size,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work_buf) {
        free(out_buf);
        free(jpeg_buf);
        return ESP_ERR_NO_MEM;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = jpeg_buf,
        .indata_size = file_size,
        .outbuf = out_buf,
        .outbuf_size = out_buf_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = 0 },
        .advanced = {
            .working_buffer = work_buf,
            .working_buffer_size = work_buf_size,
        },
    };

    esp_jpeg_image_output_t outimg;
    esp_err_t ret = esp_jpeg_decode(&cfg, &outimg);
    free(work_buf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        free(out_buf);
        free(jpeg_buf);
        return ret;
    }

    ESP_LOGI(TAG, "Decoded %dx%d splash image", outimg.width, outimg.height);

    /* Blit decoded image to the LCD framebuffer (centered, clipped) */
    void *fb0 = NULL;
    ret = esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb0);
    if (ret != ESP_OK || !fb0) {
        free(out_buf);
        free(jpeg_buf);
        return ret;
    }

    uint16_t *framebuffer = (uint16_t *)fb0;
    uint16_t *img_data    = (uint16_t *)out_buf;

    int lcd_w = CONFIG_LCD_H_RES;
    int lcd_h = CONFIG_LCD_V_RES;
    int img_w = outimg.width;
    int img_h = outimg.height;

    memset(framebuffer, 0, lcd_w * lcd_h * 2);

    int off_x  = (lcd_w > img_w) ? (lcd_w - img_w) / 2 : 0;
    int off_y  = (lcd_h > img_h) ? (lcd_h - img_h) / 2 : 0;
    int copy_w = (img_w < lcd_w) ? img_w : lcd_w;
    int copy_h = (img_h < lcd_h) ? img_h : lcd_h;

    for (int y = 0; y < copy_h; y++) {
        memcpy(&framebuffer[(y + off_y) * lcd_w + off_x],
               &img_data[y * img_w],
               copy_w * sizeof(uint16_t));
    }

    free(out_buf);
    free(jpeg_buf);

    ESP_LOGI(TAG, "Splash image displayed");
    return ESP_OK;
}

/* ========================================================================= */
/* SD Card Error Screen (LVGL)                                               */
/* ========================================================================= */

void ui_splash_show_sd_error(void)
{
    ESP_LOGE(TAG, "SD card not detected — showing error screen");

    /* We need LVGL for the error screen */
    lv_disp_t  *disp = NULL;
    lv_indev_t *touch = NULL;
    esp_err_t ret = ui_init(&disp, &touch);
    if (ret != ESP_OK) {
        /* Can't even bring up the display — just log forever */
        while (1) {
            ESP_LOGE(TAG, "SD Card missing and LVGL init failed!");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    ui_lock();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Warning icon */
    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFF9800), LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -80);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SD Card Not Detected");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    /* Instructions */
    lv_obj_t *body = lv_label_create(scr);
    lv_label_set_text(body,
        "Please insert an SD card with the required\n"
        "configuration files and restart the device.\n\n"
        "Required files:\n"
        "  - nodeid.txt (LCC node ID)\n"
        "  - turnouts.json (turnout definitions)\n\n"
        "Optional files:\n"
        "  - panel.json (layout diagram)");
    lv_obj_set_style_text_font(body, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(body, lv_color_hex(0xB0B0B0), LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 70);

    ui_unlock();

    /* Halt — user must insert card and restart */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGW(TAG, "SD Card missing — insert card and restart device");
    }
}
