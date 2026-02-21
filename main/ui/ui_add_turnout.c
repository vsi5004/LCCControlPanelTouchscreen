/**
 * @file ui_add_turnout.c
 * @brief Add Turnout Tab — manual entry form and event discovery list
 *
 * Provides two ways to add turnouts:
 *   1. Manual entry: user types name & event IDs in dotted-hex
 *   2. Discovery: listens for events on the bus so user can pick them
 */

#include "ui_common.h"
#include "app/turnout_manager.h"
#include "app/turnout_storage.h"
#include "app/lcc_node.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ui_add_turnout";

// ============================================================================
// Internal state
// ============================================================================
static lv_obj_t *s_parent = NULL;

// Manual entry widgets
static lv_obj_t *s_name_ta = NULL;
static lv_obj_t *s_normal_ta = NULL;
static lv_obj_t *s_reverse_ta = NULL;
static lv_obj_t *s_add_btn = NULL;
static lv_obj_t *s_status_label = NULL;

// Discovery widgets
static lv_obj_t *s_discover_btn = NULL;
static lv_obj_t *s_discover_label = NULL;
static lv_obj_t *s_discover_list = NULL;

// Keyboard for text input
static lv_obj_t *s_keyboard = NULL;
static lv_obj_t *s_active_ta = NULL;

// ============================================================================
// Helpers
// ============================================================================

/**
 * @brief Parse a dotted-hex event ID string (e.g. "05.01.01.01.22.60.00.00")
 *        into a uint64_t.
 */
static bool parse_event_id_str(const char *str, uint64_t *out)
{
    if (!str || !out) return false;

    unsigned int b[8];
    if (sscanf(str, "%02x.%02x.%02x.%02x.%02x.%02x.%02x.%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) == 8) {
        *out = ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
               ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
               ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
               ((uint64_t)b[6] << 8)  | ((uint64_t)b[7]);
        return true;
    }
    return false;
}

static void format_event_id_str(uint64_t id, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "%02X.%02X.%02X.%02X.%02X.%02X.%02X.%02X",
             (unsigned)((id >> 56) & 0xFF), (unsigned)((id >> 48) & 0xFF),
             (unsigned)((id >> 40) & 0xFF), (unsigned)((id >> 32) & 0xFF),
             (unsigned)((id >> 24) & 0xFF), (unsigned)((id >> 16) & 0xFF),
             (unsigned)((id >> 8) & 0xFF),  (unsigned)(id & 0xFF));
}

static void show_status(const char *msg, lv_color_t color)
{
    if (s_status_label) {
        lv_label_set_text(s_status_label, msg);
        lv_obj_set_style_text_color(s_status_label, color, LV_PART_MAIN);
    }
}

// ============================================================================
// Keyboard management
// ============================================================================

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        s_active_ta = NULL;
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (s_keyboard) {
        lv_keyboard_set_textarea(s_keyboard, ta);
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        s_active_ta = ta;
    }
}

// ============================================================================
// Add button callback
// ============================================================================

static void add_btn_cb(lv_event_t *e)
{
    const char *name = lv_textarea_get_text(s_name_ta);
    const char *normal_str = lv_textarea_get_text(s_normal_ta);
    const char *reverse_str = lv_textarea_get_text(s_reverse_ta);

    // Validate name
    if (!name || strlen(name) == 0) {
        show_status("Please enter a turnout name", lv_color_hex(0xF44336));
        return;
    }

    // Parse event IDs
    uint64_t ev_normal, ev_reverse;
    if (!parse_event_id_str(normal_str, &ev_normal)) {
        show_status("Invalid NORMAL event ID format\n(use XX.XX.XX.XX.XX.XX.XX.XX)", lv_color_hex(0xF44336));
        return;
    }
    if (!parse_event_id_str(reverse_str, &ev_reverse)) {
        show_status("Invalid REVERSE event ID format\n(use XX.XX.XX.XX.XX.XX.XX.XX)", lv_color_hex(0xF44336));
        return;
    }

    // Add turnout to manager
    int new_idx = turnout_manager_add(ev_normal, ev_reverse, name);
    if (new_idx < 0) {
        show_status("Failed to add turnout (duplicate or full?)", lv_color_hex(0xF44336));
        return;
    }

    // Register events with LCC node
    lcc_node_register_turnout_events(ev_normal, ev_reverse);

    // Save to SD card
    turnout_manager_save();

    // Refresh turnout grid
    ui_lock();
    ui_turnouts_refresh();
    ui_unlock();

    // Clear form
    lv_textarea_set_text(s_name_ta, "");
    lv_textarea_set_text(s_normal_ta, "");
    lv_textarea_set_text(s_reverse_ta, "");

    char msg[64];
    snprintf(msg, sizeof(msg), "Added '%s' successfully!", name);
    show_status(msg, lv_color_hex(0x4CAF50));
}

// ============================================================================
// Discovery mode
// ============================================================================

static void discover_btn_cb(lv_event_t *e)
{
    bool is_active = lcc_node_is_discovery_mode();
    
    if (is_active) {
        // Stop discovery
        lcc_node_set_discovery_mode(false);
        lv_label_set_text(s_discover_label, LV_SYMBOL_EYE_OPEN " Start Discovery");
        lv_obj_set_style_bg_color(s_discover_btn, lv_color_hex(0x2196F3), LV_PART_MAIN);
    } else {
        // Start discovery
        lcc_node_set_discovery_mode(true);
        lv_label_set_text(s_discover_label, LV_SYMBOL_CLOSE " Stop Discovery");
        lv_obj_set_style_bg_color(s_discover_btn, lv_color_hex(0xF44336), LV_PART_MAIN);
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_create_add_turnout_tab(lv_obj_t *parent)
{
    s_parent = parent;

    lv_obj_set_style_pad_all(parent, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0xF5F5F5), LV_PART_MAIN);
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 6, LV_PART_MAIN);

    // ---- Section: Manual Entry ----
    lv_obj_t *manual_header = lv_label_create(parent);
    lv_label_set_text(manual_header, "Add Turnout Manually");
    lv_obj_set_style_text_font(manual_header, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(manual_header, lv_color_hex(0x212121), LV_PART_MAIN);

    // Row container for form fields (horizontal layout)
    lv_obj_t *form_row = lv_obj_create(parent);
    lv_obj_set_size(form_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(form_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(form_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(form_row, 0, LV_PART_MAIN);
    lv_obj_set_layout(form_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(form_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(form_row, 8, LV_PART_MAIN);
    lv_obj_set_flex_align(form_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    // Name field
    lv_obj_t *name_col = lv_obj_create(form_row);
    lv_obj_set_size(name_col, 140, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(name_col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(name_col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(name_col, 0, LV_PART_MAIN);
    lv_obj_set_layout(name_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(name_col, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *name_lbl = lv_label_create(name_col);
    lv_label_set_text(name_lbl, "Name:");
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, LV_PART_MAIN);

    s_name_ta = lv_textarea_create(name_col);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_max_length(s_name_ta, 31);
    lv_textarea_set_placeholder_text(s_name_ta, "e.g. Turnout 1");
    lv_obj_set_width(s_name_ta, 130);
    lv_obj_add_event_cb(s_name_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    // Normal event field
    lv_obj_t *normal_col = lv_obj_create(form_row);
    lv_obj_set_size(normal_col, 220, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(normal_col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(normal_col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(normal_col, 0, LV_PART_MAIN);
    lv_obj_set_layout(normal_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(normal_col, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *normal_lbl = lv_label_create(normal_col);
    lv_label_set_text(normal_lbl, "Normal Event ID:");
    lv_obj_set_style_text_font(normal_lbl, &lv_font_montserrat_12, LV_PART_MAIN);

    s_normal_ta = lv_textarea_create(normal_col);
    lv_textarea_set_one_line(s_normal_ta, true);
    lv_textarea_set_max_length(s_normal_ta, 23);
    lv_textarea_set_placeholder_text(s_normal_ta, "XX.XX.XX.XX.XX.XX.XX.XX");
    lv_obj_set_width(s_normal_ta, 210);
    lv_obj_add_event_cb(s_normal_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    // Reverse event field
    lv_obj_t *reverse_col = lv_obj_create(form_row);
    lv_obj_set_size(reverse_col, 220, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(reverse_col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(reverse_col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(reverse_col, 0, LV_PART_MAIN);
    lv_obj_set_layout(reverse_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(reverse_col, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *reverse_lbl = lv_label_create(reverse_col);
    lv_label_set_text(reverse_lbl, "Reverse Event ID:");
    lv_obj_set_style_text_font(reverse_lbl, &lv_font_montserrat_12, LV_PART_MAIN);

    s_reverse_ta = lv_textarea_create(reverse_col);
    lv_textarea_set_one_line(s_reverse_ta, true);
    lv_textarea_set_max_length(s_reverse_ta, 23);
    lv_textarea_set_placeholder_text(s_reverse_ta, "XX.XX.XX.XX.XX.XX.XX.XX");
    lv_obj_set_width(s_reverse_ta, 210);
    lv_obj_add_event_cb(s_reverse_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    // Add button
    s_add_btn = lv_btn_create(form_row);
    lv_obj_set_size(s_add_btn, 100, 45);
    lv_obj_set_style_bg_color(s_add_btn, lv_color_hex(0x4CAF50), LV_PART_MAIN);
    lv_obj_set_style_radius(s_add_btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(s_add_btn, add_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *add_lbl = lv_label_create(s_add_btn);
    lv_label_set_text(add_lbl, LV_SYMBOL_PLUS " Add");
    lv_obj_set_style_text_font(add_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(add_lbl);

    // Status label
    s_status_label = lv_label_create(parent);
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, LV_PART_MAIN);

    // ---- Separator ----
    lv_obj_t *sep = lv_obj_create(parent);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0xBDBDBD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);

    // ---- Section: Discovery ----
    lv_obj_t *disc_header_row = lv_obj_create(parent);
    lv_obj_set_size(disc_header_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(disc_header_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(disc_header_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(disc_header_row, 0, LV_PART_MAIN);
    lv_obj_set_layout(disc_header_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(disc_header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(disc_header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *disc_title = lv_label_create(disc_header_row);
    lv_label_set_text(disc_title, "Event Discovery");
    lv_obj_set_style_text_font(disc_title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(disc_title, lv_color_hex(0x212121), LV_PART_MAIN);

    s_discover_btn = lv_btn_create(disc_header_row);
    lv_obj_set_size(s_discover_btn, 180, 36);
    lv_obj_set_style_bg_color(s_discover_btn, lv_color_hex(0x2196F3), LV_PART_MAIN);
    lv_obj_set_style_radius(s_discover_btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(s_discover_btn, discover_btn_cb, LV_EVENT_CLICKED, NULL);
    s_discover_label = lv_label_create(s_discover_btn);
    lv_label_set_text(s_discover_label, LV_SYMBOL_EYE_OPEN " Start Discovery");
    lv_obj_set_style_text_font(s_discover_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_center(s_discover_label);

    // Discovery list
    s_discover_list = lv_list_create(parent);
    lv_obj_set_size(s_discover_list, lv_pct(100), 120);
    lv_obj_set_style_bg_color(s_discover_list, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_discover_list, lv_color_hex(0xBDBDBD), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_discover_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_discover_list, 4, LV_PART_MAIN);
    lv_obj_set_flex_grow(s_discover_list, 1);

    lv_obj_t *empty_disc = lv_label_create(s_discover_list);
    lv_label_set_text(empty_disc, "Start discovery to see events on the LCC bus...");
    lv_obj_set_style_text_color(empty_disc, lv_color_hex(0x9E9E9E), LV_PART_MAIN);
    lv_obj_set_style_text_font(empty_disc, &lv_font_montserrat_12, LV_PART_MAIN);

    // ---- Keyboard (hidden by default) ----
    s_keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(s_keyboard, lv_pct(100), 180);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
}

void ui_add_turnout_discovery_event(uint64_t event_id, turnout_state_t state)
{
    if (!s_discover_list) return;

    // Format event ID
    char id_str[32];
    format_event_id_str(event_id, id_str, sizeof(id_str));

    // Check if already in the list (avoid duplicates)
    uint32_t child_count = lv_obj_get_child_cnt(s_discover_list);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(s_discover_list, i);
        // Skip non-button children (like the empty label)
        if (!lv_obj_check_type(child, &lv_list_btn_class)) continue;
        lv_obj_t *lbl = lv_obj_get_child(child, 0);
        if (lbl) {
            const char *text = lv_label_get_text(lbl);
            if (text && strstr(text, id_str)) return; // Already listed
        }
    }

    // Remove the "Start discovery..." placeholder text if present
    if (child_count > 0) {
        lv_obj_t *first = lv_obj_get_child(s_discover_list, 0);
        if (first && !lv_obj_check_type(first, &lv_list_btn_class)) {
            lv_obj_del(first);
        }
    }

    // Add to list
    char label_text[48];
    snprintf(label_text, sizeof(label_text), "%s", id_str);
    lv_obj_t *btn = lv_list_add_btn(s_discover_list, LV_SYMBOL_RIGHT, label_text);
    lv_obj_set_style_text_font(btn, &lv_font_montserrat_12, LV_PART_MAIN);

    ESP_LOGI(TAG, "Discovered event: %s", id_str);
}

void ui_add_turnout_clear_discoveries(void)
{
    if (!s_discover_list) return;
    lv_obj_clean(s_discover_list);

    lv_obj_t *empty_disc = lv_label_create(s_discover_list);
    lv_label_set_text(empty_disc, "Start discovery to see events on the LCC bus...");
    lv_obj_set_style_text_color(empty_disc, lv_color_hex(0x9E9E9E), LV_PART_MAIN);
    lv_obj_set_style_text_font(empty_disc, &lv_font_montserrat_12, LV_PART_MAIN);
}
