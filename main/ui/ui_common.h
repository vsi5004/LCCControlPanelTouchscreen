/**
 * @file ui_common.h
 * @brief Common UI definitions and initialization for LCC Turnout Panel
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"
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
 * @brief Show splash screen
 * 
 * @param disp LVGL display object
 */
void ui_show_splash(lv_disp_t *disp);

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
 * @brief Create the main screen with tabview
 */
void ui_create_main_screen(void);

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

#ifdef __cplusplus
}
#endif
