/**
 * @file turnout_manager.h
 * @brief Turnout state management and coordination
 * 
 * Manages the in-memory array of turnout definitions, state tracking,
 * and coordination between LCC events and the UI. Thread-safe - can be
 * called from the OpenMRN executor thread and the LVGL task.
 */

#ifndef TURNOUT_MANAGER_H_
#define TURNOUT_MANAGER_H_

#include "esp_err.h"
#include "../ui/ui_common.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback type for turnout state changes
 * 
 * Called from the turnout manager when a turnout's state is updated
 * (e.g., from an LCC event). The callback should be lightweight -
 * typically queues an LVGL UI update.
 * 
 * @param index Turnout index in the manager array
 * @param state New turnout state
 */
typedef void (*turnout_state_callback_t)(int index, turnout_state_t state);

/**
 * @brief Initialize the turnout manager
 * 
 * Loads turnout definitions from SD card. Must be called after SD is mounted.
 * 
 * @return ESP_OK on success
 */
esp_err_t turnout_manager_init(void);

/**
 * @brief Register a callback for turnout state changes
 * 
 * @param cb Callback function (NULL to unregister)
 */
void turnout_manager_set_state_callback(turnout_state_callback_t cb);

/**
 * @brief Get total number of managed turnouts
 */
size_t turnout_manager_get_count(void);

/**
 * @brief Get a copy of a turnout by index
 * 
 * @param index Turnout index
 * @param out Output turnout struct
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t turnout_manager_get_by_index(size_t index, turnout_t *out);

/**
 * @brief Get a pointer to the internal turnout array (read-only access)
 * 
 * Caller MUST hold the turnout manager lock while accessing this pointer.
 * Use turnout_manager_lock() / turnout_manager_unlock().
 * 
 * @param out_turnouts Output pointer to array
 * @param out_count Output count
 */
void turnout_manager_get_all(const turnout_t **out_turnouts, size_t *out_count);

/**
 * @brief Add a new turnout
 * 
 * @param event_normal Event ID for NORMAL command
 * @param event_reverse Event ID for REVERSE command  
 * @param name Display name (will be truncated to 31 chars)
 * @return Index of the new turnout, or -1 on failure
 */
int turnout_manager_add(uint64_t event_normal, uint64_t event_reverse, const char *name);

/**
 * @brief Remove a turnout by index
 * 
 * @param index Turnout index to remove
 * @return ESP_OK on success
 */
esp_err_t turnout_manager_remove(size_t index);

/**
 * @brief Rename a turnout
 * 
 * @param index Turnout index
 * @param name New name
 * @return ESP_OK on success
 */
esp_err_t turnout_manager_rename(size_t index, const char *name);

/**
 * @brief Swap two turnouts (for reordering)
 * 
 * @param index_a First turnout index
 * @param index_b Second turnout index
 * @return ESP_OK on success
 */
esp_err_t turnout_manager_swap(size_t index_a, size_t index_b);

/**
 * @brief Update turnout state from an LCC event
 * 
 * Called when a ProducerIdentified or EventReport is received.
 * Matches the event_id to a turnout's normal/reverse event and updates state.
 * Clears command_pending. Invokes the state callback if registered.
 * 
 * @param event_id The LCC event ID received
 * @param state The state indicated by this event (NORMAL or REVERSE)
 */
void turnout_manager_set_state_by_event(uint64_t event_id, turnout_state_t state);

/**
 * @brief Set command_pending flag for a turnout
 * 
 * @param index Turnout index
 * @param pending Whether a command is pending
 */
void turnout_manager_set_pending(size_t index, bool pending);

/**
 * @brief Find a turnout by event ID
 * 
 * @param event_id Event ID to search for (matches either normal or reverse)
 * @return Index of the matching turnout, or -1 if not found
 */
int turnout_manager_find_by_event(uint64_t event_id);

/**
 * @brief Check for stale turnouts and update their state
 * 
 * Any turnout with last_update_us older than timeout_ms will be marked STALE.
 * Call periodically from the main loop.
 * 
 * @param timeout_ms Stale timeout in milliseconds
 */
void turnout_manager_check_stale(uint32_t timeout_ms);

/**
 * @brief Save current turnout definitions to SD card
 * 
 * @return ESP_OK on success
 */
esp_err_t turnout_manager_save(void);

/**
 * @brief Lock the turnout manager mutex
 * 
 * Must be held when accessing the array pointer from get_all().
 * Do NOT hold this while doing lengthy operations.
 */
void turnout_manager_lock(void);

/**
 * @brief Unlock the turnout manager mutex
 */
void turnout_manager_unlock(void);

#ifdef __cplusplus
}
#endif

#endif // TURNOUT_MANAGER_H_
