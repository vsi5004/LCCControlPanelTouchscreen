/**
 * @file panel_geometry.c
 * @brief Turnout Y-shape geometry calculations
 *
 * Computes pixel positions of turnout symbol endpoints given grid position,
 * rotation (0-7 for 0°-315° in 45° steps), and mirror state.
 *
 * Base shape (rotation=0, mirrored=false) in local coordinates:
 *   Entry:   (0, 0)
 *   Normal:  (60, 0)      — colinear with entry
 *   Reverse: (40, -40)    — diverges upward at 45°, spans 2 grid spacings vertically
 *
 * The reverse exit is at approximately 45° from the entry-normal axis,
 * branching at the midpoint. cos(45°)*30 ≈ 42 offset along primary axis.
 *
 * Rotation is applied as a 2D rotation of the local offsets about the entry point.
 * Mirror flips the Y component of local offsets before rotation.
 */

#include "panel_geometry.h"
#include <string.h>

// ============================================================================
// Local coordinate offsets for the three points (relative to entry at origin)
// ============================================================================

// Normal exit: 60px to the right (colinear)
#define LOCAL_NORMAL_DX   60
#define LOCAL_NORMAL_DY   0

// Reverse exit: 45° diverging upward-right
// 40px along each axis = 2 grid spacings vertically
#define LOCAL_REVERSE_DX  40
#define LOCAL_REVERSE_DY  (-40)

// ============================================================================
// Precomputed rotation table (fixed-point, scaled by 1024)
// For 8 rotation steps: 0°, 45°, 90°, 135°, 180°, 225°, 270°, 315°
// ============================================================================

typedef struct {
    int16_t cos_fp;  // cos(angle) * 1024
    int16_t sin_fp;  // sin(angle) * 1024
} rot_entry_t;

static const rot_entry_t s_rot_table[8] = {
    { 1024,    0 },   // 0°
    {  724,  724 },   // 45°   (cos45 ≈ 0.7071 * 1024 ≈ 724)
    {    0, 1024 },   // 90°
    { -724,  724 },   // 135°
    {-1024,    0 },   // 180°
    { -724, -724 },   // 225°
    {    0,-1024 },   // 270°
    {  724, -724 },   // 315°
};

/**
 * @brief Rotate a local offset by the given rotation index
 *
 * Applies 2D rotation: x' = x*cos - y*sin, y' = x*sin + y*cos
 * Uses fixed-point arithmetic (scale 1024) to avoid floating point.
 *
 * @param dx    Local X offset
 * @param dy    Local Y offset (negative is up in screen coords)
 * @param rot   Rotation index 0-7
 * @param out_x Output rotated X
 * @param out_y Output rotated Y
 */
static void rotate_offset(int16_t dx, int16_t dy, uint8_t rot,
                          int16_t *out_x, int16_t *out_y)
{
    const rot_entry_t *r = &s_rot_table[rot & 0x07];
    // Standard 2D rotation: x' = x*cos - y*sin, y' = x*sin + y*cos
    *out_x = (int16_t)(((int32_t)dx * r->cos_fp - (int32_t)dy * r->sin_fp) / 1024);
    *out_y = (int16_t)(((int32_t)dx * r->sin_fp + (int32_t)dy * r->cos_fp) / 1024);
}

/**
 * @brief Transform a local offset by mirror + rotation, then add origin
 */
static void transform_point(int16_t local_dx, int16_t local_dy,
                            const panel_item_t *item,
                            int16_t origin_x, int16_t origin_y,
                            int16_t *out_x, int16_t *out_y)
{
    int16_t dy = local_dy;

    // Mirror: flip Y component (diverging leg goes opposite direction)
    if (item->mirrored) {
        dy = -dy;
    }

    int16_t rx, ry;
    rotate_offset(local_dx, dy, item->rotation, &rx, &ry);

    *out_x = origin_x + rx;
    *out_y = origin_y + ry;
}

// ============================================================================
// Public API
// ============================================================================

void panel_geometry_get_points(const panel_item_t *item,
                               lv_point_t *entry,
                               lv_point_t *normal,
                               lv_point_t *reverse)
{
    if (!item) return;

    // Entry is at the grid position (pixel origin)
    int16_t origin_x = (int16_t)(item->grid_x * PANEL_GRID_SIZE);
    int16_t origin_y = (int16_t)(item->grid_y * PANEL_GRID_SIZE);

    // Entry point is at the origin
    if (entry) {
        entry->x = origin_x;
        entry->y = origin_y;
    }

    // Normal exit
    if (normal) {
        int16_t nx, ny;
        transform_point(LOCAL_NORMAL_DX, LOCAL_NORMAL_DY, item,
                        origin_x, origin_y, &nx, &ny);
        normal->x = nx;
        normal->y = ny;
    }

    // Reverse exit
    if (reverse) {
        int16_t rx, ry;
        transform_point(LOCAL_REVERSE_DX, LOCAL_REVERSE_DY, item,
                        origin_x, origin_y, &rx, &ry);
        reverse->x = rx;
        reverse->y = ry;
    }
}

void panel_geometry_get_connection_point(const panel_item_t *item,
                                         panel_point_type_t point,
                                         int16_t *px, int16_t *py)
{
    if (!item || !px || !py) return;

    lv_point_t entry, normal, reverse;
    panel_geometry_get_points(item, &entry, &normal, &reverse);

    switch (point) {
        case PANEL_POINT_ENTRY:
            *px = (int16_t)entry.x;
            *py = (int16_t)entry.y;
            break;
        case PANEL_POINT_NORMAL:
            *px = (int16_t)normal.x;
            *py = (int16_t)normal.y;
            break;
        case PANEL_POINT_REVERSE:
            *px = (int16_t)reverse.x;
            *py = (int16_t)reverse.y;
            break;
        default:
            *px = (int16_t)entry.x;
            *py = (int16_t)entry.y;
            break;
    }
}

void panel_geometry_get_center(const panel_item_t *item,
                               int16_t *cx, int16_t *cy)
{
    if (!item || !cx || !cy) return;

    lv_point_t entry, normal, reverse;
    panel_geometry_get_points(item, &entry, &normal, &reverse);

    // Center is the average of all three points
    *cx = (int16_t)((entry.x + normal.x + reverse.x) / 3);
    *cy = (int16_t)((entry.y + normal.y + reverse.y) / 3);
}
