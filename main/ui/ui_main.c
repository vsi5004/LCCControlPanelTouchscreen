/**
 * @file ui_main.c
 * @brief Main UI Screen with Tabview (Turnouts and Add Turnout)
 */

#include "ui_common.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_main";

// UI Objects
static lv_obj_t *s_tabview = NULL;
static lv_obj_t *s_tab_turnouts = NULL;
static lv_obj_t *s_tab_add = NULL;

void ui_create_main_screen(void)
{
    ESP_LOGI(TAG, "Creating main screen with tabview");

    ui_lock();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    // Create tabview
    s_tabview = lv_tabview_create(scr, LV_DIR_TOP, 50);
    lv_obj_set_style_bg_color(s_tabview, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_tabview, lv_color_hex(0x000000), LV_PART_MAIN);

    // Style tab buttons
    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(s_tabview);
    lv_obj_set_style_text_font(tab_btns, &lv_font_montserrat_24, LV_PART_MAIN);

    // Unselected tabs
    lv_obj_set_style_bg_color(tab_btns, lv_color_make(158, 158, 158), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tab_btns, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(tab_btns, lv_color_make(220, 220, 220), LV_PART_MAIN);
    lv_obj_set_style_text_color(tab_btns, lv_color_make(220, 220, 220), LV_PART_ITEMS | LV_STATE_DEFAULT);

    // Selected tab
    lv_obj_set_style_bg_color(tab_btns, lv_color_make(33, 150, 243), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(tab_btns, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tab_btns, lv_color_make(255, 255, 255), LV_PART_ITEMS | LV_STATE_CHECKED);

    // Add tabs - Turnouts first
    s_tab_turnouts = lv_tabview_add_tab(s_tabview, "Turnouts");
    s_tab_add = lv_tabview_add_tab(s_tabview, "Add Turnout");

    lv_obj_set_style_bg_color(s_tab_turnouts, lv_color_make(245, 245, 245), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_tab_add, lv_color_make(245, 245, 245), LV_PART_MAIN);

    // Create tab content
    ui_create_turnouts_tab(s_tab_turnouts);
    ui_create_add_turnout_tab(s_tab_add);

    ESP_LOGI(TAG, "Main screen created");
    ui_unlock();
}

lv_obj_t* ui_get_turnouts_tab(void)
{
    return s_tab_turnouts;
}

lv_obj_t* ui_get_add_turnout_tab(void)
{
    return s_tab_add;
}

void ui_show_main(void)
{
    ESP_LOGI(TAG, "Showing main screen");
    ui_create_main_screen();
}
