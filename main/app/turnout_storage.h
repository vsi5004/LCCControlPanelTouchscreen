/**
 * @file turnout_storage.h
 * @brief Turnout definition persistence to SD card
 * 
 * Stores and loads turnout definitions (event ID pairs, names, order)
 * in JSON format on the SD card.
 */

#ifndef TURNOUT_STORAGE_H_
#define TURNOUT_STORAGE_H_

#include "esp_err.h"
#include "../ui/ui_common.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Path to turnout definitions file on SD card
#define TURNOUT_STORAGE_PATH "/sdcard/turnouts.json"

/**
 * @brief Load turnout definitions from SD card
 * 
 * Reads /sdcard/turnouts.json and populates the turnouts array.
 * States are set to TURNOUT_STATE_UNKNOWN (actual state comes from LCC queries).
 * 
 * @param turnouts Output array to populate
 * @param max_count Maximum entries the array can hold
 * @param out_count Output: number of turnouts loaded
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file missing
 */
esp_err_t turnout_storage_load(turnout_t *turnouts, size_t max_count, size_t *out_count);

/**
 * @brief Save turnout definitions to SD card
 * 
 * Writes the turnout array to /sdcard/turnouts.json.
 * Only persists name, event IDs, and user_order — state is transient.
 * 
 * @param turnouts Array of turnout definitions
 * @param count Number of turnouts to save
 * @return ESP_OK on success
 */
esp_err_t turnout_storage_save(const turnout_t *turnouts, size_t count);

#ifdef __cplusplus
}
#endif

#endif // TURNOUT_STORAGE_H_
