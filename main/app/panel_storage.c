/**
 * @file panel_storage.c
 * @brief Panel layout persistence to SD card
 *
 * Stores the control panel layout as JSON on SD card. Turnout items are
 * referenced by their stable turnout_id (integer). Track endpoints use
 * "turnout:N" or "endpoint:N" string format.
 *
 * JSON format:
 * {
 *   "version": 2,
 *   "items": [
 *     {
 *       "turnout_id": 1,
 *       "grid_x": 5,
 *       "grid_y": 3,
 *       "rotation": 0,
 *       "mirrored": false
 *     }
 *   ],
 *   "endpoints": [
 *     {
 *       "id": 1,
 *       "grid_x": 10,
 *       "grid_y": 4
 *     }
 *   ],
 *   "next_endpoint_id": 2,
 *   "tracks": [
 *     {
 *       "from": "turnout:1",
 *       "from_point": "entry",
 *       "to": "endpoint:1",
 *       "to_point": "entry"
 *     }
 *   ]
 * }
 */

#include "panel_storage.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/** @brief Max attempts for SD card file open (card may need wake-up) */
#define SD_OPEN_MAX_RETRIES  3
#define SD_OPEN_RETRY_MS     100

static const char *TAG = "panel_storage";

// ============================================================================
// Point type string conversion
// ============================================================================

static const char* point_type_to_str(panel_point_type_t pt)
{
    switch (pt) {
        case PANEL_POINT_ENTRY:   return "entry";
        case PANEL_POINT_NORMAL:  return "normal";
        case PANEL_POINT_REVERSE: return "reverse";
        default:                  return "entry";
    }
}

static panel_point_type_t str_to_point_type(const char *str)
{
    if (!str) return PANEL_POINT_ENTRY;
    if (strcmp(str, "normal") == 0)  return PANEL_POINT_NORMAL;
    if (strcmp(str, "reverse") == 0) return PANEL_POINT_REVERSE;
    return PANEL_POINT_ENTRY;
}



// ============================================================================
// Public API
// ============================================================================

esp_err_t panel_storage_load(panel_layout_t *layout)
{
    if (!layout) return ESP_ERR_INVALID_ARG;

    // Initialize to empty
    memset(layout, 0, sizeof(panel_layout_t));

    struct stat st;
    if (stat(PANEL_STORAGE_PATH, &st) != 0) {
        ESP_LOGI(TAG, "No panel layout file found at %s - starting empty", PANEL_STORAGE_PATH);
        return ESP_OK;
    }

    FILE *f = fopen(PANEL_STORAGE_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open %s", PANEL_STORAGE_PATH);
        return ESP_OK;  // Not an error — just empty layout
    }

    // Read entire file
    char *buf = malloc(st.st_size + 1);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate %ld bytes for panel file", (long)st.st_size);
        return ESP_OK;
    }

    size_t read_len = fread(buf, 1, st.st_size, f);
    fclose(f);
    buf[read_len] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGW(TAG, "Failed to parse panel JSON");
        return ESP_OK;
    }

    // Check version
    cJSON *version = cJSON_GetObjectItem(root, "version");
    int ver = cJSON_IsNumber(version) ? version->valueint : -1;
    if (ver != 1 && ver != 2) {
        ESP_LOGW(TAG, "Unknown panel version: %d", ver);
        cJSON_Delete(root);
        return ESP_OK;
    }

    // Parse items
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) {
        int count = cJSON_GetArraySize(items);
        if (count > PANEL_MAX_ITEMS) {
            ESP_LOGW(TAG, "Panel has %d items, truncating to %d", count, PANEL_MAX_ITEMS);
            count = PANEL_MAX_ITEMS;
        }

        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(items, i);
            if (!item) continue;

            cJSON *tid = cJSON_GetObjectItem(item, "turnout_id");
            cJSON *gx = cJSON_GetObjectItem(item, "grid_x");
            cJSON *gy = cJSON_GetObjectItem(item, "grid_y");
            cJSON *rot = cJSON_GetObjectItem(item, "rotation");
            cJSON *mir = cJSON_GetObjectItem(item, "mirrored");

            if (!cJSON_IsNumber(tid)) continue;

            panel_item_t *pi = &layout->items[layout->item_count];
            pi->turnout_id = (uint32_t)tid->valueint;

            pi->grid_x = cJSON_IsNumber(gx) ? (uint16_t)gx->valueint : 0;
            pi->grid_y = cJSON_IsNumber(gy) ? (uint16_t)gy->valueint : 0;
            pi->rotation = cJSON_IsNumber(rot) ? (uint8_t)(rot->valueint & 0x07) : 0;
            pi->mirrored = cJSON_IsBool(mir) ? cJSON_IsTrue(mir) : false;

            layout->item_count++;
        }
    }

    // Parse endpoints
    cJSON *endpoints = cJSON_GetObjectItem(root, "endpoints");
    if (cJSON_IsArray(endpoints)) {
        int count = cJSON_GetArraySize(endpoints);
        if (count > PANEL_MAX_ENDPOINTS) {
            ESP_LOGW(TAG, "Panel has %d endpoints, truncating to %d", count, PANEL_MAX_ENDPOINTS);
            count = PANEL_MAX_ENDPOINTS;
        }

        for (int i = 0; i < count; i++) {
            cJSON *ep = cJSON_GetArrayItem(endpoints, i);
            if (!ep) continue;

            cJSON *j_id = cJSON_GetObjectItem(ep, "id");
            cJSON *gx = cJSON_GetObjectItem(ep, "grid_x");
            cJSON *gy = cJSON_GetObjectItem(ep, "grid_y");

            if (!cJSON_IsNumber(j_id)) continue;

            panel_endpoint_t *pe = &layout->endpoints[layout->endpoint_count];
            pe->id = (uint32_t)j_id->valueint;
            pe->grid_x = cJSON_IsNumber(gx) ? (uint16_t)gx->valueint : 0;
            pe->grid_y = cJSON_IsNumber(gy) ? (uint16_t)gy->valueint : 0;

            layout->endpoint_count++;
        }
    }

    // Parse next_endpoint_id
    cJSON *next_id = cJSON_GetObjectItem(root, "next_endpoint_id");
    layout->next_endpoint_id = cJSON_IsNumber(next_id) ? (uint32_t)next_id->valueint : 1;

    // Parse tracks
    cJSON *tracks = cJSON_GetObjectItem(root, "tracks");
    if (cJSON_IsArray(tracks)) {
        int count = cJSON_GetArraySize(tracks);
        if (count > PANEL_MAX_TRACKS) {
            ESP_LOGW(TAG, "Panel has %d tracks, truncating to %d", count, PANEL_MAX_TRACKS);
            count = PANEL_MAX_TRACKS;
        }

        for (int i = 0; i < count; i++) {
            cJSON *track = cJSON_GetArrayItem(tracks, i);
            if (!track) continue;

            cJSON *from_ev = cJSON_GetObjectItem(track, "from");
            cJSON *from_pt = cJSON_GetObjectItem(track, "from_point");
            cJSON *to_ev = cJSON_GetObjectItem(track, "to");
            cJSON *to_pt = cJSON_GetObjectItem(track, "to_point");

            if (!cJSON_IsString(from_ev) || !cJSON_IsString(to_ev)) continue;

            panel_track_t *pt = &layout->tracks[layout->track_count];
            memset(pt, 0, sizeof(*pt));

            // Parse "from" reference — either "endpoint:N" or "turnout:N"
            const char *from_str = from_ev->valuestring;
            if (strncmp(from_str, "endpoint:", 9) == 0) {
                pt->from.type = PANEL_REF_ENDPOINT;
                pt->from.id = (uint32_t)atoi(from_str + 9);
            } else if (strncmp(from_str, "turnout:", 8) == 0) {
                pt->from.type = PANEL_REF_TURNOUT;
                pt->from.id = (uint32_t)atoi(from_str + 8);
            } else {
                ESP_LOGW(TAG, "Skipping track %d - unrecognized from ref: %s", i, from_str);
                continue;
            }

            // Parse "to" reference
            const char *to_str = to_ev->valuestring;
            if (strncmp(to_str, "endpoint:", 9) == 0) {
                pt->to.type = PANEL_REF_ENDPOINT;
                pt->to.id = (uint32_t)atoi(to_str + 9);
            } else if (strncmp(to_str, "turnout:", 8) == 0) {
                pt->to.type = PANEL_REF_TURNOUT;
                pt->to.id = (uint32_t)atoi(to_str + 8);
            } else {
                ESP_LOGW(TAG, "Skipping track %d - unrecognized to ref: %s", i, to_str);
                continue;
            }

            pt->from.point = cJSON_IsString(from_pt) ? str_to_point_type(from_pt->valuestring) : PANEL_POINT_ENTRY;
            pt->to.point = cJSON_IsString(to_pt) ? str_to_point_type(to_pt->valuestring) : PANEL_POINT_ENTRY;

            layout->track_count++;
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Panel layout loaded: %d items, %d endpoints, %d tracks",
             (int)layout->item_count, (int)layout->endpoint_count, (int)layout->track_count);

    return ESP_OK;
}

esp_err_t panel_storage_save(const panel_layout_t *layout)
{
    if (!layout) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Saving panel layout: %d items, %d endpoints, %d tracks",
             (int)layout->item_count, (int)layout->endpoint_count, (int)layout->track_count);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON root");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "version", 2);

    // Serialize items
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    if (!items) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < layout->item_count; i++) {
        const panel_item_t *pi = &layout->items[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) continue;

        cJSON_AddNumberToObject(item, "turnout_id", pi->turnout_id);
        cJSON_AddNumberToObject(item, "grid_x", pi->grid_x);
        cJSON_AddNumberToObject(item, "grid_y", pi->grid_y);
        cJSON_AddNumberToObject(item, "rotation", pi->rotation);
        cJSON_AddBoolToObject(item, "mirrored", pi->mirrored);

        cJSON_AddItemToArray(items, item);
    }

    // Serialize endpoints
    cJSON *ep_array = cJSON_AddArrayToObject(root, "endpoints");
    if (ep_array) {
        for (size_t i = 0; i < layout->endpoint_count; i++) {
            const panel_endpoint_t *pe = &layout->endpoints[i];
            cJSON *ep = cJSON_CreateObject();
            if (!ep) continue;
            cJSON_AddNumberToObject(ep, "id", pe->id);
            cJSON_AddNumberToObject(ep, "grid_x", pe->grid_x);
            cJSON_AddNumberToObject(ep, "grid_y", pe->grid_y);
            cJSON_AddItemToArray(ep_array, ep);
        }
    }
    cJSON_AddNumberToObject(root, "next_endpoint_id", layout->next_endpoint_id);

    // Serialize tracks
    cJSON *tracks = cJSON_AddArrayToObject(root, "tracks");
    if (!tracks) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char ref_buf[32];
    for (size_t i = 0; i < layout->track_count; i++) {
        const panel_track_t *pt = &layout->tracks[i];
        cJSON *track = cJSON_CreateObject();
        if (!track) continue;

        if (pt->from.type == PANEL_REF_ENDPOINT) {
            snprintf(ref_buf, sizeof(ref_buf), "endpoint:%u", (unsigned)pt->from.id);
        } else {
            snprintf(ref_buf, sizeof(ref_buf), "turnout:%u", (unsigned)pt->from.id);
        }
        cJSON_AddStringToObject(track, "from", ref_buf);
        cJSON_AddStringToObject(track, "from_point", point_type_to_str(pt->from.point));

        if (pt->to.type == PANEL_REF_ENDPOINT) {
            snprintf(ref_buf, sizeof(ref_buf), "endpoint:%u", (unsigned)pt->to.id);
        } else {
            snprintf(ref_buf, sizeof(ref_buf), "turnout:%u", (unsigned)pt->to.id);
        }
        cJSON_AddStringToObject(track, "to", ref_buf);
        cJSON_AddStringToObject(track, "to_point", point_type_to_str(pt->to.point));

        cJSON_AddItemToArray(tracks, track);
    }

    // Write to file atomically (write to .tmp, then rename)
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return ESP_ERR_NO_MEM;
    }

    const char *tmp_path = PANEL_STORAGE_PATH ".tmp";
    FILE *f = NULL;
    for (int attempt = 0; attempt < SD_OPEN_MAX_RETRIES; attempt++) {
        f = fopen(tmp_path, "w");
        if (f) break;
        ESP_LOGW(TAG, "SD card open failed (attempt %d/%d), retrying...",
                 attempt + 1, SD_OPEN_MAX_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(SD_OPEN_RETRY_MS));
    }
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing after %d attempts",
                 tmp_path, SD_OPEN_MAX_RETRIES);
        cJSON_free(json_str);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fclose(f);
    cJSON_free(json_str);

    if (written != len) {
        ESP_LOGE(TAG, "Failed to write panel JSON (wrote %d of %d)", (int)written, (int)len);
        remove(tmp_path);
        return ESP_FAIL;
    }

    // Atomic rename
    remove(PANEL_STORAGE_PATH);
    if (rename(tmp_path, PANEL_STORAGE_PATH) != 0) {
        ESP_LOGE(TAG, "Failed to rename temp file to %s", PANEL_STORAGE_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Panel layout saved successfully");
    return ESP_OK;
}
