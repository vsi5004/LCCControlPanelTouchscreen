/**
 * @file ui_panel.c
 * @brief Control Panel Screen — Layout Diagram with Turnouts and Tracks
 *
 * This is the default screen shown on boot. It displays a spatial diagram
 * of turnout Y-shapes at user-defined positions, connected by straight track
 * lines. Tapping a turnout toggles its position via LCC events. A settings
 * gear icon in the upper-right navigates to the settings tabs.
 */

#include "ui_common.h"
#include "panel_layout.h"
#include "panel_geometry.h"
#include "app/turnout_manager.h"
#include "app/lcc_node.h"
#include "app/panel_storage.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_panel";

// ============================================================================
// Module State
// ============================================================================

/**
 * Layout data is owned by panel_layout.c (singleton).
 * The s_layout macro maintains source compatibility with existing code.
 */
#define s_layout (*panel_layout_get())

/// LVGL objects
static lv_obj_t *s_panel_screen = NULL;     ///< The full-screen container
static lv_obj_t *s_canvas = NULL;           ///< Canvas area for layout diagram
static lv_obj_t *s_empty_label = NULL;      ///< "No layout configured" label
static lv_obj_t *s_empty_btn = NULL;        ///< "Open Panel Builder" button

/// Per-item LVGL objects for lightweight state updates
#define MAX_LINES_PER_ITEM 2
static lv_obj_t *s_item_lines[PANEL_MAX_ITEMS][MAX_LINES_PER_ITEM];  ///< line objects per item
static lv_obj_t *s_item_hitbox[PANEL_MAX_ITEMS];                      ///< clickable overlay per item
static lv_obj_t *s_track_lines[PANEL_MAX_TRACKS];                     ///< track line objects
static size_t s_rendered_item_count = 0;
static size_t s_rendered_track_count = 0;

/// Static line point arrays (LVGL line needs persistent point data)
static lv_point_t s_line_points[PANEL_MAX_ITEMS][MAX_LINES_PER_ITEM][2];

/// Track line point arrays
static lv_point_t s_track_points[PANEL_MAX_TRACKS][2];

// ============================================================================
// Color Helpers
// ============================================================================

#define COLOR_NORMAL    0x4CAF50    // Green
#define COLOR_REVERSE   0xFFC107    // Amber
#define COLOR_UNKNOWN   0x9E9E9E    // Grey
#define COLOR_STALE     0xF44336    // Red
#define COLOR_TRACK     0x424242    // Dark grey for track lines
#define COLOR_PANEL_BG  0x1E1E1E    // Dark background for layout
#define COLOR_HEADER_BG 0x333333    // Header bar background
#define COLOR_ORPHAN    0x795548    // Brown for unresolved turnouts

/** @brief Padding (pixels) inside canvas when auto-fitting the layout */
#define FIT_MARGIN 20

/// Auto-fit transform: computed each render to scale+center layout in canvas
static int16_t s_fit_scale_pct = 100;   ///< Scale percentage (100 = 1:1)
static int16_t s_fit_off_x = 0;        ///< X offset (pixels) applied after scale
static int16_t s_fit_off_y = 0;        ///< Y offset (pixels) applied after scale

/** @brief Transform a world-space coordinate to canvas-space */
static inline int16_t fit_x(int16_t wx)
{
    return (int16_t)((int32_t)wx * s_fit_scale_pct / 100 + s_fit_off_x);
}
static inline int16_t fit_y(int16_t wy)
{
    return (int16_t)((int32_t)wy * s_fit_scale_pct / 100 + s_fit_off_y);
}

static lv_color_t state_to_color_normal_leg(turnout_state_t state)
{
    switch (state) {
        case TURNOUT_STATE_NORMAL:  return lv_color_hex(COLOR_NORMAL);
        case TURNOUT_STATE_REVERSE: return lv_color_hex(COLOR_UNKNOWN);  // dim when reverse
        case TURNOUT_STATE_STALE:   return lv_color_hex(COLOR_STALE);
        default:                    return lv_color_hex(COLOR_UNKNOWN);
    }
}

static lv_color_t state_to_color_reverse_leg(turnout_state_t state)
{
    switch (state) {
        case TURNOUT_STATE_NORMAL:  return lv_color_hex(COLOR_UNKNOWN);  // dim when normal
        case TURNOUT_STATE_REVERSE: return lv_color_hex(COLOR_REVERSE);
        case TURNOUT_STATE_STALE:   return lv_color_hex(COLOR_STALE);
        default:                    return lv_color_hex(COLOR_UNKNOWN);
    }
}

// ============================================================================
// Turnout Click Handler
// ============================================================================

static void turnout_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    size_t item_idx = packed;

    if (item_idx >= s_layout.item_count) return;

    uint64_t event_normal = s_layout.items[item_idx].event_normal;

    // Find the turnout in the manager by event
    int tm_idx = turnout_manager_find_by_event(event_normal);
    if (tm_idx < 0) {
        ESP_LOGW(TAG, "Turnout not found in manager for panel item %d", (int)item_idx);
        return;
    }

    turnout_t t;
    if (turnout_manager_get_by_index((size_t)tm_idx, &t) != ESP_OK) return;

    // Toggle: send the opposite state's event
    uint64_t send_event;
    if (t.state == TURNOUT_STATE_REVERSE) {
        send_event = t.event_normal;
    } else {
        send_event = t.event_reverse;
    }

    ESP_LOGI(TAG, "Toggle turnout '%s' (panel item %d, manager idx %d)",
             t.name, (int)item_idx, tm_idx);

    turnout_manager_set_pending((size_t)tm_idx, true);
    lcc_node_send_event(send_event);
}

// ============================================================================
// Settings Button Handler
// ============================================================================

static void settings_nav_async(void *param)
{
    (void)param;
    ui_show_settings();
}

static void builder_nav_async(void *param)
{
    (void)param;
    ui_show_settings_at_tab(2);  // Panel Builder is tab index 2
}

static void settings_btn_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    ESP_LOGI(TAG, "Settings button pressed — navigating to settings");
    // Defer navigation: can't destroy current screen from inside its event handler
    lv_async_call(settings_nav_async, NULL);
}

// ============================================================================
// Empty State Button Handler
// ============================================================================

static void open_builder_btn_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    ESP_LOGI(TAG, "Open Panel Builder pressed");
    // Defer navigation to avoid destroying objects mid-event
    lv_async_call(builder_nav_async, NULL);
}

// ============================================================================
// Rendering
// ============================================================================

/**
 * @brief Clear all rendered objects from the canvas
 */
static void panel_clear_render(void)
{
    // Items
    for (size_t i = 0; i < s_rendered_item_count; i++) {
        for (int l = 0; l < MAX_LINES_PER_ITEM; l++) {
            if (s_item_lines[i][l]) {
                lv_obj_del(s_item_lines[i][l]);
                s_item_lines[i][l] = NULL;
            }
        }
        if (s_item_hitbox[i]) {
            lv_obj_del(s_item_hitbox[i]);
            s_item_hitbox[i] = NULL;
        }
    }
    s_rendered_item_count = 0;

    // Tracks
    for (size_t i = 0; i < s_rendered_track_count; i++) {
        if (s_track_lines[i]) {
            lv_obj_del(s_track_lines[i]);
            s_track_lines[i] = NULL;
        }
    }
    s_rendered_track_count = 0;
}

/**
 * @brief Find the turnout manager index for a given event_normal key
 * @return manager index, or -1 if not found
 */
static int find_turnout_for_event(uint64_t event_normal)
{
    return turnout_manager_find_by_event(event_normal);
}

/**
 * @brief Render all turnout items and track segments on the canvas
 */
static void panel_render(void)
{
    if (!s_canvas) return;

    panel_clear_render();

    // Show/hide empty state
    bool empty = (s_layout.item_count == 0 && s_layout.endpoint_count == 0);
    if (s_empty_label) {
        if (empty) lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_empty_btn) {
        if (empty) lv_obj_clear_flag(s_empty_btn, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(s_empty_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (empty) return;

    // --- Compute auto-fit transform (scale + center) ---
    {
        int16_t min_x, min_y, max_x, max_y;
        if (panel_layout_get_bounds(&s_layout, FIT_MARGIN,
                                    &min_x, &min_y, &max_x, &max_y)) {
            int32_t world_w = max_x - min_x;
            int32_t world_h = max_y - min_y;
            if (world_w < 1) world_w = 1;
            if (world_h < 1) world_h = 1;

            int16_t scale_x = (int16_t)((int32_t)PANEL_CANVAS_WIDTH  * 100 / world_w);
            int16_t scale_y = (int16_t)((int32_t)PANEL_CANVAS_HEIGHT * 100 / world_h);
            s_fit_scale_pct = scale_x < scale_y ? scale_x : scale_y;
            if (s_fit_scale_pct < 10) s_fit_scale_pct = 10;   // safety floor

            int16_t world_cx = (int16_t)((min_x + max_x) / 2);
            int16_t world_cy = (int16_t)((min_y + max_y) / 2);
            s_fit_off_x = (int16_t)(PANEL_CANVAS_WIDTH  / 2
                            - (int32_t)world_cx * s_fit_scale_pct / 100);
            s_fit_off_y = (int16_t)(PANEL_CANVAS_HEIGHT / 2
                            - (int32_t)world_cy * s_fit_scale_pct / 100);
        } else {
            s_fit_scale_pct = 100;
            s_fit_off_x = 0;
            s_fit_off_y = 0;
        }
    }

    // --- Render turnout items ---
    for (size_t i = 0; i < s_layout.item_count && i < PANEL_MAX_ITEMS; i++) {
        const panel_item_t *pi = &s_layout.items[i];

        lv_point_t entry, normal, reverse;
        panel_geometry_get_points(pi, &entry, &normal, &reverse);

        // Look up turnout state
        int tm_idx = find_turnout_for_event(pi->event_normal);
        turnout_state_t state = TURNOUT_STATE_UNKNOWN;
        if (tm_idx >= 0) {
            turnout_t t;
            if (turnout_manager_get_by_index((size_t)tm_idx, &t) == ESP_OK) {
                state = t.state;
            }
        }

        // Entry → Normal line (straight leg)
        s_line_points[i][0][0].x = fit_x((int16_t)entry.x);
        s_line_points[i][0][0].y = fit_y((int16_t)entry.y);
        s_line_points[i][0][1].x = fit_x((int16_t)normal.x);
        s_line_points[i][0][1].y = fit_y((int16_t)normal.y);

        int16_t line_w = (int16_t)(4 * s_fit_scale_pct / 100);
        if (line_w < 2) line_w = 2;

        lv_obj_t *line_normal = lv_line_create(s_canvas);
        lv_line_set_points(line_normal, s_line_points[i][0], 2);
        lv_obj_set_style_line_width(line_normal, line_w, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(line_normal, true, LV_PART_MAIN);
        lv_obj_set_style_line_color(line_normal,
            (tm_idx >= 0) ? state_to_color_normal_leg(state) : lv_color_hex(COLOR_ORPHAN),
            LV_PART_MAIN);
        s_item_lines[i][0] = line_normal;

        // Entry → Reverse line (diverging leg)
        s_line_points[i][1][0].x = fit_x((int16_t)entry.x);
        s_line_points[i][1][0].y = fit_y((int16_t)entry.y);
        s_line_points[i][1][1].x = fit_x((int16_t)reverse.x);
        s_line_points[i][1][1].y = fit_y((int16_t)reverse.y);

        lv_obj_t *line_reverse = lv_line_create(s_canvas);
        lv_line_set_points(line_reverse, s_line_points[i][1], 2);
        lv_obj_set_style_line_width(line_reverse, line_w, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(line_reverse, true, LV_PART_MAIN);
        lv_obj_set_style_line_color(line_reverse,
            (tm_idx >= 0) ? state_to_color_reverse_leg(state) : lv_color_hex(COLOR_ORPHAN),
            LV_PART_MAIN);
        s_item_lines[i][1] = line_reverse;

        // Clickable hitbox overlay (invisible, touch-friendly size)
        int16_t cx, cy;
        panel_geometry_get_center(pi, &cx, &cy);
        cx = fit_x(cx);
        cy = fit_y(cy);

        // Scale hitbox but keep minimum touch-friendly size
        int16_t hb_w = (int16_t)(70 * s_fit_scale_pct / 100);
        int16_t hb_h = (int16_t)(50 * s_fit_scale_pct / 100);
        if (hb_w < 40) hb_w = 40;
        if (hb_h < 30) hb_h = 30;

        lv_obj_t *hitbox = lv_obj_create(s_canvas);
        lv_obj_remove_style_all(hitbox);
        lv_obj_set_size(hitbox, hb_w, hb_h);
        lv_obj_set_pos(hitbox, cx - hb_w / 2, cy - hb_h / 2);
        lv_obj_add_flag(hitbox, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(hitbox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(hitbox, turnout_click_cb, LV_EVENT_CLICKED,
                           (void *)(uintptr_t)i);
        s_item_hitbox[i] = hitbox;

        // If turnout not found in manager, show a "?" label
        if (tm_idx < 0) {
            lv_obj_t *q_label = lv_label_create(hitbox);
            lv_label_set_text(q_label, "?");
            lv_obj_set_style_text_font(q_label, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(q_label, lv_color_hex(COLOR_ORPHAN), LV_PART_MAIN);
            lv_obj_center(q_label);
        }
    }
    s_rendered_item_count = s_layout.item_count;

    // --- Render track segments ---
    for (size_t i = 0; i < s_layout.track_count && i < PANEL_MAX_TRACKS; i++) {
        const panel_track_t *pt = &s_layout.tracks[i];

        // Resolve track endpoints via shared layout operation
        int16_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (!panel_layout_resolve_track(&s_layout, pt, &x1, &y1, &x2, &y2)) continue;

        s_track_points[i][0].x = fit_x(x1);
        s_track_points[i][0].y = fit_y(y1);
        s_track_points[i][1].x = fit_x(x2);
        s_track_points[i][1].y = fit_y(y2);

        int16_t track_w = (int16_t)(4 * s_fit_scale_pct / 100);
        if (track_w < 2) track_w = 2;

        lv_obj_t *track_line = lv_line_create(s_canvas);
        lv_line_set_points(track_line, s_track_points[i], 2);
        lv_obj_set_style_line_width(track_line, track_w, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(track_line, true, LV_PART_MAIN);
        lv_obj_set_style_line_color(track_line, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
        s_track_lines[i] = track_line;
    }
    s_rendered_track_count = s_layout.track_count;

    ESP_LOGI(TAG, "Panel rendered: %d items, %d tracks (fit: %d%% offset %d,%d)",
             (int)s_rendered_item_count, (int)s_rendered_track_count,
             (int)s_fit_scale_pct, (int)s_fit_off_x, (int)s_fit_off_y);
}

// ============================================================================
// Public API
// ============================================================================

void ui_create_panel_screen(void)
{
    ESP_LOGI(TAG, "Creating control panel screen");

    ui_lock();

    // Invalidate turnout tiles — settings screen objects are about to be destroyed
    ui_turnouts_invalidate();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_PANEL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_panel_screen = scr;

    // Reset render state
    s_rendered_item_count = 0;
    s_rendered_track_count = 0;
    memset(s_item_lines, 0, sizeof(s_item_lines));
    memset(s_item_hitbox, 0, sizeof(s_item_hitbox));
    memset(s_track_lines, 0, sizeof(s_track_lines));

    // --- Header bar with settings button ---
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, PANEL_CANVAS_WIDTH, PANEL_HEADER_HEIGHT);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(COLOR_HEADER_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Title label
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Control Panel");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);

    // Settings gear button
    lv_obj_t *settings_btn = lv_btn_create(header);
    lv_obj_set_size(settings_btn, 40, 36);
    lv_obj_align(settings_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(settings_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_radius(settings_btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(settings_btn, settings_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *gear_label = lv_label_create(settings_btn);
    lv_label_set_text(gear_label, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(gear_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(gear_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(gear_label);

    // --- Canvas area for layout diagram ---
    s_canvas = lv_obj_create(scr);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_size(s_canvas, PANEL_CANVAS_WIDTH, PANEL_CANVAS_HEIGHT);
    lv_obj_set_pos(s_canvas, 0, PANEL_HEADER_HEIGHT);
    lv_obj_set_style_bg_color(s_canvas, lv_color_hex(COLOR_PANEL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_canvas, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);

    // --- Empty state ---
    s_empty_label = lv_label_create(s_canvas);
    lv_label_set_text(s_empty_label, "No layout configured");
    lv_obj_set_style_text_font(s_empty_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_empty_label, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(s_empty_label, LV_ALIGN_CENTER, 0, -30);

    s_empty_btn = lv_btn_create(s_canvas);
    lv_obj_set_size(s_empty_btn, 220, 44);
    lv_obj_align(s_empty_btn, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(s_empty_btn, lv_color_hex(0x2196F3), LV_PART_MAIN);
    lv_obj_set_style_radius(s_empty_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(s_empty_btn, open_builder_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(s_empty_btn);
    lv_label_set_text(btn_label, "Open Panel Builder");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(btn_label);

    // Render the layout
    panel_render();

    ui_unlock();

    ESP_LOGI(TAG, "Control panel screen created");
}

void ui_panel_update_turnout(int index, turnout_state_t state)
{
    // Find which panel items correspond to this turnout manager index
    turnout_t t;
    if (turnout_manager_get_by_index((size_t)index, &t) != ESP_OK) return;

    for (size_t i = 0; i < s_rendered_item_count; i++) {
        if (s_layout.items[i].event_normal == t.event_normal) {
            // Update normal leg color
            if (s_item_lines[i][0]) {
                lv_obj_set_style_line_color(s_item_lines[i][0],
                    state_to_color_normal_leg(state), LV_PART_MAIN);
            }
            // Update reverse leg color
            if (s_item_lines[i][1]) {
                lv_obj_set_style_line_color(s_item_lines[i][1],
                    state_to_color_reverse_leg(state), LV_PART_MAIN);
            }
        }
    }
}

void ui_panel_invalidate(void)
{
    s_canvas = NULL;
    s_panel_screen = NULL;
    s_empty_label = NULL;
    s_empty_btn = NULL;
    s_rendered_item_count = 0;
    s_rendered_track_count = 0;
    memset(s_item_lines, 0, sizeof(s_item_lines));
    memset(s_item_hitbox, 0, sizeof(s_item_hitbox));
    memset(s_track_lines, 0, sizeof(s_track_lines));
}

void ui_panel_refresh(void)
{
    if (!s_canvas) return;
    ui_lock();
    panel_render();
    ui_unlock();
}
