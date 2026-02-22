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
        ESP_LOGI(TAG, "turnouts.json not found - starting with empty list");
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
            ESP_LOGW(TAG, "Skipping turnout '%s' - invalid event_normal", t->name);
            continue;
        }
        if (!cJSON_IsString(ev_reverse) || !parse_event_id(ev_reverse->valuestring, &t->event_reverse)) {
            ESP_LOGW(TAG, "Skipping turnout '%s' - invalid event_reverse", t->name);
            continue;
        }

        // Order
        cJSON *order = cJSON_GetObjectItem(item, "order");
        if (cJSON_IsNumber(order)) {
            t->user_order = (uint16_t)order->valueint;
        } else {
            t->user_order = (uint16_t)count;
        }

        // State is always UNKNOWN on load - will be refreshed from LCC
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

// ============================================================================
// JMRI XML Import
// ============================================================================

/**
 * @brief Check if an event ID already exists in the turnout array
 */
static bool event_already_exists(const turnout_t *turnouts, size_t count,
                                 uint64_t ev_normal, uint64_t ev_reverse)
{
    for (size_t i = 0; i < count; i++) {
        if (turnouts[i].event_normal == ev_normal ||
            turnouts[i].event_reverse == ev_reverse ||
            turnouts[i].event_normal == ev_reverse ||
            turnouts[i].event_reverse == ev_normal) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Extract the value of an XML attribute from a tag string.
 *
 * Finds attr_name="value" and copies value (without quotes) into out_buf.
 * Returns true on success.
 */
static bool xml_get_attr(const char *tag, const char *attr_name,
                         char *out_buf, size_t buf_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=\"", attr_name);

    const char *p = strstr(tag, search);
    if (!p) return false;

    p += strlen(search);
    const char *end = strchr(p, '"');
    if (!end) return false;

    size_t len = (size_t)(end - p);
    if (len >= buf_len) len = buf_len - 1;
    memcpy(out_buf, p, len);
    out_buf[len] = '\0';
    return true;
}

/**
 * @brief Extract text content between <tag>content</tag>
 *
 * Searches within the region [start, region_end) for <tag_name>...</tag_name>
 * and copies the text content into out_buf.
 */
static bool xml_get_element_text(const char *start, const char *region_end,
                                 const char *tag_name,
                                 char *out_buf, size_t buf_len)
{
    // Build opening tag e.g. "<systemName>"
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag_name);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag_name);

    const char *open = strstr(start, open_tag);
    if (!open || open >= region_end) return false;

    const char *text_start = open + strlen(open_tag);
    const char *close = strstr(text_start, close_tag);
    if (!close || close >= region_end) return false;

    size_t len = (size_t)(close - text_start);
    if (len >= buf_len) len = buf_len - 1;
    memcpy(out_buf, text_start, len);
    out_buf[len] = '\0';
    return true;
}

/**
 * @brief Parse JMRI systemName into two event IDs.
 *
 * JMRI format: "MT05.01.01.01.22.50.00.00;05.01.01.01.22.50.00.01"
 * The "MT" prefix is skipped.  The two dotted-hex event IDs are separated
 * by a semicolon.
 */
static bool parse_jmri_system_name(const char *sys_name,
                                   uint64_t *out_ev1, uint64_t *out_ev2)
{
    const char *p = sys_name;

    // Skip "MT" prefix if present
    if (p[0] == 'M' && p[1] == 'T') {
        p += 2;
    }

    // Find the semicolon separator
    const char *semi = strchr(p, ';');
    if (!semi) return false;

    // Parse first event ID
    char ev1_str[32];
    size_t len1 = (size_t)(semi - p);
    if (len1 >= sizeof(ev1_str)) return false;
    memcpy(ev1_str, p, len1);
    ev1_str[len1] = '\0';

    if (!parse_event_id(ev1_str, out_ev1)) return false;

    // Parse second event ID (after semicolon)
    if (!parse_event_id(semi + 1, out_ev2)) return false;

    return true;
}

esp_err_t turnout_storage_import_jmri(turnout_t *turnouts, size_t *count,
                                      size_t max_count)
{
    if (!turnouts || !count) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(TURNOUT_JMRI_IMPORT_PATH, &st) != 0) {
        ESP_LOGI(TAG, "No JMRI import file found at %s", TURNOUT_JMRI_IMPORT_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(TURNOUT_JMRI_IMPORT_PATH, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", TURNOUT_JMRI_IMPORT_PATH);
        return ESP_FAIL;
    }

    // Read entire file
    char *buf = malloc(st.st_size + 1);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "Not enough memory for JMRI file (%ld bytes)",
                 (long)st.st_size);
        return ESP_ERR_NO_MEM;
    }
    size_t read_sz = fread(buf, 1, st.st_size, f);
    fclose(f);
    buf[read_sz] = '\0';

    // Find the turnouts section:  <turnouts class="...openlcb...">
    const char *turnout_section = strstr(buf, "OlcbTurnoutManager");
    if (!turnout_section) {
        // Try generic turnouts tag
        turnout_section = buf;
    }

    size_t imported = 0;
    const char *cursor = turnout_section;

    while ((cursor = strstr(cursor, "<turnout ")) != NULL) {
        if (*count >= max_count) {
            ESP_LOGW(TAG, "Turnout limit reached, stopping JMRI import");
            break;
        }

        // Find the end of this <turnout ...> ... </turnout> block
        const char *block_end = strstr(cursor, "</turnout>");
        if (!block_end) break;
        block_end += strlen("</turnout>");

        // Extract the opening tag (up to first '>')
        const char *tag_end = strchr(cursor, '>');
        if (!tag_end || tag_end > block_end) {
            cursor = block_end;
            continue;
        }

        // Copy the opening tag for attribute parsing
        size_t tag_len = (size_t)(tag_end - cursor + 1);
        char tag_buf[256];
        if (tag_len >= sizeof(tag_buf)) tag_len = sizeof(tag_buf) - 1;
        memcpy(tag_buf, cursor, tag_len);
        tag_buf[tag_len] = '\0';

        // Extract systemName and userName
        char sys_name[80] = {0};
        char user_name[32] = {0};

        if (!xml_get_element_text(cursor, block_end, "systemName",
                                  sys_name, sizeof(sys_name))) {
            cursor = block_end;
            continue;
        }

        xml_get_element_text(cursor, block_end, "userName",
                             user_name, sizeof(user_name));

        // Parse event IDs from systemName
        uint64_t ev1 = 0, ev2 = 0;
        if (!parse_jmri_system_name(sys_name, &ev1, &ev2)) {
            ESP_LOGW(TAG, "JMRI: failed to parse systemName: %s", sys_name);
            cursor = block_end;
            continue;
        }

        // Check inverted attribute
        char inverted_str[8] = {0};
        bool inverted = false;
        if (xml_get_attr(tag_buf, "inverted", inverted_str,
                         sizeof(inverted_str))) {
            inverted = (strcmp(inverted_str, "true") == 0);
        }

        // In JMRI: systemName is "MT<event1>;<event2>"
        // Without inversion: event1 = closed/normal, event2 = thrown/reverse
        // With inversion:    event1 = thrown/reverse, event2 = closed/normal
        uint64_t ev_normal, ev_reverse;
        if (inverted) {
            ev_normal = ev2;
            ev_reverse = ev1;
        } else {
            ev_normal = ev1;
            ev_reverse = ev2;
        }

        // Skip if already exists
        if (event_already_exists(turnouts, *count, ev_normal, ev_reverse)) {
            cursor = block_end;
            continue;
        }

        // Add new turnout
        turnout_t *t = &turnouts[*count];
        memset(t, 0, sizeof(turnout_t));

        if (user_name[0]) {
            snprintf(t->name, sizeof(t->name), "%s", user_name);
        } else {
            snprintf(t->name, sizeof(t->name), "JMRI Turnout %d",
                     (int)(imported + 1));
        }

        t->event_normal = ev_normal;
        t->event_reverse = ev_reverse;
        t->state = TURNOUT_STATE_UNKNOWN;
        t->user_order = (uint16_t)(*count);

        (*count)++;
        imported++;

        ESP_LOGI(TAG, "JMRI import: '%s' N=%016llx R=%016llx%s",
                 t->name,
                 (unsigned long long)ev_normal,
                 (unsigned long long)ev_reverse,
                 inverted ? " (inverted)" : "");

        cursor = block_end;
    }

    free(buf);

    if (imported > 0) {
        ESP_LOGI(TAG, "Imported %d new turnouts from JMRI file", (int)imported);
    } else {
        ESP_LOGI(TAG, "No new turnouts to import from JMRI file");
    }

    return ESP_OK;
}
