/**
 * @file panel_layout.c
 * @brief Panel layout data model — singleton owner and operations
 *
 * Owns the global panel_layout_t singleton and provides all mutation/query
 * operations.  Keeps layout manipulation logic in one place, used by both
 * the live panel screen and the panel builder editor.
 *
 * Key responsibilities:
 *   - Singleton ownership of panel_layout_t
 *   - Add / remove items, endpoints, tracks (with cascade delete)
 *   - Resolve track segments to pixel coordinates
 *   - Compute layout bounding box for auto-fit
 *   - Turnout-placed queries
 */

#include "panel_layout.h"
#include "panel_geometry.h"   /* turnout connection point resolution (uses lv_point_t) */
#include "esp_log.h"
#include <string.h>

static const char *TAG = "panel_layout";

// ============================================================================
// Singleton
// ============================================================================

static panel_layout_t s_layout;

panel_layout_t* panel_layout_get(void)
{
    return &s_layout;
}

// ============================================================================
// Query Operations
// ============================================================================

bool panel_layout_is_empty(const panel_layout_t *layout)
{
    return layout->item_count == 0 && layout->endpoint_count == 0;
}

bool panel_layout_is_turnout_placed(const panel_layout_t *layout,
                                     uint32_t turnout_id)
{
    for (size_t i = 0; i < layout->item_count; i++) {
        if (layout->items[i].turnout_id == turnout_id) return true;
    }
    return false;
}

int panel_layout_find_item(const panel_layout_t *layout, uint32_t turnout_id)
{
    for (size_t i = 0; i < layout->item_count; i++) {
        if (layout->items[i].turnout_id == turnout_id) return (int)i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Track endpoint resolution (shared by live panel + builder)
// ---------------------------------------------------------------------------

/**
 * @brief Resolve one end of a track to pixel coordinates
 * @return true if resolved
 */
static bool resolve_track_end(const panel_layout_t *layout,
                               const panel_ref_t *ref,
                               int16_t *px, int16_t *py)
{
    if (ref->type == PANEL_REF_ENDPOINT) {
        for (size_t j = 0; j < layout->endpoint_count; j++) {
            if (layout->endpoints[j].id == ref->id) {
                *px = (int16_t)(layout->endpoints[j].grid_x * PANEL_GRID_SIZE);
                *py = (int16_t)(layout->endpoints[j].grid_y * PANEL_GRID_SIZE);
                return true;
            }
        }
        return false;
    }

    /* Turnout connection point — check last-hit hint first (#6: avoid full scan) */
    static size_t s_last_hit = 0;
    if (s_last_hit < layout->item_count &&
        layout->items[s_last_hit].turnout_id == ref->id) {
        panel_geometry_get_connection_point(&layout->items[s_last_hit], ref->point, px, py);
        return true;
    }

    for (size_t j = 0; j < layout->item_count; j++) {
        if (layout->items[j].turnout_id == ref->id) {
            s_last_hit = j;
            panel_geometry_get_connection_point(&layout->items[j], ref->point, px, py);
            return true;
        }
    }
    return false;
}

bool panel_layout_resolve_track(const panel_layout_t *layout,
                                 const panel_track_t *track,
                                 int16_t *x1, int16_t *y1,
                                 int16_t *x2, int16_t *y2)
{
    bool from_ok = resolve_track_end(layout, &track->from, x1, y1);
    bool to_ok   = resolve_track_end(layout, &track->to,   x2, y2);
    return from_ok && to_ok;
}

// ---------------------------------------------------------------------------
// Bounding box (used by builder auto-center)
// ---------------------------------------------------------------------------

bool panel_layout_get_bounds(const panel_layout_t *layout,
                              int16_t margin,
                              int16_t *out_min_x, int16_t *out_min_y,
                              int16_t *out_max_x, int16_t *out_max_y)
{
    if (panel_layout_is_empty(layout)) return false;

    int16_t min_x = INT16_MAX, min_y = INT16_MAX;
    int16_t max_x = INT16_MIN, max_y = INT16_MIN;

    /* Include all turnout symbol points */
    for (size_t i = 0; i < layout->item_count; i++) {
        lv_point_t entry, normal_pt, reverse_pt;
        panel_geometry_get_points(&layout->items[i], &entry, &normal_pt, &reverse_pt);
        int16_t xs[3] = { (int16_t)entry.x, (int16_t)normal_pt.x, (int16_t)reverse_pt.x };
        int16_t ys[3] = { (int16_t)entry.y, (int16_t)normal_pt.y, (int16_t)reverse_pt.y };
        for (int p = 0; p < 3; p++) {
            if (xs[p] < min_x) min_x = xs[p];
            if (xs[p] > max_x) max_x = xs[p];
            if (ys[p] < min_y) min_y = ys[p];
            if (ys[p] > max_y) max_y = ys[p];
        }
    }

    /* Include all endpoint positions */
    for (size_t i = 0; i < layout->endpoint_count; i++) {
        int16_t wx = (int16_t)(layout->endpoints[i].grid_x * PANEL_GRID_SIZE);
        int16_t wy = (int16_t)(layout->endpoints[i].grid_y * PANEL_GRID_SIZE);
        if (wx < min_x) min_x = wx;
        if (wx > max_x) max_x = wx;
        if (wy < min_y) min_y = wy;
        if (wy > max_y) max_y = wy;
    }

    *out_min_x = min_x - margin;
    *out_min_y = min_y - margin;
    *out_max_x = max_x + margin;
    *out_max_y = max_y + margin;
    return true;
}

// ============================================================================
// Mutation Operations
// ============================================================================

int panel_layout_add_item(panel_layout_t *layout, uint32_t turnout_id,
                           uint16_t grid_x, uint16_t grid_y)
{
    if (layout->item_count >= PANEL_MAX_ITEMS) {
        ESP_LOGW(TAG, "Layout full — cannot add more items (max %d)", PANEL_MAX_ITEMS);
        return -1;
    }

    size_t idx = layout->item_count;
    panel_item_t *pi = &layout->items[idx];
    pi->turnout_id = turnout_id;
    pi->grid_x = grid_x;
    pi->grid_y = grid_y;
    pi->rotation = 0;
    pi->mirrored = false;
    layout->item_count++;

    ESP_LOGI(TAG, "Added item at grid (%d, %d), %d items total",
             grid_x, grid_y, (int)layout->item_count);
    return (int)idx;
}

bool panel_layout_add_endpoint(panel_layout_t *layout,
                                uint16_t grid_x, uint16_t grid_y,
                                size_t *out_index)
{
    if (layout->endpoint_count >= PANEL_MAX_ENDPOINTS) {
        ESP_LOGW(TAG, "Layout full — cannot add more endpoints (max %d)",
                 PANEL_MAX_ENDPOINTS);
        return false;
    }

    size_t idx = layout->endpoint_count;
    panel_endpoint_t *ep = &layout->endpoints[idx];
    ep->id = layout->next_endpoint_id++;
    ep->grid_x = grid_x;
    ep->grid_y = grid_y;
    layout->endpoint_count++;

    if (out_index) *out_index = idx;

    ESP_LOGI(TAG, "Added endpoint %u at grid (%d, %d), %d endpoints total",
             (unsigned)ep->id, grid_x, grid_y, (int)layout->endpoint_count);
    return true;
}

bool panel_layout_add_track(panel_layout_t *layout, const panel_track_t *track)
{
    if (layout->track_count >= PANEL_MAX_TRACKS) {
        ESP_LOGW(TAG, "Layout full — cannot add more tracks (max %d)", PANEL_MAX_TRACKS);
        return false;
    }

    layout->tracks[layout->track_count] = *track;
    layout->track_count++;

    ESP_LOGI(TAG, "Added track segment, %d tracks total", (int)layout->track_count);
    return true;
}

void panel_layout_remove_item(panel_layout_t *layout, size_t index)
{
    if (index >= layout->item_count) return;

    uint32_t removed_id = layout->items[index].turnout_id;

    /* Shift items down */
    for (size_t i = index; i < layout->item_count - 1; i++) {
        layout->items[i] = layout->items[i + 1];
    }
    layout->item_count--;

    /* Cascade: remove tracks referencing this turnout */
    size_t write = 0;
    for (size_t i = 0; i < layout->track_count; i++) {
        bool from_match = layout->tracks[i].from.type == PANEL_REF_TURNOUT &&
                          layout->tracks[i].from.id == removed_id;
        bool to_match   = layout->tracks[i].to.type == PANEL_REF_TURNOUT &&
                          layout->tracks[i].to.id == removed_id;
        if (!from_match && !to_match) {
            if (write != i) layout->tracks[write] = layout->tracks[i];
            write++;
        }
    }
    size_t removed_tracks = layout->track_count - write;
    layout->track_count = write;

    ESP_LOGI(TAG, "Removed item, cascade deleted %d tracks, %d items remain",
             (int)removed_tracks, (int)layout->item_count);
}

void panel_layout_remove_endpoint(panel_layout_t *layout, size_t index)
{
    if (index >= layout->endpoint_count) return;

    uint32_t removed_id = layout->endpoints[index].id;

    /* Shift endpoints down */
    for (size_t i = index; i < layout->endpoint_count - 1; i++) {
        layout->endpoints[i] = layout->endpoints[i + 1];
    }
    layout->endpoint_count--;

    /* Cascade: remove tracks referencing this endpoint */
    size_t write = 0;
    for (size_t i = 0; i < layout->track_count; i++) {
        bool refs = (layout->tracks[i].from.type == PANEL_REF_ENDPOINT &&
                     layout->tracks[i].from.id == removed_id) ||
                    (layout->tracks[i].to.type == PANEL_REF_ENDPOINT &&
                     layout->tracks[i].to.id == removed_id);
        if (!refs) {
            if (write != i) layout->tracks[write] = layout->tracks[i];
            write++;
        }
    }
    size_t removed_tracks = layout->track_count - write;
    layout->track_count = write;

    ESP_LOGI(TAG, "Removed endpoint %u, cascade deleted %d tracks, %d endpoints remain",
             (unsigned)removed_id, (int)removed_tracks, (int)layout->endpoint_count);
}

void panel_layout_remove_track(panel_layout_t *layout, size_t index)
{
    if (index >= layout->track_count) return;

    for (size_t i = index; i < layout->track_count - 1; i++) {
        layout->tracks[i] = layout->tracks[i + 1];
    }
    layout->track_count--;

    ESP_LOGI(TAG, "Removed track segment, %d tracks remain", (int)layout->track_count);
}
