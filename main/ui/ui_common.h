/**
 * @file ui_common.h
 * @brief Common UI definitions and initialization for LCC Turnout Panel
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LVGL configuration
 */
#define UI_LVGL_TASK_PRIORITY       CONFIG_LVGL_TASK_PRIORITY
#define UI_LVGL_TASK_STACK_SIZE_KB  CONFIG_LVGL_TASK_STACK_SIZE_KB
#define UI_LVGL_TICK_PERIOD_MS      CONFIG_LVGL_TICK_PERIOD_MS
#define UI_LVGL_TASK_MAX_DELAY_MS   CONFIG_LVGL_TASK_MAX_DELAY_MS
#define UI_LVGL_TASK_MIN_DELAY_MS   CONFIG_LVGL_TASK_MIN_DELAY_MS

/**
 * @brief Maximum number of turnouts the panel can manage
 */
#define TURNOUT_MAX_COUNT   150

/**
 * @brief Turnout state enumeration
 */
typedef enum {
    TURNOUT_STATE_UNKNOWN = 0,  ///< State not yet known
    TURNOUT_STATE_NORMAL,       ///< Turnout is in NORMAL (closed) position
    TURNOUT_STATE_REVERSE,      ///< Turnout is in REVERSE (thrown) position
    TURNOUT_STATE_STALE,        ///< No state update received within timeout
} turnout_state_t;

/**
 * @brief Turnout definition structure
 * 
 * Represents a single turnout on the layout. Each turnout is identified
 * by a pair of LCC event IDs: one for NORMAL and one for REVERSE.
 */
typedef struct {
    uint32_t id;                ///< Stable unique ID (auto-assigned, never changes)
    char name[32];              ///< User-assigned name
    uint64_t event_normal;      ///< LCC event ID for NORMAL/CLOSED command
    uint64_t event_reverse;     ///< LCC event ID for REVERSE/THROWN command
    turnout_state_t state;      ///< Current known state
    int64_t last_update_us;     ///< Timestamp of last state update (esp_timer_get_time)
    bool command_pending;       ///< True when a command has been sent, awaiting confirmation
    uint16_t user_order;        ///< User-assigned display order
} turnout_t;

/**
 * @brief Initialize LVGL with LCD and touch
 * 
 * @param disp Output display handle
 * @param touch_indev Output touch input device handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ui_init(lv_disp_t **disp, lv_indev_t **touch_indev);

/**
 * @brief Display a JPEG splash image on the LCD framebuffer (pre-LVGL)
 *
 * Writes directly to the RGB LCD framebuffer — does NOT use LVGL.
 *
 * @param panel LCD panel handle
 * @param filepath Path to baseline JPEG file on SD (e.g., "/sdcard/SPLASH.JPG")
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ui_splash_show_image(esp_lcd_panel_handle_t panel, const char *filepath);

/**
 * @brief Show SD-card-missing error screen and halt
 *
 * Initialises LVGL if needed, displays a warning, and loops forever.
 * Does NOT return — the user must insert an SD card and restart.
 */
void ui_splash_show_sd_error(void);

/**
 * @brief Show main UI (tabs with Turnouts and Add Turnout)
 */
void ui_show_main(void);

/**
 * @brief Lock LVGL mutex (for non-UI task access)
 * 
 * @return true if locked successfully
 */
bool ui_lock(void);

/**
 * @brief Unlock LVGL mutex
 */
void ui_unlock(void);

// ----- Main Screen Functions -----

/**
 * @brief Get the turnouts tab object
 */
lv_obj_t* ui_get_turnouts_tab(void);

/**
 * @brief Get the add turnout tab object
 */
lv_obj_t* ui_get_add_turnout_tab(void);

// ----- Turnout Switchboard Tab Functions -----

/**
 * @brief Create the turnout switchboard tab content
 */
void ui_create_turnouts_tab(lv_obj_t *parent);

/**
 * @brief Refresh all turnout tiles from the turnout manager data
 * 
 * Rebuilds the tile grid. Call after adding/removing turnouts.
 */
void ui_turnouts_refresh(void);

/**
 * @brief Update a single turnout tile's visual state
 * 
 * Lightweight update - changes color/status indicator without rebuilding.
 * Safe to call from LVGL async context.
 * 
 * @param index Turnout index in the manager array
 * @param state New state to display
 */
void ui_turnouts_update_tile(int index, turnout_state_t state);

/**
 * @brief Invalidate turnout tile tracking pointers
 *
 * Must be called when the settings screen is destroyed (before navigating
 * to the panel screen) so that async state-change callbacks don't access
 * freed LVGL objects.
 */
void ui_turnouts_invalidate(void);

/**
 * @brief Clear the command-pending indicator on a turnout tile
 * 
 * @param index Turnout index
 */
void ui_turnouts_clear_pending(int index);

// ----- Add Turnout Tab Functions -----

/**
 * @brief Create the add turnout tab content
 */
void ui_create_add_turnout_tab(lv_obj_t *parent);

/**
 * @brief Add a discovered turnout to the discovery list UI
 * 
 * @param event_id The event ID that was discovered
 * @param state The state indicated by the producer
 */
void ui_add_turnout_discovery_event(uint64_t event_id, turnout_state_t state);

/**
 * @brief Clear the discovery list
 */
void ui_add_turnout_clear_discoveries(void);

// ============================================================================
// Panel Layout Data Model (types, constants, operations → panel_layout.h)
// ============================================================================

#include "panel_layout.h"

/**
 * @brief Width of the panel canvas area in pixels (full screen width)
 */
#define PANEL_CANVAS_WIDTH  800

/**
 * @brief Height of the panel canvas area (full screen height)
 */
#define PANEL_CANVAS_HEIGHT 480

// ----- Panel Screen Functions -----

/**
 * @brief Create the control panel screen (default/main screen)
 *
 * Displays the layout diagram with turnouts and tracks.
 * Has a settings gear icon in the upper-right corner.
 */
void ui_create_panel_screen(void);

/**
 * @brief Update a turnout's visual state on the panel screen
 *
 * Lightweight update — changes line colors without full re-render.
 * Safe to call from LVGL async context.
 *
 * @param index Turnout index in the manager array
 * @param state New state to display
 */
void ui_panel_update_turnout(int index, turnout_state_t state);

/**
 * @brief Invalidate panel screen tracking pointers
 *
 * Must be called when the panel screen is destroyed (before navigating
 * to the settings screen) so that async state-change callbacks don't
 * access freed LVGL objects.
 */
void ui_panel_invalidate(void);

/**
 * @brief Trigger a full re-render of the panel screen
 *
 * Call after the panel builder modifies the layout.
 */
void ui_panel_refresh(void);

// ----- Settings Screen Functions -----

/**
 * @brief Show the settings screen (tabview with 3 tabs)
 *
 * Creates a screen with: Turnouts, Add Turnout, Panel Builder tabs,
 * plus a back button to return to the control panel.
 */
void ui_show_settings(void);

/**
 * @brief Show the settings screen and jump directly to a specific tab
 *
 * @param tab_idx Zero-based tab index (0=Turnouts, 1=Add Turnout, 2=Panel Builder)
 */
void ui_show_settings_at_tab(uint32_t tab_idx);

/**
 * @brief Get the panel builder tab object
 */
lv_obj_t* ui_get_panel_builder_tab(void);

// ----- Panel Builder Tab Functions -----

/**
 * @brief Create the panel builder tab content
 *
 * @param parent The tab container to build into
 */
void ui_create_panel_builder_tab(lv_obj_t *parent);

/**
 * @brief Refresh the panel builder view after layout changes
 */
void ui_panel_builder_refresh(void);

#ifdef __cplusplus
}
#endif
