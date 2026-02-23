/**
 * @file panel_storage.h
 * @brief Panel layout persistence to SD card
 *
 * Stores and loads the control panel layout (placed turnouts and track
 * connections) in JSON format on the SD card.
 */

#ifndef PANEL_STORAGE_H_
#define PANEL_STORAGE_H_

#include "esp_err.h"
#include "panel_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Path to panel layout file on SD card
#define PANEL_STORAGE_PATH "/sdcard/panel.json"

/**
 * @brief Load panel layout from SD card
 *
 * Reads /sdcard/panel.json and populates the layout structure.
 * If the file is missing or corrupt, returns an empty layout (not an error).
 *
 * @param layout Output layout to populate
 * @return ESP_OK on success (including empty/missing file)
 */
esp_err_t panel_storage_load(panel_layout_t *layout);

/**
 * @brief Save panel layout to SD card
 *
 * Writes the layout to /sdcard/panel.json atomically.
 *
 * @param layout Layout to save
 * @return ESP_OK on success
 */
esp_err_t panel_storage_save(const panel_layout_t *layout);

#ifdef __cplusplus
}
#endif

#endif // PANEL_STORAGE_H_
