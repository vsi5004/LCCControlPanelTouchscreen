/**
 * @file main.c
 * @brief LCC Turnout Control Panel — Application Entry Point
 *
 * Orchestrates hardware init, SD card loading, LCC/OpenMRN startup, and
 * LVGL UI creation.  All display / screen code lives in the ui/ layer;
 * this file only wires modules together and runs the main loop.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "driver/i2c.h"

// Board drivers
#include "ch422g.h"
#include "waveshare_lcd.h"
#include "waveshare_touch.h"
#include "waveshare_sd.h"

// UI
#include "ui_common.h"

// App modules
#include "app/turnout_manager.h"
#include "app/panel_layout.h"
#include "app/lcc_node.h"
#include "app/screen_timeout.h"
#include "app/bootloader_hal.h"
#include "app/panel_storage.h"

// Reset-reason detection (bootloader check)
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "esp32s3/rom/rtc.h"
#elif defined(CONFIG_IDF_TARGET_ESP32)
#include "esp32/rom/rtc.h"
#endif

static const char *TAG = "main";

// Hardware handles (global — referenced by ui_common.c)
ch422g_handle_t        s_ch422g    = NULL;
esp_lcd_panel_handle_t s_lcd_panel = NULL;
esp_lcd_touch_handle_t s_touch     = NULL;
static waveshare_sd_handle_t s_sd_card   = NULL;
static bool                  s_sd_card_ok = false;

/**
 * @brief Initialize I2C master bus
 */
static esp_err_t init_i2c(void)
{
    ESP_LOGI(TAG, "Initializing I2C bus");

    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CONFIG_I2C_MASTER_SDA_IO,
        .scl_io_num = CONFIG_I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CONFIG_I2C_MASTER_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM_0, &i2c_conf), TAG, "I2C param config failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0), TAG, "I2C driver install failed");

    return ESP_OK;
}

/**
 * @brief Initialize all board hardware
 *
 * Order matters: I2C → CH422G → SD → LCD → Touch
 */
static esp_err_t init_hardware(void)
{
    esp_err_t ret;

    /* 1. I2C (needed by CH422G, Touch) */
    ret = init_i2c();
    if (ret != ESP_OK) return ret;

    /* 2. CH422G I/O expander (needed for SD CS, LCD backlight, touch reset) */
    ch422g_config_t ch422g_cfg = { .i2c_port = I2C_NUM_0, .timeout_ms = 1000 };
    ret = ch422g_init(&ch422g_cfg, &s_ch422g);
    if (ret != ESP_OK) return ret;

    /* 3. SD card (soft-fail — error screen shown later if missing) */
    waveshare_sd_config_t sd_cfg = {
        .mosi_gpio  = CONFIG_SD_MOSI_GPIO,
        .miso_gpio  = CONFIG_SD_MISO_GPIO,
        .clk_gpio   = CONFIG_SD_CLK_GPIO,
        .mount_point = CONFIG_SD_MOUNT_POINT,
        .ch422g_handle = s_ch422g,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    ret = waveshare_sd_init(&sd_cfg, &s_sd_card);
    s_sd_card_ok = (ret == ESP_OK);
    if (!s_sd_card_ok) {
        ESP_LOGW(TAG, "SD card init failed: %s", esp_err_to_name(ret));
    }

    /* 4. RGB LCD (double-buffered with DMA bounce buffer) */
    waveshare_lcd_config_t lcd_cfg = {
        .h_res = CONFIG_LCD_H_RES,
        .v_res = CONFIG_LCD_V_RES,
        .pixel_clock_hz = CONFIG_LCD_PIXEL_CLOCK_HZ,
        .num_fb = 2,
        .bounce_buffer_size_px = CONFIG_LCD_H_RES * CONFIG_LCD_RGB_BOUNCE_BUFFER_HEIGHT,
        .ch422g_handle = s_ch422g,
    };
    ret = waveshare_lcd_init(&lcd_cfg, &s_lcd_panel);
    if (ret != ESP_OK) return ret;

    /* 5. Capacitive touch */
    waveshare_touch_config_t touch_cfg = {
        .i2c_port = I2C_NUM_0,
        .h_res = CONFIG_LCD_H_RES,
        .v_res = CONFIG_LCD_V_RES,
        .ch422g_handle = s_ch422g,
    };
    ret = waveshare_touch_init(&touch_cfg, &s_touch);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Hardware init complete");
    return ESP_OK;
}

/* ========================================================================= */
/* Cross-module callbacks (LCC thread → LVGL async)                          */
/* ========================================================================= */

/**
 * @brief LVGL-safe callback: update both the switchboard tile and panel diagram
 */
static void ui_turnouts_update_tile_async(void *param)
{
    uint32_t packed = (uint32_t)(uintptr_t)param;
    int index            = (int)(packed >> 8);
    turnout_state_t state = (turnout_state_t)(packed & 0xFF);

    ui_turnouts_update_tile(index, state);
    ui_panel_update_turnout(index, state);
}

/**
 * @brief Turnout state callback — runs on LCC executor, schedules LVGL update
 */
static void turnout_state_changed_cb(int index, turnout_state_t new_state)
{
    uint32_t packed = ((uint32_t)index << 8) | (uint32_t)new_state;
    lv_async_call((lv_async_cb_t)ui_turnouts_update_tile_async,
                  (void *)(uintptr_t)packed);
}

/**
 * @brief LVGL-safe callback: forward a discovered event to the Add Turnout tab
 */
static void discovery_event_async(void *param)
{
    uint64_t event_id = *((uint64_t *)param);
    free(param);
    if (!lcc_node_is_discovery_mode()) return;  /* stale callback guard */
    ui_add_turnout_discovery_event(event_id, TURNOUT_STATE_UNKNOWN);
}

/**
 * @brief Discovery callback — runs on LCC executor, schedules LVGL update
 */
static void discovery_cb(uint64_t event_id, uint8_t state)
{
    uint64_t *ev = malloc(sizeof(*ev));
    if (ev) {
        *ev = event_id;
        lv_async_call((lv_async_cb_t)discovery_event_async, ev);
    }
}

/**
 * @brief Register all stored turnout events with the LCC node for consumption
 */
static void register_all_turnout_events(void)
{
    size_t count = turnout_manager_get_count();
    ESP_LOGI(TAG, "Registering %d turnout event pairs with LCC node", (int)count);
    
    for (size_t i = 0; i < count; i++) {
        turnout_t t;
        if (turnout_manager_get_by_index(i, &t) == ESP_OK) {
            lcc_node_register_turnout_events(t.event_normal, t.event_reverse);
        }
    }
}

/**
 * @brief Check if bootloader mode was requested and enter it if so (FR-060)
 *
 * Must run before any other init so the CAN bootloader starts fast.
 * Does NOT return if bootloader mode is active.
 */
static void check_and_run_bootloader(void)
{
    uint8_t reset_reason = rtc_get_reset_reason(0);
    ESP_LOGI(TAG, "Reset reason: %d", reset_reason);

    bootloader_hal_init(reset_reason);

    if (!bootloader_hal_should_enter()) return;

    ESP_LOGI(TAG, "Entering bootloader mode for firmware update...");

    /* Minimal HW init: I2C → CH422G → SD (to read node ID) */
    esp_err_t ret = init_i2c();
    if (ret == ESP_OK) {
        ch422g_config_t cfg = { .i2c_port = I2C_NUM_0, .timeout_ms = 1000 };
        ret = ch422g_init(&cfg, &s_ch422g);
    }

    uint64_t node_id = LCC_DEFAULT_NODE_ID;
    if (ret == ESP_OK) {
        waveshare_sd_config_t sd = {
            .mosi_gpio  = CONFIG_SD_MOSI_GPIO,
            .miso_gpio  = CONFIG_SD_MISO_GPIO,
            .clk_gpio   = CONFIG_SD_CLK_GPIO,
            .mount_point = CONFIG_SD_MOUNT_POINT,
            .ch422g_handle = s_ch422g,
            .max_files = 5,
            .format_if_mount_failed = false,
        };
        if (waveshare_sd_init(&sd, &s_sd_card) == ESP_OK) {
            uint64_t id = lcc_node_get_node_id();
            if (id) node_id = id;
        }
    }

    bootloader_hal_run(node_id, CONFIG_TWAI_RX_GPIO, CONFIG_TWAI_TX_GPIO);
    esp_restart();  /* should never reach here */
}

/**
 * @brief Application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "LCC Turnout Control Panel starting  (IDF %s, heap %lu)",
             esp_get_idf_version(), esp_get_free_heap_size());

    /* ---- Bootloader check (must be first) ---- */
    check_and_run_bootloader();

    /* ---- NVS ---- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ---- Hardware (I2C, CH422G, SD, LCD, Touch) ---- */
    ret = init_hardware();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Hardware init failed: %s — halting", esp_err_to_name(ret));
        while (1) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    if (!s_sd_card_ok) {
        ui_splash_show_sd_error();   /* never returns */
    }

    /* ---- Turnout manager (loads turnouts.json) ---- */
    ret = turnout_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Turnout manager init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Loaded %d turnouts", (int)turnout_manager_get_count());
    }

    /* ---- Panel layout (loads panel.json) ---- */
    panel_layout_t *layout = panel_layout_get();
    ret = panel_storage_load(layout);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Panel layout load failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Panel layout: %d items, %d tracks",
                 (int)layout->item_count, (int)layout->track_count);
    }

    /* ---- Wire up cross-module callbacks ---- */
    turnout_manager_set_state_callback(turnout_state_changed_cb);
    lcc_node_set_discovery_callback(discovery_cb);

    /* ---- Splash image (direct framebuffer, pre-LVGL) ---- */
    ui_splash_show_image(s_lcd_panel, "/sdcard/SPLASH.JPG");
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* ---- LCC / OpenMRN ---- */
    lcc_config_t lcc_cfg = LCC_CONFIG_DEFAULT();
    ret = lcc_node_init(&lcc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LCC init failed: %s — continuing without LCC",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "LCC node %012llX online",
                 (unsigned long long)lcc_node_get_node_id());
        register_all_turnout_events();
    }

    /* ---- Screen timeout (power saving) ---- */
    screen_timeout_config_t st_cfg = {
        .ch422g_handle = s_ch422g,
        .timeout_sec   = lcc_node_get_screen_timeout_sec(),
    };
    screen_timeout_init(&st_cfg);

    /* ---- LVGL + UI ---- */
    lv_disp_t  *disp = NULL;
    lv_indev_t *touch_indev = NULL;
    ret = ui_init(&disp, &touch_indev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL init failed: %s — halting", esp_err_to_name(ret));
        while (1) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    ui_show_main();

    if (lcc_node_get_status() == LCC_STATUS_RUNNING) {
        lcc_node_query_all_turnout_states();
    }

    ESP_LOGI(TAG, "Init complete — entering main loop");

    /* ---- Main loop ---- */
    TickType_t last_status  = xTaskGetTickCount();
    TickType_t last_refresh = xTaskGetTickCount();

    while (1) {
        screen_timeout_tick();
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Periodic state re-query (stale timeout interval) */
        uint16_t refresh_sec = lcc_node_get_stale_timeout_sec();
        if (refresh_sec > 0 &&
            (xTaskGetTickCount() - last_refresh) >=
                pdMS_TO_TICKS((uint32_t)refresh_sec * 1000)) {
            last_refresh = xTaskGetTickCount();
            lcc_node_query_all_turnout_states();
        }

        /* Heartbeat status log every 30 s */
        if ((xTaskGetTickCount() - last_status) >= pdMS_TO_TICKS(30000)) {
            last_status = xTaskGetTickCount();
            ESP_LOGI(TAG, "heap=%lu LCC=%s screen=%s turnouts=%d",
                     esp_get_free_heap_size(),
                     lcc_node_get_status() == LCC_STATUS_RUNNING ? "ok" : "off",
                     screen_timeout_is_screen_on() ? "on" : "off",
                     (int)turnout_manager_get_count());
        }
    }
}
