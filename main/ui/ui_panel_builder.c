/**
 * @file ui_panel_builder.c
 * @brief Panel Builder Tab — Drag-and-place turnout layout editor
 *
 * Third tab in the settings screen. Allows the user to:
 * - Place turnouts via a "+ Turnout" toolbar button that opens a modal list
 * - Drag placed turnouts to reposition with snap-to-grid (20px)
 * - Rotate (8 orientations) and mirror (left/right hand) placed turnouts
 * - Draw track segments between turnout connection points
 * - Delete placed turnouts from the layout (not from the network)
 * - Save/load the layout to/from /sdcard/panel.json
 */

#include "ui_common.h"
#include "panel_layout.h"
#include "panel_geometry.h"
#include "app/turnout_manager.h"
#include "app/panel_storage.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_panel_builder";

// ============================================================================
// Layout Constants
// ============================================================================

#define BUILDER_CANVAS_WIDTH    748     ///< Canvas width (800 - nav bar)
#define BUILDER_CANVAS_HEIGHT   382     ///< Canvas height (430 content - 48 toolbar)
#define BUILDER_TOOLBAR_HEIGHT  48      ///< Toolbar height at top of canvas
#define BUILDER_CANVAS_OFFSET_Y BUILDER_TOOLBAR_HEIGHT
#define BUILDER_NAV_WIDTH       52      ///< Width of zoom/pan navigation bar

#define HITBOX_SIZE             24      ///< Connection point hitbox radius
#define PLACED_HITBOX_W         70      ///< Placed turnout drag area width
#define PLACED_HITBOX_H         50      ///< Placed turnout drag area height

// Zoom/pan constants
#define ZOOM_MIN    50          ///< Minimum zoom percentage (50% = 0.5x)
#define ZOOM_MAX    300         ///< Maximum zoom percentage (300% = 3x)
#define ZOOM_DEFAULT 100        ///< Default zoom percentage (100% = 1x)
#define ZOOM_STEP   25          ///< Zoom increment per button press
#define PAN_STEP    40          ///< Pan increment in pixels per button press

// Modal dimensions
#define MODAL_WIDTH             400     ///< Turnout selection modal width
#define MODAL_HEIGHT            340     ///< Turnout selection modal height
#define MODAL_ITEM_HEIGHT       44      ///< Each list item height in modal

// Colors
#define COLOR_CANVAS_BG     0x2A2A2A
#define COLOR_GRID_LINE     0x3A3A3A
#define COLOR_SELECTED      0x2196F3
#define COLOR_CONN_POINT    0x00BCD4
#define COLOR_CONN_ACTIVE   0xFF5722
#define COLOR_BTN_SAVE      0x4CAF50
#define COLOR_BTN_ROTATE    0x2196F3
#define COLOR_BTN_MIRROR    0x9C27B0
#define COLOR_BTN_DELETE    0xF44336
#define COLOR_BTN_TRACK     0xFF9800
#define COLOR_BTN_ADD       0x009688
#define COLOR_NORMAL_LINE   0x9E9E9E
#define COLOR_TRACK_DRAW    0x424242
#define COLOR_ENDPOINT      0x42A5F5

// ============================================================================
// Module State
// ============================================================================

static lv_obj_t *s_builder_parent = NULL;   ///< Tab container
static lv_obj_t *s_canvas = NULL;           ///< Full-width canvas preview

/// Toolbar buttons
static lv_obj_t *s_btn_rotate = NULL;
static lv_obj_t *s_btn_mirror = NULL;
static lv_obj_t *s_btn_delete = NULL;
static lv_obj_t *s_btn_draw_track = NULL;
static lv_obj_t *s_btn_save = NULL;
static lv_obj_t *s_btn_add_turnout = NULL;
static lv_obj_t *s_btn_add_endpoint = NULL;

/// Turnout selection modal
static lv_obj_t *s_modal_overlay = NULL;

/// Zoom/pan viewport state
static int16_t s_zoom_pct = ZOOM_DEFAULT;   ///< Zoom percentage (100 = 1:1)
static int16_t s_pan_x = 0;                 ///< Pan offset X in screen pixels
static int16_t s_pan_y = 0;                 ///< Pan offset Y in screen pixels
static lv_obj_t *s_zoom_label = NULL;       ///< Zoom percentage label

/// Mode state
static bool s_placement_mode = false;       ///< Waiting for canvas tap to place turnout
static int  s_placement_turnout_idx = -1;   ///< Manager index of turnout being placed
static bool s_draw_track_mode = false;      ///< Track drawing mode active
static int  s_selected_item = -1;           ///< Currently selected placed item index (-1 = none)

/// Track drawing state
static bool        s_track_first_selected = false;
static panel_ref_t s_track_from_ref = {0};     ///< "from" end of track being drawn

/// Track selection state (-1 = no track selected)
static int s_selected_track = -1;

/// Endpoint selection and placement state
static int  s_selected_endpoint = -1;          ///< Selected endpoint array index (-1 = none)
static bool s_placement_endpoint_mode = false;  ///< Waiting for tap to place endpoint

/// Dirty flag — true when layout has unsaved changes
static bool s_dirty = false;

/// Save button label (for changing text on save confirmation)
static lv_obj_t *s_save_label = NULL;
static lv_timer_t *s_save_flash_timer = NULL;

/// Drag state: when non-NULL, this hitbox is being actively dragged and
/// must NOT be deleted by builder_clear_canvas(). builder_refresh_canvas()
/// will skip creating a new hitbox for the dragged item and reuse this one.
static lv_obj_t *s_drag_active_hitbox = NULL;
static int       s_drag_active_idx = -1;
static bool      s_drag_active_is_endpoint = false;

/// Rendered objects on canvas
#define BUILDER_MAX_LINES (PANEL_MAX_ITEMS * 2)
static lv_obj_t *s_preview_objs[PANEL_MAX_ITEMS * 8 + PANEL_MAX_ENDPOINTS * 3 + PANEL_MAX_TRACKS * 2 + 4];
static size_t s_preview_obj_count = 0;

/// Static point arrays for preview lines
static lv_point_t s_preview_line_pts[BUILDER_MAX_LINES + PANEL_MAX_TRACKS][2];

// ============================================================================
// Forward Declarations
// ============================================================================

static void builder_refresh_canvas(void);
static void builder_refresh_toolbar(void);
static void update_zoom_label(void);

// ============================================================================
// Viewport Transform Helpers
// ============================================================================

/**
 * @brief Transform a world-space coordinate to screen-space (canvas pixel)
 */
static inline int16_t world_to_view_x(int16_t wx)
{
    return (int16_t)((int32_t)wx * s_zoom_pct / 100 + s_pan_x);
}

static inline int16_t world_to_view_y(int16_t wy)
{
    return (int16_t)((int32_t)wy * s_zoom_pct / 100 + s_pan_y);
}

/**
 * @brief Transform a screen-space (canvas pixel) to world-space coordinate
 */
static inline int16_t view_to_world_x(int16_t vx)
{
    return (int16_t)(((int32_t)vx - s_pan_x) * 100 / s_zoom_pct);
}

static inline int16_t view_to_world_y(int16_t vy)
{
    return (int16_t)(((int32_t)vy - s_pan_y) * 100 / s_zoom_pct);
}

/**
 * @brief Transform a world-space lv_point_t to view-space
 */
static inline lv_point_t world_to_view_pt(const lv_point_t *wp)
{
    lv_point_t vp;
    vp.x = world_to_view_x(wp->x);
    vp.y = world_to_view_y(wp->y);
    return vp;
}

/**
 * @brief Convert absolute screen touch point to world-space grid coords.
 *        Uses lv_obj_get_coords() for correct absolute positioning regardless
 *        of how deeply the canvas is nested in the widget hierarchy,
 *        then applies inverse viewport transform.
 */
static void screen_to_canvas_grid(const lv_point_t *screen_pt,
                                   int16_t *out_grid_x, int16_t *out_grid_y)
{
    lv_area_t canvas_area;
    lv_obj_get_coords(s_canvas, &canvas_area);

    // Screen touch -> canvas-local pixel
    int16_t local_x = (int16_t)(screen_pt->x - canvas_area.x1);
    int16_t local_y = (int16_t)(screen_pt->y - canvas_area.y1);

    // Canvas-local pixel -> world pixel
    int16_t world_x = view_to_world_x(local_x);
    int16_t world_y = view_to_world_y(local_y);

    // World pixel -> grid snap
    *out_grid_x = (world_x + PANEL_GRID_SIZE / 2) / PANEL_GRID_SIZE;
    *out_grid_y = (world_y + PANEL_GRID_SIZE / 2) / PANEL_GRID_SIZE;
}

// ============================================================================
// Helper: Check if turnout is already placed on panel
// ============================================================================

static bool is_turnout_placed(uint32_t turnout_id)
{
    return panel_layout_is_turnout_placed(panel_layout_get(), turnout_id);
}

// ============================================================================
// Modal: Turnout selection
// ============================================================================

static void modal_close(void)
{
    if (s_modal_overlay) {
        lv_obj_del(s_modal_overlay);
        s_modal_overlay = NULL;
    }
}

static void modal_overlay_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    // Only close if the overlay itself (not a child) was clicked
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        modal_close();
    }
}

static void modal_item_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    int tm_idx = (int)(uintptr_t)lv_event_get_user_data(e);

    s_placement_mode = true;
    s_placement_turnout_idx = tm_idx;
    s_draw_track_mode = false;
    s_track_first_selected = false;

    ESP_LOGI(TAG, "Placement mode: turnout manager index %d — tap canvas to place", tm_idx);

    modal_close();
    builder_refresh_toolbar();
    builder_refresh_canvas();
}

static void modal_close_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    modal_close();
}

/**
 * @brief Open the turnout selection modal over the current screen
 */
static void open_turnout_modal(void)
{
    // Close any existing modal
    modal_close();

    // Get the active screen as modal parent (so it overlays everything)
    lv_obj_t *scr = lv_scr_act();

    // Semi-transparent overlay covering the entire screen
    s_modal_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_modal_overlay);
    lv_obj_set_size(s_modal_overlay, 800, 480);
    lv_obj_set_pos(s_modal_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_modal_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_modal_overlay, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_flag(s_modal_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_modal_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_modal_overlay, modal_overlay_click_cb, LV_EVENT_CLICKED, NULL);

    // Modal card — centered
    lv_obj_t *card = lv_obj_create(s_modal_overlay);
    lv_obj_set_size(card, MODAL_WIDTH, MODAL_HEIGHT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Title row
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Select Turnout to Place");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x212121), LV_PART_MAIN);
    lv_obj_set_pos(title, 0, 0);

    // Close "X" button
    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xEEEEEE), LV_PART_MAIN);
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(close_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(close_btn, modal_close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x_lbl = lv_label_create(close_btn);
    lv_label_set_text(x_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x_lbl, lv_color_hex(0x616161), LV_PART_MAIN);
    lv_obj_center(x_lbl);

    // Scrollable list area
    lv_obj_t *list_area = lv_obj_create(card);
    lv_obj_remove_style_all(list_area);
    lv_obj_set_size(list_area, MODAL_WIDTH - 32, MODAL_HEIGHT - 72);
    lv_obj_set_pos(list_area, 0, 36);
    lv_obj_set_layout(list_area, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(list_area, 4, LV_PART_MAIN);
    lv_obj_add_flag(list_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_area, LV_DIR_VER);

    // Populate with unplaced turnouts
    size_t count = turnout_manager_get_count();
    int available = 0;

    for (size_t i = 0; i < count; i++) {
        turnout_t t;
        if (turnout_manager_get_by_index(i, &t) != ESP_OK) continue;
        if (is_turnout_placed(t.id)) continue;

        lv_obj_t *item = lv_obj_create(list_area);
        lv_obj_set_size(item, MODAL_WIDTH - 40, MODAL_ITEM_HEIGHT);
        lv_obj_set_style_bg_color(item, lv_color_hex(0xF5F5F5), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(item, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_left(item, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_right(item, 12, LV_PART_MAIN);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(item, modal_item_click_cb, LV_EVENT_CLICKED,
                           (void *)(uintptr_t)i);

        lv_obj_t *name_lbl = lv_label_create(item);
        lv_label_set_text(name_lbl, t.name);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(name_lbl, lv_color_hex(0x212121), LV_PART_MAIN);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *plus_lbl = lv_label_create(item);
        lv_label_set_text(plus_lbl, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_color(plus_lbl, lv_color_hex(COLOR_BTN_ADD), LV_PART_MAIN);
        lv_obj_align(plus_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

        available++;
    }

    if (available == 0) {
        lv_obj_t *empty_lbl = lv_label_create(list_area);
        lv_label_set_text(empty_lbl, "All turnouts have been placed\n"
                          "or no turnouts are configured.\n\n"
                          "Use the Add Turnout tab first.");
        lv_obj_set_style_text_font(empty_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(empty_lbl, lv_color_hex(0x757575), LV_PART_MAIN);
        lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_width(empty_lbl, MODAL_WIDTH - 40);
        lv_obj_center(empty_lbl);
    }
}

// ============================================================================
// Canvas Event Handlers
// ============================================================================

/**
 * @brief Canvas tap handler — places turnout or starts track connection
 */
static void canvas_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Convert screen touch to canvas grid coordinates
    int16_t grid_x, grid_y;
    screen_to_canvas_grid(&point, &grid_x, &grid_y);

    // Clamp to canvas bounds
    int16_t max_gx = BUILDER_CANVAS_WIDTH / PANEL_GRID_SIZE;
    int16_t max_gy = BUILDER_CANVAS_HEIGHT / PANEL_GRID_SIZE;
    if (grid_x < 0) grid_x = 0;
    if (grid_y < 0) grid_y = 0;
    if (grid_x > max_gx) grid_x = max_gx;
    if (grid_y > max_gy) grid_y = max_gy;

    if (s_placement_mode && s_placement_turnout_idx >= 0) {
        // Place the turnout
        turnout_t t;
        if (turnout_manager_get_by_index((size_t)s_placement_turnout_idx, &t) != ESP_OK) {
            s_placement_mode = false;
            return;
        }

        panel_layout_t *layout = panel_layout_get();

        int idx = panel_layout_add_item(layout, t.id,
                                         (uint16_t)grid_x, (uint16_t)grid_y);
        if (idx < 0) {
            s_placement_mode = false;
            return;
        }

        ESP_LOGI(TAG, "Placed turnout '%s' at grid (%d, %d)", t.name, grid_x, grid_y);

        s_placement_mode = false;
        s_placement_turnout_idx = -1;
        s_selected_item = idx;
        s_selected_endpoint = -1;
        s_selected_track = -1;
        s_dirty = true;

        builder_refresh_canvas();
        builder_refresh_toolbar();
        return;
    }

    if (s_placement_endpoint_mode) {
        // Place an endpoint
        panel_layout_t *layout = panel_layout_get();
        size_t ep_idx = 0;
        if (!panel_layout_add_endpoint(layout, (uint16_t)grid_x, (uint16_t)grid_y, &ep_idx)) {
            s_placement_endpoint_mode = false;
            return;
        }

        s_placement_endpoint_mode = false;
        s_selected_endpoint = (int)ep_idx;
        s_selected_item = -1;
        s_selected_track = -1;
        s_dirty = true;

        builder_refresh_canvas();
        builder_refresh_toolbar();
        return;
    }

    // Otherwise deselect current item, track, and endpoint
    s_selected_item = -1;
    s_selected_track = -1;
    s_selected_endpoint = -1;
    builder_refresh_canvas();
    builder_refresh_toolbar();
}

/**
 * @brief Find the nearest connection point on a placed item to a screen touch.
 */
static panel_point_type_t find_nearest_point(const panel_item_t *pi,
                                              const lv_point_t *screen_pt)
{
    lv_area_t canvas_area;
    lv_obj_get_coords(s_canvas, &canvas_area);

    // Convert screen touch to view-space, then to world-space for comparison
    int16_t touch_vx = (int16_t)(screen_pt->x - canvas_area.x1);
    int16_t touch_vy = (int16_t)(screen_pt->y - canvas_area.y1);
    int16_t touch_x = view_to_world_x(touch_vx);
    int16_t touch_y = view_to_world_y(touch_vy);

    lv_point_t entry, normal_pt, reverse_pt;
    panel_geometry_get_points(pi, &entry, &normal_pt, &reverse_pt);

    // Squared distances to each connection point
    int32_t d_entry   = (int32_t)(touch_x - entry.x)     * (touch_x - entry.x)
                      + (int32_t)(touch_y - entry.y)     * (touch_y - entry.y);
    int32_t d_normal  = (int32_t)(touch_x - normal_pt.x) * (touch_x - normal_pt.x)
                      + (int32_t)(touch_y - normal_pt.y) * (touch_y - normal_pt.y);
    int32_t d_reverse = (int32_t)(touch_x - reverse_pt.x) * (touch_x - reverse_pt.x)
                      + (int32_t)(touch_y - reverse_pt.y) * (touch_y - reverse_pt.y);

    if (d_normal <= d_entry && d_normal <= d_reverse) return PANEL_POINT_NORMAL;
    if (d_reverse <= d_entry && d_reverse <= d_normal) return PANEL_POINT_REVERSE;
    return PANEL_POINT_ENTRY;
}

/**
 * @brief Placed turnout click — select it, or in track draw mode find nearest
 *        connection point as fallback (dots are preferred but hitbox catches
 *        taps that miss a dot).
 */
static void placed_item_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    panel_layout_t *layout = panel_layout_get();
    if (idx < 0 || (size_t)idx >= layout->item_count) return;

    if (s_draw_track_mode) {
        // Determine nearest connection point to the touch position
        lv_indev_t *indev = lv_indev_get_act();
        panel_point_type_t nearest = PANEL_POINT_ENTRY;
        if (indev) {
            lv_point_t touch;
            lv_indev_get_point(indev, &touch);
            nearest = find_nearest_point(&layout->items[idx], &touch);
        }

        uint32_t tid = layout->items[idx].turnout_id;

        if (!s_track_first_selected) {
            s_track_first_selected = true;
            s_track_from_ref = (panel_ref_t){
                .type = PANEL_REF_TURNOUT, .id = tid, .point = nearest
            };
            ESP_LOGI(TAG, "Track start: item %d, point %d", idx, (int)nearest);
        } else {
            // Complete the track
            bool self_conn = s_track_from_ref.type == PANEL_REF_TURNOUT &&
                             tid == s_track_from_ref.id &&
                             nearest == s_track_from_ref.point;
            if (!self_conn) {
                panel_track_t new_track = {
                    .from = s_track_from_ref,
                    .to   = { .type = PANEL_REF_TURNOUT, .id = tid, .point = nearest }
                };
                if (panel_layout_add_track(layout, &new_track)) {
                    s_dirty = true;
                }
            }
            s_track_first_selected = false;
            builder_refresh_canvas();
        }
        return;
    }

    ESP_LOGI(TAG, "Selected placed item %d", idx);
    s_selected_item = idx;
    s_selected_track = -1;
    s_selected_endpoint = -1;
    builder_refresh_canvas();
    builder_refresh_toolbar();
}

/**
 * @brief Placed turnout drag handler — reposition with grid snap.
 *        Does NOT call builder_refresh_canvas() to avoid destroying the
 *        hitbox that is actively being pressed. Instead, moves just the
 *        hitbox widget for visual feedback. Full redraw happens on release.
 */
static void placed_item_drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSING) return;
    if (s_draw_track_mode) return;  // No drag while drawing tracks

    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    panel_layout_t *layout = panel_layout_get();
    if (idx < 0 || (size_t)idx >= layout->item_count) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Convert screen touch to canvas grid coordinates
    int16_t grid_x, grid_y;
    screen_to_canvas_grid(&point, &grid_x, &grid_y);

    // Clamp to canvas bounds
    int16_t max_gx = BUILDER_CANVAS_WIDTH / PANEL_GRID_SIZE;
    int16_t max_gy = BUILDER_CANVAS_HEIGHT / PANEL_GRID_SIZE;
    if (grid_x < 1) grid_x = 1;
    if (grid_y < 1) grid_y = 1;
    if (grid_x > max_gx - 1) grid_x = max_gx - 1;
    if (grid_y > max_gy - 1) grid_y = max_gy - 1;

    // Register the dragged hitbox so builder_clear_canvas() preserves it
    if (!s_drag_active_hitbox) {
        s_drag_active_hitbox = lv_event_get_target(e);
        s_drag_active_idx = idx;
    }

    if (layout->items[idx].grid_x != (uint16_t)grid_x ||
        layout->items[idx].grid_y != (uint16_t)grid_y) {
        layout->items[idx].grid_x = (uint16_t)grid_x;
        layout->items[idx].grid_y = (uint16_t)grid_y;
        s_selected_item = idx;
        s_dirty = true;

        // Full redraw — lines, labels, dots all update live.
        // The active hitbox is protected from deletion by builder_clear_canvas()
        // and reused by builder_refresh_canvas().
        builder_refresh_canvas();
    }
}

/**
 * @brief Placed turnout drag release handler — clear drag state and
 *        do a final clean canvas redraw.
 */
static void placed_item_release_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    if (s_draw_track_mode) return;

    // Clear drag-active state so the next refresh creates a fresh hitbox
    s_drag_active_hitbox = NULL;
    s_drag_active_idx = -1;
    s_drag_active_is_endpoint = false;

    // Final clean redraw
    builder_refresh_canvas();
    builder_refresh_toolbar();
}

/**
 * @brief Connection point click — for track drawing mode
 */
static void conn_point_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;
    if (!s_draw_track_mode) return;

    uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    int item_idx = (int)(packed >> 8);
    panel_point_type_t pt = (panel_point_type_t)(packed & 0xFF);

    panel_layout_t *layout = panel_layout_get();
    if (item_idx < 0 || (size_t)item_idx >= layout->item_count) return;

    uint32_t tid = layout->items[item_idx].turnout_id;

    if (!s_track_first_selected) {
        s_track_first_selected = true;
        s_track_from_ref = (panel_ref_t){
            .type = PANEL_REF_TURNOUT, .id = tid, .point = pt
        };
        ESP_LOGI(TAG, "Track start: item %d, point %d", item_idx, (int)pt);
        builder_refresh_canvas();
    } else {
        // Complete the track
        if (tid != s_track_from_ref.id || pt != s_track_from_ref.point ||
            s_track_from_ref.type != PANEL_REF_TURNOUT) {
            panel_track_t new_track = {
                .from = s_track_from_ref,
                .to   = { .type = PANEL_REF_TURNOUT, .id = tid, .point = pt }
            };
            if (panel_layout_add_track(layout, &new_track)) {
                s_dirty = true;
            }
        }
        s_track_first_selected = false;
        builder_refresh_canvas();
    }
}

/**
 * @brief Track segment click handler — select/deselect a track section
 */
static void track_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_draw_track_mode) return;  // Don't select tracks while drawing new ones

    int track_idx = (int)(uintptr_t)lv_event_get_user_data(e);

    if (s_selected_track == track_idx) {
        // Deselect
        s_selected_track = -1;
    } else {
        s_selected_track = track_idx;
        s_selected_item = -1;
        s_selected_endpoint = -1;
    }

    ESP_LOGI(TAG, "Track selection: %d", s_selected_track);
    builder_refresh_canvas();
    builder_refresh_toolbar();
}

// ============================================================================
// Endpoint Event Handlers
// ============================================================================

/**
 * @brief Endpoint click — select it, or use as track connection in draw mode
 */
static void placed_endpoint_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    panel_layout_t *layout = panel_layout_get();
    if (idx < 0 || (size_t)idx >= layout->endpoint_count) return;

    const panel_endpoint_t *ep = &layout->endpoints[idx];

    if (s_draw_track_mode) {
        if (!s_track_first_selected) {
            s_track_first_selected = true;
            s_track_from_ref = (panel_ref_t){
                .type = PANEL_REF_ENDPOINT, .id = ep->id, .point = PANEL_POINT_ENTRY
            };
            ESP_LOGI(TAG, "Track start: endpoint %u", (unsigned)ep->id);
            builder_refresh_canvas();
        } else {
            // Complete track to this endpoint
            bool self_conn = s_track_from_ref.type == PANEL_REF_ENDPOINT &&
                             s_track_from_ref.id == ep->id;
            if (!self_conn) {
                panel_track_t new_track = {
                    .from = s_track_from_ref,
                    .to   = { .type = PANEL_REF_ENDPOINT, .id = ep->id, .point = PANEL_POINT_ENTRY }
                };
                if (panel_layout_add_track(layout, &new_track)) {
                    s_dirty = true;
                }
            }
            s_track_first_selected = false;
            builder_refresh_canvas();
        }
        return;
    }

    ESP_LOGI(TAG, "Selected endpoint %d (id=%u)", idx, (unsigned)ep->id);
    s_selected_endpoint = idx;
    s_selected_item = -1;
    s_selected_track = -1;
    builder_refresh_canvas();
    builder_refresh_toolbar();
}

/**
 * @brief Endpoint drag handler — reposition with grid snap
 */
static void placed_endpoint_drag_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSING) return;
    if (s_draw_track_mode) return;

    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    panel_layout_t *layout = panel_layout_get();
    if (idx < 0 || (size_t)idx >= layout->endpoint_count) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    int16_t grid_x, grid_y;
    screen_to_canvas_grid(&point, &grid_x, &grid_y);

    int16_t max_gx = BUILDER_CANVAS_WIDTH / PANEL_GRID_SIZE;
    int16_t max_gy = BUILDER_CANVAS_HEIGHT / PANEL_GRID_SIZE;
    if (grid_x < 0) grid_x = 0;
    if (grid_y < 0) grid_y = 0;
    if (grid_x > max_gx) grid_x = max_gx;
    if (grid_y > max_gy) grid_y = max_gy;

    if (!s_drag_active_hitbox) {
        s_drag_active_hitbox = lv_event_get_target(e);
        s_drag_active_idx = idx;
        s_drag_active_is_endpoint = true;
    }

    if (layout->endpoints[idx].grid_x != (uint16_t)grid_x ||
        layout->endpoints[idx].grid_y != (uint16_t)grid_y) {
        layout->endpoints[idx].grid_x = (uint16_t)grid_x;
        layout->endpoints[idx].grid_y = (uint16_t)grid_y;
        s_selected_endpoint = idx;
        s_dirty = true;
        builder_refresh_canvas();
    }
}

/**
 * @brief Endpoint drag release
 */
static void placed_endpoint_release_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    if (s_draw_track_mode) return;

    s_drag_active_hitbox = NULL;
    s_drag_active_idx = -1;
    s_drag_active_is_endpoint = false;
    builder_refresh_canvas();
    builder_refresh_toolbar();
}

/**
 * @brief “+ Endpoint” toolbar button callback
 */
static void add_endpoint_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    s_placement_endpoint_mode = true;
    s_placement_mode = false;
    s_draw_track_mode = false;
    s_track_first_selected = false;

    builder_refresh_toolbar();
    builder_refresh_canvas();
}

// ============================================================================
// Toolbar Event Handlers
// ============================================================================

static void rotate_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_selected_item < 0) return;

    panel_layout_t *layout = panel_layout_get();
    if ((size_t)s_selected_item >= layout->item_count) return;

    layout->items[s_selected_item].rotation =
        (layout->items[s_selected_item].rotation + 1) & 0x07;
    s_dirty = true;

    ESP_LOGI(TAG, "Rotated item %d to %d", s_selected_item,
             layout->items[s_selected_item].rotation);

    builder_refresh_canvas();
}

static void mirror_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_selected_item < 0) return;

    panel_layout_t *layout = panel_layout_get();
    if ((size_t)s_selected_item >= layout->item_count) return;

    layout->items[s_selected_item].mirrored =
        !layout->items[s_selected_item].mirrored;
    s_dirty = true;

    ESP_LOGI(TAG, "Mirrored item %d: %s", s_selected_item,
             layout->items[s_selected_item].mirrored ? "yes" : "no");

    builder_refresh_canvas();
}

static void delete_item_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    panel_layout_t *layout = panel_layout_get();

    // Delete selected track segment
    if (s_selected_track >= 0 && (size_t)s_selected_track < layout->track_count) {
        panel_layout_remove_track(layout, (size_t)s_selected_track);
        s_dirty = true;
        s_selected_track = -1;
        builder_refresh_canvas();
        builder_refresh_toolbar();
        return;
    }

    // Delete selected endpoint (cascade removes connected tracks)
    if (s_selected_endpoint >= 0 && (size_t)s_selected_endpoint < layout->endpoint_count) {
        panel_layout_remove_endpoint(layout, (size_t)s_selected_endpoint);
        s_dirty = true;
        s_selected_endpoint = -1;
        builder_refresh_canvas();
        builder_refresh_toolbar();
        return;
    }

    // Delete selected turnout item (cascade removes connected tracks)
    if (s_selected_item < 0) return;
    if ((size_t)s_selected_item >= layout->item_count) return;

    panel_layout_remove_item(layout, (size_t)s_selected_item);

    s_dirty = true;
    s_selected_item = -1;
    s_selected_track = -1;
    s_selected_endpoint = -1;
    builder_refresh_canvas();
    builder_refresh_toolbar();
}

static void toggle_draw_track_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    s_draw_track_mode = !s_draw_track_mode;
    s_track_first_selected = false;
    s_placement_mode = false;
    s_placement_endpoint_mode = false;
    s_selected_track = -1;

    ESP_LOGI(TAG, "Draw track mode: %s", s_draw_track_mode ? "ON" : "OFF");

    builder_refresh_toolbar();
    builder_refresh_canvas();
}

static void add_turnout_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    // Exit other modes
    s_draw_track_mode = false;
    s_track_first_selected = false;
    s_placement_endpoint_mode = false;

    open_turnout_modal();
    builder_refresh_toolbar();
}

/**
 * @brief Timer callback to restore save button text after flash
 */
static void save_flash_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_save_label) {
        lv_label_set_text(s_save_label, LV_SYMBOL_SAVE " Save");
    }
    if (s_btn_save) {
        lv_obj_set_style_bg_color(s_btn_save, lv_color_hex(COLOR_BTN_SAVE), LV_PART_MAIN);
    }
    builder_refresh_toolbar();
    s_save_flash_timer = NULL;
}

static void save_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_dirty) return;  // Nothing to save

    panel_layout_t *layout = panel_layout_get();
    esp_err_t ret = panel_storage_save(layout);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Panel layout saved");
        s_dirty = false;

        // Brief "Saved!" flash on the button
        if (s_save_label) {
            lv_label_set_text(s_save_label, LV_SYMBOL_OK " Saved!");
        }
        if (s_btn_save) {
            lv_obj_set_style_bg_color(s_btn_save, lv_color_hex(0x2E7D32), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_btn_save, LV_OPA_COVER, LV_PART_MAIN);
        }
        builder_refresh_toolbar();

        // Restore after 1.5 seconds
        if (s_save_flash_timer) {
            lv_timer_del(s_save_flash_timer);
        }
        s_save_flash_timer = lv_timer_create(save_flash_timer_cb, 1500, NULL);
        lv_timer_set_repeat_count(s_save_flash_timer, 1);
    } else {
        ESP_LOGE(TAG, "Failed to save panel layout: %s", esp_err_to_name(ret));

        // Flash red error indication
        if (s_save_label) {
            lv_label_set_text(s_save_label, LV_SYMBOL_WARNING " Error");
        }
        if (s_btn_save) {
            lv_obj_set_style_bg_color(s_btn_save, lv_color_hex(COLOR_BTN_DELETE), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_btn_save, LV_OPA_COVER, LV_PART_MAIN);
        }
        if (s_save_flash_timer) {
            lv_timer_del(s_save_flash_timer);
        }
        s_save_flash_timer = lv_timer_create(save_flash_timer_cb, 2000, NULL);
        lv_timer_set_repeat_count(s_save_flash_timer, 1);
    }
}

// ============================================================================
// Zoom/Pan Button Callbacks
// ============================================================================

static void update_zoom_label(void)
{
    if (s_zoom_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", s_zoom_pct);
        lv_label_set_text(s_zoom_label, buf);
    }
}

static void zoom_in_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_zoom_pct < ZOOM_MAX) {
        // Zoom towards center of canvas
        int16_t cx = BUILDER_CANVAS_WIDTH / 2;
        int16_t cy = BUILDER_CANVAS_HEIGHT / 2;
        int16_t old_zoom = s_zoom_pct;
        s_zoom_pct += ZOOM_STEP;
        if (s_zoom_pct > ZOOM_MAX) s_zoom_pct = ZOOM_MAX;
        // Adjust pan so center stays fixed
        s_pan_x = (int16_t)(cx - (int32_t)(cx - s_pan_x) * s_zoom_pct / old_zoom);
        s_pan_y = (int16_t)(cy - (int32_t)(cy - s_pan_y) * s_zoom_pct / old_zoom);
        update_zoom_label();
        builder_refresh_canvas();
    }
}

static void zoom_out_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_zoom_pct > ZOOM_MIN) {
        int16_t cx = BUILDER_CANVAS_WIDTH / 2;
        int16_t cy = BUILDER_CANVAS_HEIGHT / 2;
        int16_t old_zoom = s_zoom_pct;
        s_zoom_pct -= ZOOM_STEP;
        if (s_zoom_pct < ZOOM_MIN) s_zoom_pct = ZOOM_MIN;
        s_pan_x = (int16_t)(cx - (int32_t)(cx - s_pan_x) * s_zoom_pct / old_zoom);
        s_pan_y = (int16_t)(cy - (int32_t)(cy - s_pan_y) * s_zoom_pct / old_zoom);
        update_zoom_label();
        builder_refresh_canvas();
    }
}

static void zoom_reset_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_zoom_pct = ZOOM_DEFAULT;
    s_pan_x = 0;
    s_pan_y = 0;
    update_zoom_label();
    builder_refresh_canvas();
}

static void pan_left_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_pan_x += PAN_STEP;
    builder_refresh_canvas();
}

static void pan_right_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_pan_x -= PAN_STEP;
    builder_refresh_canvas();
}

static void pan_up_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_pan_y += PAN_STEP;
    builder_refresh_canvas();
}

static void pan_down_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_pan_y -= PAN_STEP;
    builder_refresh_canvas();
}

static void auto_center_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    panel_layout_t *layout = panel_layout_get();

    // Use shared bounds calculation
    int16_t min_x, min_y, max_x, max_y;
    if (!panel_layout_get_bounds(layout, 40, &min_x, &min_y, &max_x, &max_y)) {
        s_zoom_pct = ZOOM_DEFAULT;
        s_pan_x = 0;
        s_pan_y = 0;
        update_zoom_label();
        builder_refresh_canvas();
        return;
    }

    int16_t world_w = max_x - min_x;
    int16_t world_h = max_y - min_y;
    if (world_w < 1) world_w = 1;
    if (world_h < 1) world_h = 1;

    // Compute zoom to fit
    int16_t zoom_x = (int16_t)((int32_t)BUILDER_CANVAS_WIDTH * 100 / world_w);
    int16_t zoom_y = (int16_t)((int32_t)BUILDER_CANVAS_HEIGHT * 100 / world_h);
    s_zoom_pct = zoom_x < zoom_y ? zoom_x : zoom_y;
    if (s_zoom_pct < ZOOM_MIN) s_zoom_pct = ZOOM_MIN;
    if (s_zoom_pct > ZOOM_MAX) s_zoom_pct = ZOOM_MAX;

    // Center the bounding box in the canvas
    int16_t world_cx = (min_x + max_x) / 2;
    int16_t world_cy = (min_y + max_y) / 2;
    s_pan_x = (int16_t)(BUILDER_CANVAS_WIDTH / 2 - (int32_t)world_cx * s_zoom_pct / 100);
    s_pan_y = (int16_t)(BUILDER_CANVAS_HEIGHT / 2 - (int32_t)world_cy * s_zoom_pct / 100);

    update_zoom_label();
    builder_refresh_canvas();
    ESP_LOGI(TAG, "Auto center: zoom=%d%% pan=(%d,%d)", s_zoom_pct, s_pan_x, s_pan_y);
}

// ============================================================================
// Canvas Rendering
// ============================================================================

static void builder_clear_canvas(void)
{
    if (!s_canvas) return;

    if (s_drag_active_hitbox) {
        /* Reparent the in-flight drag hitbox so lv_obj_clean() won't destroy it */
        lv_obj_set_parent(s_drag_active_hitbox, lv_layer_top());
        lv_obj_add_flag(s_drag_active_hitbox, LV_OBJ_FLAG_HIDDEN);
    }

    /* Bulk-delete all canvas children (much faster than per-object lv_obj_del) */
    lv_obj_clean(s_canvas);

    if (s_drag_active_hitbox) {
        /* Move the hitbox back — builder_refresh_canvas() will reposition it */
        lv_obj_set_parent(s_drag_active_hitbox, s_canvas);
        lv_obj_clear_flag(s_drag_active_hitbox, LV_OBJ_FLAG_HIDDEN);
    }

    s_preview_obj_count = 0;
}

static void add_preview_obj(lv_obj_t *obj)
{
    if (s_preview_obj_count < sizeof(s_preview_objs) / sizeof(s_preview_objs[0])) {
        s_preview_objs[s_preview_obj_count++] = obj;
    }
}

static void builder_refresh_canvas(void)
{
    if (!s_canvas) return;

    builder_clear_canvas();

    panel_layout_t *layout = panel_layout_get();
    size_t line_idx = 0;

    // Compute scaled line widths
    int16_t lw_normal = (int16_t)((int32_t)3 * s_zoom_pct / 100);
    int16_t lw_selected = (int16_t)((int32_t)5 * s_zoom_pct / 100);
    int16_t lw_track = (int16_t)((int32_t)3 * s_zoom_pct / 100);
    if (lw_normal < 1) lw_normal = 1;
    if (lw_selected < 2) lw_selected = 2;
    if (lw_track < 1) lw_track = 1;

    // Compute scaled sizes
    int16_t hb_w = (int16_t)((int32_t)PLACED_HITBOX_W * s_zoom_pct / 100);
    int16_t hb_h = (int16_t)((int32_t)PLACED_HITBOX_H * s_zoom_pct / 100);
    if (hb_w < 20) hb_w = 20;
    if (hb_h < 16) hb_h = 16;

    // Snapshot turnout names under a single lock (#2: batch lookups)
    static char s_tn_names[PANEL_MAX_ITEMS][32];
    static bool s_tn_found[PANEL_MAX_ITEMS];
    {
        turnout_manager_lock();
        const turnout_t *turnouts;
        size_t turnout_count;
        turnout_manager_get_all(&turnouts, &turnout_count);
        for (size_t i = 0; i < layout->item_count && i < PANEL_MAX_ITEMS; i++) {
            s_tn_found[i] = false;
            for (size_t j = 0; j < turnout_count; j++) {
                if (turnouts[j].id == layout->items[i].turnout_id) {
                    memcpy(s_tn_names[i], turnouts[j].name, sizeof(turnouts[j].name));
                    s_tn_found[i] = true;
                    break;
                }
            }
        }
        turnout_manager_unlock();
    }

    // Draw placed items
    for (size_t i = 0; i < layout->item_count && i < PANEL_MAX_ITEMS; i++) {
        const panel_item_t *pi = &layout->items[i];

        // Get world-space points then transform to view-space
        lv_point_t w_entry, w_normal, w_reverse;
        panel_geometry_get_points(pi, &w_entry, &w_normal, &w_reverse);

        lv_point_t entry = world_to_view_pt(&w_entry);
        lv_point_t normal_pt = world_to_view_pt(&w_normal);
        lv_point_t reverse_pt = world_to_view_pt(&w_reverse);

        bool selected = ((int)i == s_selected_item);
        lv_color_t line_color = selected ? lv_color_hex(COLOR_SELECTED)
                                         : lv_color_hex(COLOR_NORMAL_LINE);

        // Entry -> Normal line
        if (line_idx < BUILDER_MAX_LINES) {
            s_preview_line_pts[line_idx][0] = entry;
            s_preview_line_pts[line_idx][1] = normal_pt;

            lv_obj_t *line = lv_line_create(s_canvas);
            lv_line_set_points(line, s_preview_line_pts[line_idx], 2);
            lv_obj_set_style_line_width(line, selected ? lw_selected : lw_normal, LV_PART_MAIN);
            lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
            lv_obj_set_style_line_color(line, line_color, LV_PART_MAIN);
            add_preview_obj(line);
            line_idx++;
        }

        // Entry -> Reverse line
        if (line_idx < BUILDER_MAX_LINES) {
            s_preview_line_pts[line_idx][0] = entry;
            s_preview_line_pts[line_idx][1] = reverse_pt;

            lv_obj_t *line = lv_line_create(s_canvas);
            lv_line_set_points(line, s_preview_line_pts[line_idx], 2);
            lv_obj_set_style_line_width(line, selected ? lw_selected : lw_normal, LV_PART_MAIN);
            lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
            lv_obj_set_style_line_color(line, line_color, LV_PART_MAIN);
            add_preview_obj(line);
            line_idx++;
        }

        // Clickable/draggable hitbox for this item (view-space)
        int16_t w_cx, w_cy;
        panel_geometry_get_center(pi, &w_cx, &w_cy);
        int16_t vcx = world_to_view_x(w_cx);
        int16_t vcy = world_to_view_y(w_cy);

        // If this item is being actively dragged, reuse the existing hitbox
        if (s_drag_active_hitbox && !s_drag_active_is_endpoint && s_drag_active_idx == (int)i) {
            lv_obj_set_size(s_drag_active_hitbox, hb_w, hb_h);
            lv_obj_set_pos(s_drag_active_hitbox, vcx - hb_w / 2, vcy - hb_h / 2);
            add_preview_obj(s_drag_active_hitbox);
        } else {
            lv_obj_t *hitbox = lv_obj_create(s_canvas);
            lv_obj_remove_style_all(hitbox);
            lv_obj_set_size(hitbox, hb_w, hb_h);
            lv_obj_set_pos(hitbox, vcx - hb_w / 2, vcy - hb_h / 2);
            lv_obj_add_flag(hitbox, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(hitbox, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(hitbox, placed_item_click_cb, LV_EVENT_CLICKED,
                               (void *)(uintptr_t)i);
            lv_obj_add_event_cb(hitbox, placed_item_drag_cb, LV_EVENT_PRESSING,
                               (void *)(uintptr_t)i);
            lv_obj_add_event_cb(hitbox, placed_item_release_cb, LV_EVENT_RELEASED,
                               (void *)(uintptr_t)i);
            lv_obj_add_event_cb(hitbox, placed_item_release_cb, LV_EVENT_PRESS_LOST,
                               (void *)(uintptr_t)i);
            add_preview_obj(hitbox);
        }

        // Connection point indicators (visible when selected or in track draw mode)
        if (selected || s_draw_track_mode) {
            lv_point_t pts[3] = { entry, normal_pt, reverse_pt };
            panel_point_type_t pt_types[3] = {
                PANEL_POINT_ENTRY, PANEL_POINT_NORMAL, PANEL_POINT_REVERSE
            };
            lv_color_t pt_colors[3] = {
                lv_color_hex(0xFFFFFF),    // Entry: white
                lv_color_hex(0x4CAF50),    // Normal: green
                lv_color_hex(0xFFC107),    // Reverse: amber
            };

            int base_dot = s_draw_track_mode ? 20 : 12;
            int dot_size = (int)((int32_t)base_dot * s_zoom_pct / 100);
            if (dot_size < 10) dot_size = 10;

            for (int p = 0; p < 3; p++) {
                int ds = dot_size;
                lv_obj_t *dot = lv_obj_create(s_canvas);
                lv_obj_remove_style_all(dot);
                lv_obj_set_style_bg_color(dot, pt_colors[p], LV_PART_MAIN);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
                lv_obj_set_style_border_width(dot, 1, LV_PART_MAIN);
                lv_obj_set_style_border_color(dot, lv_color_hex(0x000000), LV_PART_MAIN);

                // Highlight the first-selected point in track draw mode
                if (s_draw_track_mode && s_track_first_selected &&
                    s_track_from_ref.type == PANEL_REF_TURNOUT &&
                    pi->turnout_id == s_track_from_ref.id &&
                    pt_types[p] == s_track_from_ref.point) {
                    lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_CONN_ACTIVE), LV_PART_MAIN);
                    ds = ds + ds / 3;  // ~33% larger for highlight
                }

                lv_obj_set_size(dot, ds, ds);
                lv_obj_set_pos(dot, pts[p].x - ds / 2, pts[p].y - ds / 2);

                if (s_draw_track_mode) {
                    lv_obj_add_flag(dot, LV_OBJ_FLAG_CLICKABLE);
                    uint32_t packed = ((uint32_t)i << 8) | (uint32_t)pt_types[p];
                    lv_obj_add_event_cb(dot, conn_point_click_cb, LV_EVENT_CLICKED,
                                       (void *)(uintptr_t)packed);
                }

                add_preview_obj(dot);
            }
        }

        // Turnout name label (small, above the symbol)
        if (s_tn_found[i]) {
            lv_obj_t *name_lbl = lv_label_create(s_canvas);
            lv_label_set_text(name_lbl, s_tn_names[i]);
            lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_12, LV_PART_MAIN);
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(0xBBBBBB), LV_PART_MAIN);
            lv_obj_set_pos(name_lbl, vcx - 30, vcy - hb_h / 2 - 14);
            add_preview_obj(name_lbl);
        }
    }

    // Draw track segments BEFORE endpoints so that endpoint/turnout hitboxes
    // have higher z-order and receive touch priority over track hitboxes.
    for (size_t i = 0; i < layout->track_count && i < PANEL_MAX_TRACKS; i++) {
        const panel_track_t *pt = &layout->tracks[i];

        // Resolve via shared layout operation
        int16_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (!panel_layout_resolve_track(layout, pt, &x1, &y1, &x2, &y2)) continue;

        bool track_selected = ((int)i == s_selected_track);

        if (line_idx < BUILDER_MAX_LINES + PANEL_MAX_TRACKS) {
            // Transform world-space track endpoints to view-space
            int16_t vx1 = world_to_view_x(x1);
            int16_t vy1 = world_to_view_y(y1);
            int16_t vx2 = world_to_view_x(x2);
            int16_t vy2 = world_to_view_y(y2);

            s_preview_line_pts[line_idx][0].x = vx1;
            s_preview_line_pts[line_idx][0].y = vy1;
            s_preview_line_pts[line_idx][1].x = vx2;
            s_preview_line_pts[line_idx][1].y = vy2;

            lv_color_t track_color = track_selected
                ? lv_color_hex(COLOR_SELECTED)
                : lv_color_hex(COLOR_TRACK_DRAW);
            int16_t tw = track_selected ? lw_track + 2 : lw_track;

            lv_obj_t *track_line = lv_line_create(s_canvas);
            lv_line_set_points(track_line, s_preview_line_pts[line_idx], 2);
            lv_obj_set_style_line_width(track_line, tw, LV_PART_MAIN);
            lv_obj_set_style_line_rounded(track_line, true, LV_PART_MAIN);
            lv_obj_set_style_line_color(track_line, track_color, LV_PART_MAIN);
            add_preview_obj(track_line);
            line_idx++;

            // Invisible clickable hitbox along the track segment for selection.
            // Compute an axis-aligned bounding box around the line with padding.
            if (!s_draw_track_mode) {
                int16_t pad = 12;  // tap padding in pixels
                int16_t hx = (vx1 < vx2 ? vx1 : vx2) - pad;
                int16_t hy = (vy1 < vy2 ? vy1 : vy2) - pad;
                int16_t hw = (vx1 > vx2 ? vx1 - vx2 : vx2 - vx1) + pad * 2;
                int16_t hh = (vy1 > vy2 ? vy1 - vy2 : vy2 - vy1) + pad * 2;
                if (hw < 24) { hx -= (24 - hw) / 2; hw = 24; }
                if (hh < 24) { hy -= (24 - hh) / 2; hh = 24; }

                lv_obj_t *track_hb = lv_obj_create(s_canvas);
                lv_obj_remove_style_all(track_hb);
                lv_obj_set_size(track_hb, hw, hh);
                lv_obj_set_pos(track_hb, hx, hy);
                lv_obj_add_flag(track_hb, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_clear_flag(track_hb, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_event_cb(track_hb, track_click_cb, LV_EVENT_CLICKED,
                                   (void *)(uintptr_t)i);
                add_preview_obj(track_hb);
            }
        }
    }

    // Draw endpoints AFTER tracks so their hitboxes have higher z-priority
    for (size_t i = 0; i < layout->endpoint_count && i < PANEL_MAX_ENDPOINTS; i++) {
        const panel_endpoint_t *ep = &layout->endpoints[i];

        int16_t wx = (int16_t)(ep->grid_x * PANEL_GRID_SIZE);
        int16_t wy = (int16_t)(ep->grid_y * PANEL_GRID_SIZE);
        int16_t vx = world_to_view_x(wx);
        int16_t vy = world_to_view_y(wy);

        bool ep_selected = ((int)i == s_selected_endpoint);

        // Dot size scales with zoom; larger in draw mode for easier tapping
        int base_dot = s_draw_track_mode ? 20 : 14;
        int dot_sz = (int)((int32_t)base_dot * s_zoom_pct / 100);
        if (dot_sz < 8) dot_sz = 8;

        lv_color_t dot_color = ep_selected ? lv_color_hex(COLOR_SELECTED)
                                           : lv_color_hex(COLOR_ENDPOINT);

        // Highlight the first-selected endpoint in track draw mode
        if (s_draw_track_mode && s_track_first_selected &&
            s_track_from_ref.type == PANEL_REF_ENDPOINT && s_track_from_ref.id == ep->id) {
            dot_color = lv_color_hex(COLOR_CONN_ACTIVE);
            dot_sz = dot_sz + dot_sz / 3;
        }

        lv_obj_t *dot = lv_obj_create(s_canvas);
        lv_obj_remove_style_all(dot);
        lv_obj_set_style_bg_color(dot, dot_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(dot,
            ep_selected ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_size(dot, dot_sz, dot_sz);
        lv_obj_set_pos(dot, vx - dot_sz / 2, vy - dot_sz / 2);
        add_preview_obj(dot);

        // Hitbox for click/drag
        int16_t ep_hb = (int16_t)((int32_t)30 * s_zoom_pct / 100);
        if (ep_hb < 20) ep_hb = 20;

        if (s_drag_active_hitbox && s_drag_active_is_endpoint && s_drag_active_idx == (int)i) {
            lv_obj_set_size(s_drag_active_hitbox, ep_hb, ep_hb);
            lv_obj_set_pos(s_drag_active_hitbox, vx - ep_hb / 2, vy - ep_hb / 2);
            add_preview_obj(s_drag_active_hitbox);
        } else {
            lv_obj_t *hitbox = lv_obj_create(s_canvas);
            lv_obj_remove_style_all(hitbox);
            lv_obj_set_size(hitbox, ep_hb, ep_hb);
            lv_obj_set_pos(hitbox, vx - ep_hb / 2, vy - ep_hb / 2);
            lv_obj_add_flag(hitbox, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(hitbox, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(hitbox, placed_endpoint_click_cb, LV_EVENT_CLICKED,
                               (void *)(uintptr_t)i);
            lv_obj_add_event_cb(hitbox, placed_endpoint_drag_cb, LV_EVENT_PRESSING,
                               (void *)(uintptr_t)i);
            lv_obj_add_event_cb(hitbox, placed_endpoint_release_cb, LV_EVENT_RELEASED,
                               (void *)(uintptr_t)i);
            lv_obj_add_event_cb(hitbox, placed_endpoint_release_cb, LV_EVENT_PRESS_LOST,
                               (void *)(uintptr_t)i);
            add_preview_obj(hitbox);
        }
    }

    // Show placement/mode hints
    if (s_placement_mode) {
        lv_obj_t *hint = lv_label_create(s_canvas);
        lv_label_set_text(hint, "Tap canvas to place turnout");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_BTN_ADD), LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
        add_preview_obj(hint);
    }
    if (s_placement_endpoint_mode) {
        lv_obj_t *hint = lv_label_create(s_canvas);
        lv_label_set_text(hint, "Tap canvas to place endpoint");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_ENDPOINT), LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
        add_preview_obj(hint);
    }
    if (s_draw_track_mode) {
        lv_obj_t *hint = lv_label_create(s_canvas);
        if (s_track_first_selected) {
            lv_label_set_text(hint, "Now tap a second point to complete the track");
        } else {
            lv_label_set_text(hint, "Tap a connection point on a turnout or endpoint to start a track");
        }
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_CONN_ACTIVE), LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
        add_preview_obj(hint);
    }
}

// ============================================================================
// Toolbar State
// ============================================================================

static void builder_refresh_toolbar(void)
{
    bool has_item = (s_selected_item >= 0);
    bool has_track = (s_selected_track >= 0);
    bool has_endpoint = (s_selected_endpoint >= 0);
    bool has_any = has_item || has_track || has_endpoint;

    if (s_btn_rotate) {
        // Rotate only applies to turnouts
        lv_obj_set_style_bg_opa(s_btn_rotate,
            has_item ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    }
    if (s_btn_mirror) {
        // Mirror only applies to turnouts
        lv_obj_set_style_bg_opa(s_btn_mirror,
            has_item ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    }
    if (s_btn_delete) {
        // Delete applies to turnouts, tracks, and endpoints
        lv_obj_set_style_bg_opa(s_btn_delete,
            has_any ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    }
    if (s_btn_draw_track) {
        lv_obj_set_style_bg_color(s_btn_draw_track,
            s_draw_track_mode ? lv_color_hex(0xFF5722) : lv_color_hex(COLOR_BTN_TRACK),
            LV_PART_MAIN);
    }
    if (s_btn_add_turnout) {
        lv_obj_set_style_bg_color(s_btn_add_turnout,
            s_placement_mode ? lv_color_hex(0x00796B) : lv_color_hex(COLOR_BTN_ADD),
            LV_PART_MAIN);
    }
    if (s_btn_add_endpoint) {
        lv_obj_set_style_bg_color(s_btn_add_endpoint,
            s_placement_endpoint_mode ? lv_color_hex(0x0277BD) : lv_color_hex(COLOR_ENDPOINT),
            LV_PART_MAIN);
    }
    if (s_btn_save && !s_save_flash_timer) {
        // Only update save appearance if not mid-flash
        lv_obj_set_style_bg_opa(s_btn_save,
            s_dirty ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_btn_save,
            lv_color_hex(COLOR_BTN_SAVE), LV_PART_MAIN);
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_create_panel_builder_tab(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Creating panel builder tab");

    s_builder_parent = parent;
    s_selected_item = -1;
    s_selected_track = -1;
    s_selected_endpoint = -1;
    s_placement_mode = false;
    s_placement_endpoint_mode = false;
    s_draw_track_mode = false;
    s_track_first_selected = false;
    s_preview_obj_count = 0;
    s_modal_overlay = NULL;
    s_dirty = false;
    s_save_label = NULL;
    s_save_flash_timer = NULL;

    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Container: toolbar + canvas (full width) ----
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Toolbar ----
    lv_obj_t *toolbar = lv_obj_create(container);
    lv_obj_remove_style_all(toolbar);
    lv_obj_set_size(toolbar, lv_pct(100), BUILDER_TOOLBAR_HEIGHT);
    lv_obj_set_pos(toolbar, 0, 0);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(0x424242), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(toolbar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(toolbar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(toolbar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(toolbar, 6, LV_PART_MAIN);

    // + Turnout button
    s_btn_add_turnout = lv_btn_create(toolbar);
    lv_obj_set_size(s_btn_add_turnout, 110, 38);
    lv_obj_set_style_bg_color(s_btn_add_turnout, lv_color_hex(COLOR_BTN_ADD), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_add_turnout, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_add_turnout, add_turnout_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *add_lbl = lv_label_create(s_btn_add_turnout);
    lv_label_set_text(add_lbl, LV_SYMBOL_PLUS " Turnout");
    lv_obj_set_style_text_font(add_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(add_lbl);

    // + Endpoint button
    s_btn_add_endpoint = lv_btn_create(toolbar);
    lv_obj_set_size(s_btn_add_endpoint, 110, 38);
    lv_obj_set_style_bg_color(s_btn_add_endpoint, lv_color_hex(COLOR_ENDPOINT), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_add_endpoint, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_add_endpoint, add_endpoint_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ep_lbl = lv_label_create(s_btn_add_endpoint);
    lv_label_set_text(ep_lbl, LV_SYMBOL_PLUS " Endpoint");
    lv_obj_set_style_text_font(ep_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(ep_lbl);

    // Rotate button
    s_btn_rotate = lv_btn_create(toolbar);
    lv_obj_set_size(s_btn_rotate, 78, 38);
    lv_obj_set_style_bg_color(s_btn_rotate, lv_color_hex(COLOR_BTN_ROTATE), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_rotate, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_rotate, rotate_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rot_lbl = lv_label_create(s_btn_rotate);
    lv_label_set_text(rot_lbl, LV_SYMBOL_REFRESH " Rotate");
    lv_obj_set_style_text_font(rot_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(rot_lbl);

    // Mirror button
    s_btn_mirror = lv_btn_create(toolbar);
    lv_obj_set_size(s_btn_mirror, 78, 38);
    lv_obj_set_style_bg_color(s_btn_mirror, lv_color_hex(COLOR_BTN_MIRROR), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_mirror, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_mirror, mirror_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mir_lbl = lv_label_create(s_btn_mirror);
    lv_label_set_text(mir_lbl, LV_SYMBOL_SHUFFLE " Flip");
    lv_obj_set_style_text_font(mir_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(mir_lbl);

    // Delete button
    s_btn_delete = lv_btn_create(toolbar);
    lv_obj_set_size(s_btn_delete, 56, 38);
    lv_obj_set_style_bg_color(s_btn_delete, lv_color_hex(COLOR_BTN_DELETE), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_delete, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_delete, delete_item_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_lbl = lv_label_create(s_btn_delete);
    lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_font(del_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(del_lbl);

    // Draw Track toggle
    s_btn_draw_track = lv_btn_create(toolbar);
    lv_obj_set_size(s_btn_draw_track, 90, 38);
    lv_obj_set_style_bg_color(s_btn_draw_track, lv_color_hex(COLOR_BTN_TRACK), LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_draw_track, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_draw_track, toggle_draw_track_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *track_lbl = lv_label_create(s_btn_draw_track);
    lv_label_set_text(track_lbl, LV_SYMBOL_EDIT " Track");
    lv_obj_set_style_text_font(track_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(track_lbl);

    // Save button (moved from sidebar to toolbar)
    s_btn_save = lv_btn_create(toolbar);
    lv_obj_set_size(s_btn_save, 90, 38);
    lv_obj_set_style_bg_color(s_btn_save, lv_color_hex(COLOR_BTN_SAVE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_btn_save, LV_OPA_50, LV_PART_MAIN);  // Start dimmed (no unsaved changes)
    lv_obj_set_style_radius(s_btn_save, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_save, save_cb, LV_EVENT_CLICKED, NULL);
    s_save_label = lv_label_create(s_btn_save);
    lv_label_set_text(s_save_label, LV_SYMBOL_SAVE " Save");
    lv_obj_set_style_text_font(s_save_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(s_save_label);

    // ---- Canvas ----
    s_canvas = lv_obj_create(container);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_size(s_canvas, BUILDER_CANVAS_WIDTH, BUILDER_CANVAS_HEIGHT);
    lv_obj_set_pos(s_canvas, 0, BUILDER_TOOLBAR_HEIGHT);
    lv_obj_set_style_bg_color(s_canvas, lv_color_hex(COLOR_CANVAS_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_canvas, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, canvas_click_cb, LV_EVENT_CLICKED, NULL);

    // ---- Navigation bar (right side, vertical) ----
    lv_obj_t *nav_bar = lv_obj_create(container);
    lv_obj_remove_style_all(nav_bar);
    lv_obj_set_size(nav_bar, BUILDER_NAV_WIDTH, BUILDER_CANVAS_HEIGHT);
    lv_obj_set_pos(nav_bar, BUILDER_CANVAS_WIDTH, BUILDER_TOOLBAR_HEIGHT);
    lv_obj_set_style_bg_color(nav_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nav_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(nav_bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(nav_bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nav_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(nav_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(nav_bar, 2, LV_PART_MAIN);

    // Helper macro to create a small nav button
    #define NAV_BTN(parent, text, cb_fn, w, h) do { \
        lv_obj_t *_btn = lv_btn_create(parent); \
        lv_obj_set_size(_btn, w, h); \
        lv_obj_set_style_bg_color(_btn, lv_color_hex(0x555555), LV_PART_MAIN); \
        lv_obj_set_style_radius(_btn, 3, LV_PART_MAIN); \
        lv_obj_set_style_pad_all(_btn, 0, LV_PART_MAIN); \
        lv_obj_add_event_cb(_btn, cb_fn, LV_EVENT_CLICKED, NULL); \
        lv_obj_t *_lbl = lv_label_create(_btn); \
        lv_label_set_text(_lbl, text); \
        lv_obj_set_style_text_font(_lbl, &lv_font_montserrat_14, LV_PART_MAIN); \
        lv_obj_center(_lbl); \
    } while(0)

    NAV_BTN(nav_bar, "+",                 zoom_in_cb,    46, 36);
    NAV_BTN(nav_bar, "-",                 zoom_out_cb,   46, 36);

    // Zoom label
    s_zoom_label = lv_label_create(nav_bar);
    lv_label_set_text(s_zoom_label, "100%");
    lv_obj_set_style_text_font(s_zoom_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_zoom_label, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_zoom_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    NAV_BTN(nav_bar, LV_SYMBOL_REFRESH,   zoom_reset_cb, 46, 36);

    // Spacer pushes navigation buttons to the bottom of the sidebar
    lv_obj_t *spacer = lv_obj_create(nav_bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    NAV_BTN(nav_bar, LV_SYMBOL_UP,        pan_up_cb,     46, 36);
    NAV_BTN(nav_bar, LV_SYMBOL_DOWN,      pan_down_cb,   46, 36);
    NAV_BTN(nav_bar, LV_SYMBOL_LEFT,      pan_left_cb,   46, 36);
    NAV_BTN(nav_bar, LV_SYMBOL_RIGHT,     pan_right_cb,  46, 36);

    // Auto-center button (fit all)
    NAV_BTN(nav_bar, LV_SYMBOL_HOME,       auto_center_cb, 46, 36);

    #undef NAV_BTN

    // Initial state
    builder_refresh_canvas();
    builder_refresh_toolbar();

    ESP_LOGI(TAG, "Panel builder tab created (full-width canvas, modal turnout selection)");
}

void ui_panel_builder_refresh(void)
{
    builder_refresh_canvas();
}
