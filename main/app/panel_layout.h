/**
 * @file panel_layout.h
 * @brief Panel layout data model and operations
 *
 * Pure data types for the control panel layout (turnouts, endpoints, tracks)
 * and operations to query/mutate the layout.  No LVGL or UI dependency — can
 * be used by both app-layer storage and UI rendering code.
 *
 * Design rationale:
 *   The layout model was previously defined inside ui_common.h, which forced
 *   app-layer modules (panel_storage, turnout_manager) to depend on the UI
 *   header.  Extracting it here breaks that circular dependency and gives the
 *   data model a clear owner (panel_layout.c) following the Single
 *   Responsibility Principle.
 */

#ifndef PANEL_LAYOUT_H_
#define PANEL_LAYOUT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constants
// ============================================================================

/**
 * @brief Maximum placed turnouts on the panel
 *
 * Keep these limits modest — every extra slot adds to the static BSS in
 * internal SRAM.  On the ESP32-S3 the RGB LCD driver needs DMA-capable
 * internal RAM for bounce buffers; oversized statics starve that allocation.
 */
#define PANEL_MAX_ITEMS     50

/** @brief Maximum track endpoints (dead-end terminators) */
#define PANEL_MAX_ENDPOINTS 20

/** @brief Maximum track segments connecting points */
#define PANEL_MAX_TRACKS    100

/** @brief Grid cell size in pixels for panel layout positioning */
#define PANEL_GRID_SIZE     20

// ============================================================================
// Data Types
// ============================================================================

/** @brief Turnout connection point type */
typedef enum {
    PANEL_POINT_ENTRY = 0,      ///< Entry/common rail of turnout
    PANEL_POINT_NORMAL,         ///< Normal (closed/straight) exit
    PANEL_POINT_REVERSE,        ///< Reverse (thrown/diverging) exit
} panel_point_type_t;

/**
 * @brief A turnout placed on the panel layout
 *
 * Identified by turnout_id which is the stable unique key linking to
 * turnout_manager.  This ID never changes when events are edited or
 * polarity is flipped, so track references remain valid.
 * Position is in grid coordinates (multiply by PANEL_GRID_SIZE for pixels).
 * Rotation 0-7 maps to 0°, 45°, 90°, ... 315° clockwise.
 */
typedef struct {
    uint32_t turnout_id;        ///< Stable turnout ID (matches turnout_t.id)
    uint16_t grid_x;            ///< X position in grid cells
    uint16_t grid_y;            ///< Y position in grid cells
    uint8_t  rotation;          ///< Rotation index 0-7 (0°-315° in 45° steps)
    bool     mirrored;          ///< Mirror the diverging leg (left/right hand)
} panel_item_t;

/**
 * @brief An endpoint placed on the panel layout (track terminator)
 *
 * A simple single connection point — a "dead end" or line terminator.
 * Identified by a unique uint32 ID.  Position is in grid coordinates.
 */
typedef struct {
    uint32_t id;                ///< Unique endpoint identifier
    uint16_t grid_x;            ///< X position in grid cells
    uint16_t grid_y;            ///< Y position in grid cells
} panel_endpoint_t;

/**
 * @brief Type of panel element referenced by a track endpoint
 *
 * Extensible: add new element types (signals, crossings, etc.) here.
 */
typedef enum {
    PANEL_REF_TURNOUT = 0,      ///< References a turnout (by turnout_t.id)
    PANEL_REF_ENDPOINT,         ///< References a panel endpoint (by endpoint.id)
} panel_ref_type_t;

/**
 * @brief A typed reference to a connectable panel element
 *
 * Generic "pointer" to any element that a track can connect to.
 * The (type, id) pair uniquely identifies the target.
 */
typedef struct {
    panel_ref_type_t  type;     ///< What kind of element this references
    uint32_t          id;       ///< Stable ID of the referenced element
    panel_point_type_t point;   ///< Connection point (meaningful for turnouts)
} panel_ref_t;

/**
 * @brief A track segment connecting two connection points
 *
 * Each end is a panel_ref_t — a typed reference to any connectable
 * element (turnout, endpoint, or future element types).
 */
typedef struct {
    panel_ref_t from;               ///< Source connection
    panel_ref_t to;                 ///< Destination connection
} panel_track_t;

/**
 * @brief Complete panel layout definition
 *
 * Holds all placed turnouts, endpoints, and track connections for the
 * control panel.  A single instance is owned by panel_layout.c and
 * accessed via panel_layout_get().
 */
typedef struct {
    panel_item_t     items[PANEL_MAX_ITEMS];            ///< Placed turnout items
    size_t           item_count;                         ///< Number of placed items
    panel_endpoint_t endpoints[PANEL_MAX_ENDPOINTS];     ///< Placed endpoints
    size_t           endpoint_count;                     ///< Number of placed endpoints
    uint32_t         next_endpoint_id;                   ///< Auto-increment ID for new endpoints
    panel_track_t    tracks[PANEL_MAX_TRACKS];           ///< Track segments
    size_t           track_count;                        ///< Number of track segments
} panel_layout_t;

// ============================================================================
// Singleton Access
// ============================================================================

/**
 * @brief Get the global panel layout instance
 *
 * The layout is a singleton owned by the panel_layout module.
 * All modules (UI, storage, main) access it through this function.
 */
panel_layout_t* panel_layout_get(void);

// ============================================================================
// Query Operations
// ============================================================================

/** @brief Check if the layout has no items and no endpoints */
bool panel_layout_is_empty(const panel_layout_t *layout);

/** @brief Check if a turnout (by stable ID) is already placed */
bool panel_layout_is_turnout_placed(const panel_layout_t *layout,
                                     uint32_t turnout_id);

/** @brief Find a placed item index by turnout ID.  Returns -1 if not found. */
int panel_layout_find_item(const panel_layout_t *layout, uint32_t turnout_id);

/**
 * @brief Resolve a track segment to pixel coordinates
 *
 * Looks up the pixel positions of both ends of a track segment,
 * resolving turnout connection points via panel_geometry and
 * endpoint grid positions via direct grid→pixel conversion.
 *
 * @param layout  The panel layout
 * @param track   Track segment to resolve
 * @param x1,y1   Output: "from" end pixel coordinates
 * @param x2,y2   Output: "to" end pixel coordinates
 * @return true if both ends were resolved successfully
 */
bool panel_layout_resolve_track(const panel_layout_t *layout,
                                 const panel_track_t *track,
                                 int16_t *x1, int16_t *y1,
                                 int16_t *x2, int16_t *y2);

/**
 * @brief Compute the world-space bounding box of all items and endpoints
 *
 * @param layout   The panel layout
 * @param margin   Extra padding around the bounds (pixels)
 * @param min_x .. max_y  Output bounding box
 * @return true if any items/endpoints exist (bounds are valid)
 */
bool panel_layout_get_bounds(const panel_layout_t *layout,
                              int16_t margin,
                              int16_t *min_x, int16_t *min_y,
                              int16_t *max_x, int16_t *max_y);

// ============================================================================
// Mutation Operations
// ============================================================================

/**
 * @brief Add a turnout item to the layout
 * @return Index of the new item, or -1 if layout is full
 */
int panel_layout_add_item(panel_layout_t *layout, uint32_t turnout_id,
                           uint16_t grid_x, uint16_t grid_y);

/**
 * @brief Add an endpoint to the layout (auto-assigns a unique ID)
 * @param out_index  Output: array index of the new endpoint (may be NULL)
 * @return true on success, false if layout is full
 */
bool panel_layout_add_endpoint(panel_layout_t *layout,
                                uint16_t grid_x, uint16_t grid_y,
                                size_t *out_index);

/**
 * @brief Add a track segment to the layout
 * @return true on success, false if layout is full
 */
bool panel_layout_add_track(panel_layout_t *layout, const panel_track_t *track);

/**
 * @brief Remove a turnout item by index, cascading to connected tracks
 */
void panel_layout_remove_item(panel_layout_t *layout, size_t index);

/**
 * @brief Remove an endpoint by index, cascading to connected tracks
 */
void panel_layout_remove_endpoint(panel_layout_t *layout, size_t index);

/**
 * @brief Remove a track segment by index
 */
void panel_layout_remove_track(panel_layout_t *layout, size_t index);

#ifdef __cplusplus
}
#endif

#endif // PANEL_LAYOUT_H_
