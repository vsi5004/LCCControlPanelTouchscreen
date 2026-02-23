/**
 * @file panel_geometry.h
 * @brief Turnout Y-shape geometry calculations for the panel layout
 *
 * Provides functions to compute pixel positions of turnout connection points
 * and line endpoints based on grid position, rotation (0-7), and mirror state.
 *
 * Base orientation (rotation=0, mirrored=false):
 *   Entry at left, Normal exit at right (colinear), Reverse exit upper-right ~45°.
 *
 *   Reverse exit
 *        \
 *         \
 *   Entry --------- Normal exit
 *
 * Rotation increments are 45° clockwise. Mirror flips the diverging leg
 * vertically (before rotation), giving left/right hand turnout support.
 * 8 rotations × 2 mirror = 16 unique orientations.
 */

#ifndef PANEL_GEOMETRY_H_
#define PANEL_GEOMETRY_H_

#include "panel_layout.h"
#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Size of the turnout symbol in pixels
 *
 * The Y-shape spans this many pixels along its primary axis.
 */
#define TURNOUT_SYMBOL_LENGTH   60
#define TURNOUT_SYMBOL_SPREAD   30  ///< Perpendicular spread of diverging leg

/**
 * @brief Get the three line endpoints for drawing a turnout symbol
 *
 * Returns absolute pixel coordinates for the entry point, normal exit,
 * and reverse exit of the Y-shape, transformed by position/rotation/mirror.
 *
 * @param item     Panel item with position, rotation, mirror
 * @param entry    Output: entry (common) point in pixels
 * @param normal   Output: normal exit point in pixels
 * @param reverse  Output: reverse exit point in pixels
 */
void panel_geometry_get_points(const panel_item_t *item,
                               lv_point_t *entry,
                               lv_point_t *normal,
                               lv_point_t *reverse);

/**
 * @brief Get a single connection point's pixel coordinates
 *
 * @param item   Panel item
 * @param point  Which connection point
 * @param px     Output X pixel coordinate
 * @param py     Output Y pixel coordinate
 */
void panel_geometry_get_connection_point(const panel_item_t *item,
                                         panel_point_type_t point,
                                         int16_t *px, int16_t *py);

/**
 * @brief Get the bounding box center of a turnout symbol
 *
 * Useful for hit-testing and placing clickable overlays.
 *
 * @param item  Panel item
 * @param cx    Output center X pixel
 * @param cy    Output center Y pixel
 */
void panel_geometry_get_center(const panel_item_t *item,
                               int16_t *cx, int16_t *cy);

#ifdef __cplusplus
}
#endif

#endif // PANEL_GEOMETRY_H_
