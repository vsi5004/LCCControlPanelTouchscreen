/**
 * @file ui_turnouts.c
 * @brief Turnout Switchboard Tab — grid of color-coded turnout tiles
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
#define TILE_HEIGHT     80
#define TILE_PAD        8
#define TILE_RADIUS     8
#define COLS_PER_ROW    5

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

// Array of tile objects — indexed same as turnout_manager
static lv_obj_t *s_tiles[TURNOUT_MAX_COUNT];
static lv_obj_t *s_tile_names[TURNOUT_MAX_COUNT];
static lv_obj_t *s_tile_states[TURNOUT_MAX_COUNT];
static int s_tile_count = 0;

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
        case TURNOUT_STATE_NORMAL:  return "NORMAL";
        case TURNOUT_STATE_REVERSE: return "REVERSE";
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
// Event callback — tile tap → toggle turnout
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
        // NORMAL, UNKNOWN, or STALE → send REVERSE
        event_to_send = t.event_reverse;
    }

    ESP_LOGI(TAG, "Toggle turnout '%s' → sending %016llx", t.name,
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

    // Turnout name
    lv_obj_t *name_label = lv_label_create(tile);
    lv_label_set_text(name_label, t->name);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_label, state_to_text_color(t->state), LV_PART_MAIN);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_label, TILE_WIDTH - 16);
    lv_obj_align(name_label, LV_ALIGN_TOP_MID, 0, 0);

    // State label
    lv_obj_t *state_label = lv_label_create(tile);
    lv_label_set_text(state_label, state_to_text(t->state));
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(state_label, state_to_text_color(t->state), LV_PART_MAIN);
    lv_obj_align(state_label, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Click handler
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
