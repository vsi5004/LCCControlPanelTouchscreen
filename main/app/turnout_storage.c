/**
 * @file turnout_storage.c
 * @brief Turnout definition persistence to SD card
 * 
 * Stores turnout definitions as JSON on SD card. Event IDs are stored as
 * dotted-hex strings for human readability (matching the nodeid.txt convention).
 * 
 * JSON format:
 * {
 *   "version": 1,
 *   "turnouts": [
 *     {
 *       "name": "Turnout 1",
 *       "event_normal": "05.01.01.01.22.60.00.00",
 *       "event_reverse": "05.01.01.01.22.60.00.01",
 *       "order": 0
 *     }
 *   ]
 * }
 */

#include "turnout_storage.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "turnout_storage";

// ============================================================================
// Event ID formatting helpers
// ============================================================================

/**
 * @brief Format a 64-bit event ID as dotted hex string
 * 
 * @param event_id The event ID
 * @param buf Output buffer (must be at least 24 bytes)
 */
static void format_event_id(uint64_t event_id, char *buf)
{
    snprintf(buf, 24, "%02X.%02X.%02X.%02X.%02X.%02X.%02X.%02X",
             (unsigned)((event_id >> 56) & 0xFF),
             (unsigned)((event_id >> 48) & 0xFF),
             (unsigned)((event_id >> 40) & 0xFF),
             (unsigned)((event_id >> 32) & 0xFF),
             (unsigned)((event_id >> 24) & 0xFF),
             (unsigned)((event_id >> 16) & 0xFF),
             (unsigned)((event_id >> 8) & 0xFF),
             (unsigned)(event_id & 0xFF));
}

/**
 * @brief Parse a dotted hex event ID string to 64-bit value
 * 
 * @param str Input string (e.g., "05.01.01.01.22.60.00.00")
 * @param out_id Output event ID
 * @return true on success
 */
static bool parse_event_id(const char *str, uint64_t *out_id)
{
    if (!str || !out_id) return false;

    unsigned int bytes[8];
    if (sscanf(str, "%02x.%02x.%02x.%02x.%02x.%02x.%02x.%02x",
               &bytes[0], &bytes[1], &bytes[2], &bytes[3],
               &bytes[4], &bytes[5], &bytes[6], &bytes[7]) == 8) {
        *out_id = ((uint64_t)bytes[0] << 56) |
                  ((uint64_t)bytes[1] << 48) |
                  ((uint64_t)bytes[2] << 40) |
                  ((uint64_t)bytes[3] << 32) |
                  ((uint64_t)bytes[4] << 24) |
                  ((uint64_t)bytes[5] << 16) |
                  ((uint64_t)bytes[6] << 8) |
                  ((uint64_t)bytes[7]);
        return true;
    }

    // Also try plain hex: "0501010122600000"
    char *endptr;
    uint64_t val = strtoull(str, &endptr, 16);
    if (endptr != str && *endptr == '\0') {
        *out_id = val;
        return true;
    }

    return false;
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t turnout_storage_load(turnout_t *turnouts, size_t max_count, size_t *out_count)
{
    if (!turnouts || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;

    struct stat st;
    if (stat(TURNOUT_STORAGE_PATH, &st) != 0) {
        ESP_LOGI(TAG, "turnouts.json not found — starting with empty list");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(TURNOUT_STORAGE_PATH, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", TURNOUT_STORAGE_PATH);
        return ESP_FAIL;
    }

    // Read entire file
    char *buf = malloc(st.st_size + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read_sz = fread(buf, 1, st.st_size, f);
    fclose(f);
    buf[read_sz] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse turnouts.json");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "turnouts");
    if (!cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "turnouts.json missing 'turnouts' array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (count >= max_count) {
            ESP_LOGW(TAG, "Turnout limit reached (%d), ignoring remaining", (int)max_count);
            break;
        }

        turnout_t *t = &turnouts[count];
        memset(t, 0, sizeof(turnout_t));

        // Name
        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(name) && name->valuestring) {
            strncpy(t->name, name->valuestring, sizeof(t->name) - 1);
        } else {
            snprintf(t->name, sizeof(t->name), "Turnout %d", (int)(count + 1));
        }

        // Event IDs
        cJSON *ev_normal = cJSON_GetObjectItem(item, "event_normal");
        cJSON *ev_reverse = cJSON_GetObjectItem(item, "event_reverse");

        if (!cJSON_IsString(ev_normal) || !parse_event_id(ev_normal->valuestring, &t->event_normal)) {
            ESP_LOGW(TAG, "Skipping turnout '%s' — invalid event_normal", t->name);
            continue;
        }
        if (!cJSON_IsString(ev_reverse) || !parse_event_id(ev_reverse->valuestring, &t->event_reverse)) {
            ESP_LOGW(TAG, "Skipping turnout '%s' — invalid event_reverse", t->name);
            continue;
        }

        // Order
        cJSON *order = cJSON_GetObjectItem(item, "order");
        if (cJSON_IsNumber(order)) {
            t->user_order = (uint16_t)order->valueint;
        } else {
            t->user_order = (uint16_t)count;
        }

        // State is always UNKNOWN on load — will be refreshed from LCC
        t->state = TURNOUT_STATE_UNKNOWN;
        t->last_update_us = 0;
        t->command_pending = false;

        count++;
    }

    cJSON_Delete(root);
    *out_count = count;
    ESP_LOGI(TAG, "Loaded %d turnouts from SD card", (int)count);
    return ESP_OK;
}

esp_err_t turnout_storage_save(const turnout_t *turnouts, size_t count)
{
    if (!turnouts && count > 0) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddNumberToObject(root, "version", 1);

    cJSON *arr = cJSON_AddArrayToObject(root, "turnouts");
    if (!arr) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    char ev_buf[24];
    for (size_t i = 0; i < count; i++) {
        const turnout_t *t = &turnouts[i];

        cJSON *item = cJSON_CreateObject();
        if (!item) continue;

        cJSON_AddStringToObject(item, "name", t->name);

        format_event_id(t->event_normal, ev_buf);
        cJSON_AddStringToObject(item, "event_normal", ev_buf);

        format_event_id(t->event_reverse, ev_buf);
        cJSON_AddStringToObject(item, "event_reverse", ev_buf);

        cJSON_AddNumberToObject(item, "order", t->user_order);

        cJSON_AddItemToArray(arr, item);
    }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    FILE *f = fopen(TURNOUT_STORAGE_PATH, "w");
    if (!f) {
        cJSON_free(json_str);
        ESP_LOGE(TAG, "Failed to open %s for writing", TURNOUT_STORAGE_PATH);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fflush(f);
    fclose(f);
    cJSON_free(json_str);

    if (written != len) {
        ESP_LOGE(TAG, "Failed to write complete turnouts.json");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved %d turnouts to SD card", (int)count);
    return ESP_OK;
}
