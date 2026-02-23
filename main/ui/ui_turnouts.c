/**
 * @file ui_turnouts.c
 * @brief Turnout Switchboard Tab - grid of color-coded turnout tiles
 *
 * Displays all configured turnouts in a scrollable grid. Each tile shows:
 *   - Turnout name
 *   - Current state (NORMAL / REVERSE / UNKNOWN / STALE)
 *   - Color coding: Green=NORMAL, Yellow=REVERSE, Grey=UNKNOWN, Red=STALE
 *
 * Tapping a tile sends a TOGGLE command (sends the opposite event).
 * A pulsing border indicates a command is pending confirmation.
 */

#include "ui_common.h"
#include "app/turnout_manager.h"
#include "app/panel_layout.h"
#include "app/panel_storage.h"
#include "app/lcc_node.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_turnouts";

// ============================================================================
// Layout constants
// ============================================================================
#define TILE_WIDTH      150
#define TILE_HEIGHT     110
#define TILE_PAD        8
#define TILE_RADIUS     8
#define COLS_PER_ROW    5
#define ICON_BTN_SIZE   28

// Colors (RGB565-safe hex values)
#define COLOR_NORMAL    0x4CAF50   // Green
#define COLOR_REVERSE   0xFFC107   // Amber/Yellow
#define COLOR_UNKNOWN   0x9E9E9E   // Grey
#define COLOR_STALE     0xF44336   // Red
#define COLOR_PENDING   0x2196F3   // Blue border for pending
#define COLOR_BG        0xF5F5F5   // Light grey background
#define COLOR_TEXT_DARK 0x212121   // Dark text
#define COLOR_TEXT_LIGHT 0xFFFFFF  // White text

// ============================================================================
// Internal state
// ============================================================================
static lv_obj_t *s_parent = NULL;
static lv_obj_t *s_grid_container = NULL;
static lv_obj_t *s_empty_label = NULL;

// Array of tile objects - indexed same as turnout_manager
static lv_obj_t *s_tiles[TURNOUT_MAX_COUNT];
static lv_obj_t *s_tile_names[TURNOUT_MAX_COUNT];
static lv_obj_t *s_tile_states[TURNOUT_MAX_COUNT];
static int s_tile_count = 0;

void ui_turnouts_invalidate(void)
{
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_tile_names, 0, sizeof(s_tile_names));
    memset(s_tile_states, 0, sizeof(s_tile_states));
    s_tile_count = 0;
    s_grid_container = NULL;
    s_empty_label = NULL;
}

// Edit / delete modal state
static int s_edit_index = -1;
static int s_delete_index = -1;
static lv_obj_t *s_rename_overlay = NULL;
static lv_obj_t *s_rename_ta = NULL;
static lv_obj_t *s_rename_kb = NULL;
static lv_obj_t *s_delete_modal = NULL;

// ============================================================================
// Helpers
// ============================================================================

static lv_color_t state_to_bg_color(turnout_state_t state)
{
    switch (state) {
        case TURNOUT_STATE_NORMAL:  return lv_color_hex(COLOR_NORMAL);
        case TURNOUT_STATE_REVERSE: return lv_color_hex(COLOR_REVERSE);
        case TURNOUT_STATE_STALE:   return lv_color_hex(COLOR_STALE);
        default:                    return lv_color_hex(COLOR_UNKNOWN);
    }
}

static const char* state_to_text(turnout_state_t state)
{
    switch (state) {
        case TURNOUT_STATE_NORMAL:  return "CLOSED";
        case TURNOUT_STATE_REVERSE: return "THROWN";
        case TURNOUT_STATE_STALE:   return "STALE";
        default:                    return "UNKNOWN";
    }
}

static lv_color_t state_to_text_color(turnout_state_t state)
{
    switch (state) {
        case TURNOUT_STATE_NORMAL:
        case TURNOUT_STATE_STALE:
            return lv_color_hex(COLOR_TEXT_LIGHT);
        case TURNOUT_STATE_REVERSE:
            return lv_color_hex(COLOR_TEXT_DARK);
        default:
            return lv_color_hex(COLOR_TEXT_LIGHT);
    }
}

// ============================================================================
// Event callback - tile tap -> toggle turnout
// ============================================================================

static void tile_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_tile_count) return;

    turnout_t t;
    if (turnout_manager_get_by_index(idx, &t) != ESP_OK) return;

    // Toggle: send the opposite event
    uint64_t event_to_send;
    if (t.state == TURNOUT_STATE_REVERSE) {
        event_to_send = t.event_normal;
    } else {
        // NORMAL, UNKNOWN, or STALE -> send REVERSE
        event_to_send = t.event_reverse;
    }

    ESP_LOGI(TAG, "Toggle turnout '%s' -> sending %016llx", t.name,
             (unsigned long long)event_to_send);

    // Mark command pending
    turnout_manager_set_pending(idx, true);

    // Send LCC event
    lcc_node_send_event(event_to_send);

    // Update tile to show pending state (blue border)
    if (s_tiles[idx]) {
        lv_obj_set_style_border_color(s_tiles[idx], lv_color_hex(COLOR_PENDING), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_tiles[idx], 3, LV_PART_MAIN);
    }
}

// ============================================================================
// Rename modal
// ============================================================================

static void rename_close(void)
{
    if (s_rename_overlay) {
        lv_obj_t *overlay = s_rename_overlay;
        s_rename_overlay = NULL;
        s_rename_ta = NULL;
        s_rename_kb = NULL;
        s_edit_index = -1;
        lv_obj_del(overlay);
    }
}

static void rename_save_cb(lv_event_t *e)
{
    (void)e;
    if (s_edit_index < 0 || !s_rename_ta) return;

    const char *txt = lv_textarea_get_text(s_rename_ta);
    if (!txt || txt[0] == '\0') {
        rename_close();
        return;
    }

    char name_buf[32];
    strncpy(name_buf, txt, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';

    int idx = s_edit_index;
    rename_close();

    ESP_LOGI(TAG, "Renaming turnout %d to '%s'", idx, name_buf);
    turnout_manager_rename((size_t)idx, name_buf);
    turnout_manager_save();

    if (idx < s_tile_count && s_tile_names[idx]) {
        lv_label_set_text(s_tile_names[idx], name_buf);
    }
}

static void flip_polarity_cb(lv_event_t *e)
{
    (void)e;
    if (s_edit_index < 0) return;

    int idx = s_edit_index;

    // Get old event_normal before flip (for panel layout update)
    turnout_t t_before;
    if (turnout_manager_get_by_index(idx, &t_before) != ESP_OK) return;
    uint64_t old_event_normal = t_before.event_normal;

    // Flip events in the manager
    turnout_manager_flip_polarity((size_t)idx);

    // Update panel layout item's event_normal key if this turnout is placed
    panel_layout_t *layout = panel_layout_get();
    int pi = panel_layout_find_item(layout, old_event_normal);
    if (pi >= 0) {
        turnout_t t_after;
        if (turnout_manager_get_by_index(idx, &t_after) == ESP_OK) {
            layout->items[pi].event_normal = t_after.event_normal;
        }
        panel_storage_save(layout);
    }

    // Re-register LCC events
    lcc_node_unregister_all_turnout_events();
    size_t count = turnout_manager_get_count();
    for (size_t i = 0; i < count; i++) {
        turnout_t rem;
        if (turnout_manager_get_by_index(i, &rem) == ESP_OK) {
            lcc_node_register_turnout_events(rem.event_normal, rem.event_reverse);
        }
    }

    turnout_manager_save();

    ESP_LOGI(TAG, "Flipped polarity for turnout %d", idx);

    rename_close();
    ui_turnouts_refresh();
}

static void rename_cancel_cb(lv_event_t *e)
{
    (void)e;
    rename_close();
}

static void rename_open(int index)
{
    turnout_t t;
    if (turnout_manager_get_by_index(index, &t) != ESP_OK) return;
    s_edit_index = index;

    // Full-screen dark overlay
    s_rename_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_rename_overlay, 800, 480);
    lv_obj_set_pos(s_rename_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_rename_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_rename_overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_rename_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_rename_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_rename_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // White panel
    lv_obj_t *panel = lv_obj_create(s_rename_overlay);
    lv_obj_set_size(panel, 420, 200);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 15);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Edit Turnout");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_DARK), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    // Name label
    lv_obj_t *name_lbl = lv_label_create(panel);
    lv_label_set_text(name_lbl, "Name:");
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0x757575), LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 8, 26);

    // Text area pre-filled with current name
    s_rename_ta = lv_textarea_create(panel);
    lv_obj_set_size(s_rename_ta, 380, 40);
    lv_textarea_set_max_length(s_rename_ta, 31);
    lv_textarea_set_one_line(s_rename_ta, true);
    lv_textarea_set_text(s_rename_ta, t.name);
    lv_obj_set_style_text_font(s_rename_ta, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_rename_ta, LV_ALIGN_TOP_MID, 0, 42);

    // Flip Polarity button
    lv_obj_t *flip_btn = lv_btn_create(panel);
    lv_obj_set_size(flip_btn, 380, 36);
    lv_obj_set_style_bg_color(flip_btn, lv_color_hex(0xFF9800), LV_PART_MAIN);
    lv_obj_set_style_radius(flip_btn, 6, LV_PART_MAIN);
    lv_obj_align(flip_btn, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_t *flip_lbl = lv_label_create(flip_btn);
    lv_label_set_text(flip_lbl, LV_SYMBOL_REFRESH " Flip Polarity (Swap Normal / Reverse)");
    lv_obj_set_style_text_font(flip_lbl, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_center(flip_lbl);
    lv_obj_add_event_cb(flip_btn, flip_polarity_cb, LV_EVENT_CLICKED, NULL);

    // Save button
    lv_obj_t *save_btn = lv_btn_create(panel);
    lv_obj_set_size(save_btn, 110, 36);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(COLOR_NORMAL), LV_PART_MAIN);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_LEFT, 30, 0);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(save_lbl);
    lv_obj_add_event_cb(save_btn, rename_save_cb, LV_EVENT_CLICKED, NULL);

    // Cancel button
    lv_obj_t *cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, 110, 36);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(COLOR_UNKNOWN), LV_PART_MAIN);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -30, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, rename_cancel_cb, LV_EVENT_CLICKED, NULL);

    // On-screen keyboard
    s_rename_kb = lv_keyboard_create(s_rename_overlay);
    lv_keyboard_set_textarea(s_rename_kb, s_rename_ta);
    lv_obj_align(s_rename_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// ============================================================================
// Delete confirmation modal
// ============================================================================

static void delete_close(void)
{
    if (s_delete_modal) {
        lv_obj_t *modal = s_delete_modal;
        s_delete_modal = NULL;
        s_delete_index = -1;
        lv_obj_del(modal);
    }
}

static void delete_cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    delete_close();
}

static void delete_confirm_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_delete_index < 0) {
        delete_close();
        return;
    }

    turnout_t t;
    if (turnout_manager_get_by_index(s_delete_index, &t) == ESP_OK) {
        ESP_LOGI(TAG, "Deleting turnout '%s' at index %d",
                 t.name, s_delete_index);

        // Remove matching panel item (+ cascade-delete connected tracks)
        panel_layout_t *layout = panel_layout_get();
        int pi = panel_layout_find_item(layout, t.event_normal);
        if (pi >= 0) {
            ESP_LOGI(TAG, "Removing panel item for deleted turnout (event %llx)",
                     (unsigned long long)t.event_normal);
            panel_layout_remove_item(layout, (size_t)pi);
            panel_storage_save(layout);
        }
    }

    turnout_manager_remove((size_t)s_delete_index);

    // Re-sync LCC event registrations with remaining turnouts
    lcc_node_unregister_all_turnout_events();
    size_t count = turnout_manager_get_count();
    for (size_t i = 0; i < count; i++) {
        turnout_t rem;
        if (turnout_manager_get_by_index(i, &rem) == ESP_OK) {
            lcc_node_register_turnout_events(rem.event_normal,
                                             rem.event_reverse);
        }
    }

    turnout_manager_save();
    delete_close();
    ui_turnouts_refresh();
}

static void show_delete_modal(int index)
{
    turnout_t t;
    if (turnout_manager_get_by_index(index, &t) != ESP_OK) return;
    s_delete_index = index;

    // Full-screen semi-transparent overlay
    s_delete_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_delete_modal, 800, 480);
    lv_obj_center(s_delete_modal);
    lv_obj_set_style_bg_color(s_delete_modal, lv_color_make(0, 0, 0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_delete_modal, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_delete_modal, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_delete_modal, 0, LV_PART_MAIN);

    // Dialog box
    lv_obj_t *dialog = lv_obj_create(s_delete_modal);
    lv_obj_set_size(dialog, 450, 250);
    lv_obj_center(dialog);
    lv_obj_set_style_bg_color(dialog, lv_color_make(255, 255, 255), LV_PART_MAIN);
    lv_obj_set_style_radius(dialog, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(dialog, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(dialog, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dialog, 20, LV_PART_MAIN);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    // Warning icon and title
    lv_obj_t *title = lv_label_create(dialog);
    lv_label_set_text(title, LV_SYMBOL_WARNING " Delete Turnout?");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(244, 67, 54), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    // Turnout name
    lv_obj_t *name_label = lv_label_create(dialog);
    char buf[48];
    snprintf(buf, sizeof(buf), "\"%s\"", t.name);
    lv_label_set_text(name_label, buf);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_label, lv_color_make(33, 33, 33), LV_PART_MAIN);
    lv_obj_align(name_label, LV_ALIGN_TOP_MID, 0, 50);

    // Warning message
    lv_obj_t *msg_label = lv_label_create(dialog);
    lv_label_set_text(msg_label, "This action cannot be undone.");
    lv_obj_set_style_text_font(msg_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(msg_label, lv_color_make(117, 117, 117), LV_PART_MAIN);
    lv_obj_align(msg_label, LV_ALIGN_TOP_MID, 0, 85);

    // Button container
    lv_obj_t *btn_container = lv_obj_create(dialog);
    lv_obj_set_size(btn_container, 400, 70);
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_container, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Cancel button
    lv_obj_t *btn_cancel = lv_btn_create(btn_container);
    lv_obj_set_size(btn_cancel, 160, 55);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_make(158, 158, 158), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_cancel, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_cancel, delete_cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_label = lv_label_create(btn_cancel);
    lv_label_set_text(cancel_label, LV_SYMBOL_CLOSE " Cancel");
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_label, lv_color_make(255, 255, 255), LV_PART_MAIN);
    lv_obj_center(cancel_label);

    // Delete button
    lv_obj_t *btn_delete = lv_btn_create(btn_container);
    lv_obj_set_size(btn_delete, 160, 55);
    lv_obj_set_style_bg_color(btn_delete, lv_color_make(244, 67, 54), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_delete, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_delete, delete_confirm_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *delete_label = lv_label_create(btn_delete);
    lv_label_set_text(delete_label, LV_SYMBOL_TRASH " Delete");
    lv_obj_set_style_text_font(delete_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(delete_label, lv_color_make(255, 255, 255), LV_PART_MAIN);
    lv_obj_center(delete_label);
}

// ============================================================================
// Icon button callbacks
// ============================================================================

static void edit_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    rename_open(idx);
}

static void trash_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    show_delete_modal(idx);
}

// ============================================================================
// Tile creation and update
// ============================================================================

static lv_obj_t* create_tile(lv_obj_t *parent, int index, const turnout_t *t)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, TILE_WIDTH, TILE_HEIGHT);
    lv_obj_set_style_radius(tile, TILE_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tile, state_to_bg_color(t->state), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 6, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_shadow_width(tile, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(tile, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(tile, LV_OPA_30, LV_PART_MAIN);

    // Pending indicator (blue border)
    if (t->command_pending) {
        lv_obj_set_style_border_color(tile, lv_color_hex(COLOR_PENDING), LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 3, LV_PART_MAIN);
    }

    // --- Row 1: Turnout name (top, full width) ---
    lv_obj_t *name_label = lv_label_create(tile);
    lv_label_set_text(name_label, t->name);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_label, state_to_text_color(t->state), LV_PART_MAIN);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, TILE_WIDTH - 16);
    lv_obj_align(name_label, LV_ALIGN_TOP_MID, 0, 0);

    // --- Row 2: State label (vertically centered - big click target) ---
    lv_obj_t *state_label = lv_label_create(tile);
    lv_label_set_text(state_label, state_to_text(t->state));
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(state_label, state_to_text_color(t->state), LV_PART_MAIN);
    lv_obj_align(state_label, LV_ALIGN_CENTER, 0, -2);

    // --- Row 3: Edit | Delete buttons (bottom) ---
    lv_obj_t *edit_btn = lv_btn_create(tile);
    lv_obj_set_size(edit_btn, ICON_BTN_SIZE, ICON_BTN_SIZE);
    lv_obj_set_style_pad_all(edit_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(edit_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(edit_btn, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_radius(edit_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(edit_btn, 0, LV_PART_MAIN);
    lv_obj_align(edit_btn, LV_ALIGN_BOTTOM_LEFT, 12, 0);
    lv_obj_t *edit_icon = lv_label_create(edit_btn);
    lv_label_set_text(edit_icon, LV_SYMBOL_EDIT);
    lv_obj_set_style_text_color(edit_icon, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(edit_icon);
    lv_obj_add_event_cb(edit_btn, edit_btn_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);

    lv_obj_t *del_btn = lv_btn_create(tile);
    lv_obj_set_size(del_btn, ICON_BTN_SIZE, ICON_BTN_SIZE);
    lv_obj_set_style_pad_all(del_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(del_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_radius(del_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(del_btn, 0, LV_PART_MAIN);
    lv_obj_align(del_btn, LV_ALIGN_BOTTOM_RIGHT, -12, 0);
    lv_obj_t *del_icon = lv_label_create(del_btn);
    lv_label_set_text(del_icon, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(del_icon, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(del_icon);
    lv_obj_add_event_cb(del_btn, trash_btn_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);

    // Click handler for toggle (tile background)
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)index);

    return tile;
}

// ============================================================================
// Public API
// ============================================================================

void ui_create_turnouts_tab(lv_obj_t *parent)
{
    s_parent = parent;
    
    // Make parent scrollable
    lv_obj_set_style_pad_all(parent, TILE_PAD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(parent, lv_color_hex(COLOR_BG), LV_PART_MAIN);

    // Create flex container for the grid
    s_grid_container = lv_obj_create(parent);
    lv_obj_set_size(s_grid_container, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_grid_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_grid_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_grid_container, 0, LV_PART_MAIN);
    lv_obj_set_layout(s_grid_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_grid_container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_grid_container, TILE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_grid_container, TILE_PAD, LV_PART_MAIN);

    // Empty state label
    s_empty_label = lv_label_create(parent);
    lv_label_set_text(s_empty_label, "No turnouts configured.\n\nUse the \"Add Turnout\" tab to add turnouts.");
    lv_obj_set_style_text_font(s_empty_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_empty_label, lv_color_hex(0x757575), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_empty_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_empty_label, LV_ALIGN_CENTER, 0, 0);

    // Initial refresh
    ui_turnouts_refresh();
}

void ui_turnouts_refresh(void)
{
    if (!s_grid_container) return;

    // Clear existing tiles
    lv_obj_clean(s_grid_container);
    s_tile_count = 0;
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_tile_names, 0, sizeof(s_tile_names));
    memset(s_tile_states, 0, sizeof(s_tile_states));

    size_t count = turnout_manager_get_count();

    if (count == 0) {
        lv_obj_add_flag(s_grid_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_grid_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < count && i < TURNOUT_MAX_COUNT; i++) {
        turnout_t t;
        if (turnout_manager_get_by_index(i, &t) == ESP_OK) {
            lv_obj_t *tile = create_tile(s_grid_container, (int)i, &t);
            s_tiles[i] = tile;

            // Find name and state labels (first and second children)
            s_tile_names[i] = lv_obj_get_child(tile, 0);
            s_tile_states[i] = lv_obj_get_child(tile, 1);
        }
    }
    s_tile_count = (int)count;
}

void ui_turnouts_update_tile(int index, turnout_state_t state)
{
    if (index < 0 || index >= s_tile_count || !s_tiles[index]) return;

    lv_obj_t *tile = s_tiles[index];
    
    // Update background color
    lv_obj_set_style_bg_color(tile, state_to_bg_color(state), LV_PART_MAIN);

    // Update text colors
    lv_color_t text_color = state_to_text_color(state);
    if (s_tile_names[index]) {
        lv_obj_set_style_text_color(s_tile_names[index], text_color, LV_PART_MAIN);
    }
    if (s_tile_states[index]) {
        lv_label_set_text(s_tile_states[index], state_to_text(state));
        lv_obj_set_style_text_color(s_tile_states[index], text_color, LV_PART_MAIN);
    }

    // Clear pending indicator when we get a state update
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
}

void ui_turnouts_clear_pending(int index)
{
    if (index < 0 || index >= s_tile_count || !s_tiles[index]) return;
    lv_obj_set_style_border_width(s_tiles[index], 0, LV_PART_MAIN);
}
