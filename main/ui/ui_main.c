/**
 * @file ui_main.c
 * @brief Main UI Navigation — Panel Screen (default) and Settings Screen
 *
 * The default screen is the Control Panel (layout diagram). A settings gear
 * icon navigates to a tabview with: Turnouts, Add Turnout, Panel Builder.
 * A back button on the settings screen returns to the panel.
 */

#include "ui_common.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_main";

// UI Objects — Settings screen
static lv_obj_t *s_tabview = NULL;
static lv_obj_t *s_tab_turnouts = NULL;
static lv_obj_t *s_tab_add = NULL;
static lv_obj_t *s_tab_builder = NULL;

// Forward declaration
static void back_btn_cb(lv_event_t *e);

// ============================================================================
// Settings Screen (3-tab tabview)
// ============================================================================

static void ui_create_settings_screen(void)
{
    ESP_LOGI(TAG, "Creating settings screen with tabview");

    ui_lock();

    // Invalidate panel screen — panel objects are about to be destroyed
    ui_panel_invalidate();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

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

    // Add tabs
    s_tab_turnouts = lv_tabview_add_tab(s_tabview, "Turnouts");
    s_tab_add = lv_tabview_add_tab(s_tabview, "Add Turnout");
    s_tab_builder = lv_tabview_add_tab(s_tabview, "Panel Builder");

    lv_obj_set_style_bg_color(s_tab_turnouts, lv_color_make(245, 245, 245), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_tab_add, lv_color_make(245, 245, 245), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_tab_builder, lv_color_make(245, 245, 245), LV_PART_MAIN);

    // Disable swipe gesture between tabs — horizontal swipe conflicts with
    // drag-and-drop in the Panel Builder canvas. Users switch tabs by tapping.
    lv_obj_t *tv_content = lv_tabview_get_content(s_tabview);
    if (tv_content) {
        lv_obj_clear_flag(tv_content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(tv_content, 0, LV_PART_MAIN);
    }

    // Create tab content
    ui_create_turnouts_tab(s_tab_turnouts);
    ui_create_add_turnout_tab(s_tab_add);
    ui_create_panel_builder_tab(s_tab_builder);

    // Back button — overlaid on top-left of screen, over the tab bar
    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 50, 44);
    lv_obj_set_pos(back_btn, 4, 3);
    lv_obj_set_style_bg_color(back_btn, lv_color_make(33, 150, 243), LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(back_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(back_label);

    ESP_LOGI(TAG, "Settings screen created");
    ui_unlock();
}

static void back_nav_async(void *param)
{
    (void)param;
    ui_show_main();
}

static void back_btn_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    ESP_LOGI(TAG, "Back button pressed — returning to panel");
    // Defer navigation: can't destroy current screen from inside its event handler
    lv_async_call(back_nav_async, NULL);
}

// ============================================================================
// Public API — Getters
// ============================================================================

lv_obj_t* ui_get_turnouts_tab(void)
{
    return s_tab_turnouts;
}

lv_obj_t* ui_get_add_turnout_tab(void)
{
    return s_tab_add;
}

lv_obj_t* ui_get_panel_builder_tab(void)
{
    return s_tab_builder;
}

void ui_show_main(void)
{
    ESP_LOGI(TAG, "Showing control panel (main screen)");
    ui_create_panel_screen();
}

void ui_show_settings(void)
{
    ESP_LOGI(TAG, "Showing settings screen");
    ui_create_settings_screen();
}

void ui_show_settings_at_tab(uint32_t tab_idx)
{
    ESP_LOGI(TAG, "Showing settings screen at tab %lu", (unsigned long)tab_idx);
    ui_create_settings_screen();
    if (s_tabview) {
        lv_tabview_set_act(s_tabview, tab_idx, LV_ANIM_OFF);
    }
}