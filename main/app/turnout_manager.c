/**
 * @file turnout_manager.c
 * @brief Turnout state management and coordination
 * 
 * Thread-safe in-memory turnout tracking. The manager owns the canonical
 * turnout array and coordinates between:
 *   - LCC event handler (updates state from network events)
 *   - UI layer (reads state for display, sends commands)
 *   - Persistence layer (load/save to SD)
 */

#include "turnout_manager.h"
#include "turnout_storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "turnout_mgr";

// ============================================================================
// Internal state
// ============================================================================

static turnout_t s_turnouts[TURNOUT_MAX_COUNT];
static size_t s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;
static turnout_state_callback_t s_state_callback = NULL;

// ============================================================================
// Public API
// ============================================================================

esp_err_t turnout_manager_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    memset(s_turnouts, 0, sizeof(s_turnouts));
    s_count = 0;

    size_t loaded = 0;
    esp_err_t ret = turnout_storage_load(s_turnouts, TURNOUT_MAX_COUNT, &loaded);
    if (ret == ESP_OK) {
        s_count = loaded;
        ESP_LOGI(TAG, "Loaded %d turnouts from storage", (int)s_count);
    } else if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No turnouts file found - starting empty");
        s_count = 0;
        ret = ESP_OK; // Not an error
    } else {
        ESP_LOGW(TAG, "Failed to load turnouts: %s", esp_err_to_name(ret));
        s_count = 0;
    }

    // Import from JMRI XML if present (supplements existing turnouts)
    size_t before_import = s_count;
    esp_err_t jmri_ret = turnout_storage_import_jmri(s_turnouts, &s_count,
                                                      TURNOUT_MAX_COUNT);
    if (jmri_ret == ESP_OK && s_count > before_import) {
        ESP_LOGI(TAG, "JMRI import added %d new turnouts (total: %d)",
                 (int)(s_count - before_import), (int)s_count);
        // Save merged list so future boots don't re-import
        turnout_storage_save(s_turnouts, s_count);
    }

    xSemaphoreGive(s_mutex);
    return ret;
}

void turnout_manager_set_state_callback(turnout_state_callback_t cb)
{
    s_state_callback = cb;
}

size_t turnout_manager_get_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t count = s_count;
    xSemaphoreGive(s_mutex);
    return count;
}

esp_err_t turnout_manager_get_by_index(size_t index, turnout_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (index >= s_count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_turnouts[index];
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void turnout_manager_get_all(const turnout_t **out_turnouts, size_t *out_count)
{
    // Caller must hold the lock!
    if (out_turnouts) *out_turnouts = s_turnouts;
    if (out_count) *out_count = s_count;
}

int turnout_manager_add(uint64_t event_normal, uint64_t event_reverse, const char *name)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_count >= TURNOUT_MAX_COUNT) {
        ESP_LOGW(TAG, "Turnout limit reached (%d)", TURNOUT_MAX_COUNT);
        xSemaphoreGive(s_mutex);
        return -1;
    }

    // Check for duplicates
    for (size_t i = 0; i < s_count; i++) {
        if (s_turnouts[i].event_normal == event_normal ||
            s_turnouts[i].event_reverse == event_reverse) {
            ESP_LOGW(TAG, "Duplicate event ID - turnout already exists at index %d", (int)i);
            xSemaphoreGive(s_mutex);
            return -1;
        }
    }

    int idx = (int)s_count;
    turnout_t *t = &s_turnouts[idx];
    memset(t, 0, sizeof(turnout_t));

    if (name && name[0]) {
        strncpy(t->name, name, sizeof(t->name) - 1);
    } else {
        snprintf(t->name, sizeof(t->name), "Turnout %d", idx + 1);
    }

    t->event_normal = event_normal;
    t->event_reverse = event_reverse;
    t->state = TURNOUT_STATE_UNKNOWN;
    t->last_update_us = 0;
    t->command_pending = false;
    t->user_order = (uint16_t)idx;

    s_count++;
    ESP_LOGI(TAG, "Added turnout '%s' at index %d", t->name, idx);

    xSemaphoreGive(s_mutex);
    return idx;
}

esp_err_t turnout_manager_remove(size_t index)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (index >= s_count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Removing turnout '%s' at index %d", s_turnouts[index].name, (int)index);

    // Shift remaining turnouts down
    for (size_t i = index; i < s_count - 1; i++) {
        s_turnouts[i] = s_turnouts[i + 1];
    }
    s_count--;

    // Clear the last slot
    memset(&s_turnouts[s_count], 0, sizeof(turnout_t));

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t turnout_manager_rename(size_t index, const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (index >= s_count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_turnouts[index].name, name, sizeof(s_turnouts[index].name) - 1);
    s_turnouts[index].name[sizeof(s_turnouts[index].name) - 1] = '\0';

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t turnout_manager_swap(size_t index_a, size_t index_b)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (index_a >= s_count || index_b >= s_count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    if (index_a != index_b) {
        turnout_t tmp = s_turnouts[index_a];
        s_turnouts[index_a] = s_turnouts[index_b];
        s_turnouts[index_b] = tmp;
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void turnout_manager_set_state_by_event(uint64_t event_id, turnout_state_t state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_count; i++) {
        turnout_t *t = &s_turnouts[i];
        
        if (event_id == t->event_normal) {
            t->state = TURNOUT_STATE_NORMAL;
            t->last_update_us = esp_timer_get_time();
            t->command_pending = false;
            ESP_LOGD(TAG, "Turnout '%s' -> NORMAL", t->name);
            
            xSemaphoreGive(s_mutex);
            if (s_state_callback) {
                s_state_callback((int)i, TURNOUT_STATE_NORMAL);
            }
            return;
        }
        
        if (event_id == t->event_reverse) {
            t->state = TURNOUT_STATE_REVERSE;
            t->last_update_us = esp_timer_get_time();
            t->command_pending = false;
            ESP_LOGD(TAG, "Turnout '%s' -> REVERSE", t->name);
            
            xSemaphoreGive(s_mutex);
            if (s_state_callback) {
                s_state_callback((int)i, TURNOUT_STATE_REVERSE);
            }
            return;
        }
    }

    xSemaphoreGive(s_mutex);
    // Event not matched to any turnout - may be discovered by the discovery handler
}

void turnout_manager_set_pending(size_t index, bool pending)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (index < s_count) {
        s_turnouts[index].command_pending = pending;
    }
    xSemaphoreGive(s_mutex);
}

int turnout_manager_find_by_event(uint64_t event_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    for (size_t i = 0; i < s_count; i++) {
        if (s_turnouts[i].event_normal == event_id ||
            s_turnouts[i].event_reverse == event_id) {
            xSemaphoreGive(s_mutex);
            return (int)i;
        }
    }

    xSemaphoreGive(s_mutex);
    return -1;
}

void turnout_manager_check_stale(uint32_t timeout_ms)
{
    if (timeout_ms == 0) return;

    int64_t now_us = esp_timer_get_time();
    int64_t threshold_us = (int64_t)timeout_ms * 1000;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_count; i++) {
        turnout_t *t = &s_turnouts[i];
        
        // Only mark stale if it previously had a valid state and has a timestamp
        if (t->last_update_us > 0 &&
            (t->state == TURNOUT_STATE_NORMAL || t->state == TURNOUT_STATE_REVERSE)) {
            
            if ((now_us - t->last_update_us) > threshold_us) {
                t->state = TURNOUT_STATE_STALE;
                ESP_LOGW(TAG, "Turnout '%s' marked STALE (no update for %lu ms)",
                         t->name, (unsigned long)timeout_ms);
                
                if (s_state_callback) {
                    // Release lock before callback to avoid deadlock
                    xSemaphoreGive(s_mutex);
                    s_state_callback((int)i, TURNOUT_STATE_STALE);
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                }
            }
        }
    }

    xSemaphoreGive(s_mutex);
}

esp_err_t turnout_manager_save(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t ret = turnout_storage_save(s_turnouts, s_count);
    xSemaphoreGive(s_mutex);
    return ret;
}

void turnout_manager_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void turnout_manager_unlock(void)
{
    xSemaphoreGive(s_mutex);
}
