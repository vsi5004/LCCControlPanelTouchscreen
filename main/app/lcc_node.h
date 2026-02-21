/**
 * @file lcc_node.h
 * @brief LCC/OpenMRN Node Interface for Turnout Panel (C-compatible header)
 * 
 * Provides the C interface for OpenMRN/LCC node initialization and operations.
 * The panel acts as both an event producer (sending turnout commands) and an
 * event consumer (listening for turnout state feedback).
 */

#ifndef LCC_NODE_H_
#define LCC_NODE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Default LCC node ID if nodeid.txt is not present
 */
#define LCC_DEFAULT_NODE_ID 0x050101019F6000ULL

/**
 * @brief LCC Node status
 */
typedef enum {
    LCC_STATUS_UNINITIALIZED = 0,
    LCC_STATUS_INITIALIZING,
    LCC_STATUS_RUNNING,
    LCC_STATUS_ERROR,
} lcc_status_t;

/**
 * @brief LCC initialization configuration
 */
typedef struct {
    const char *nodeid_path;        /**< Path to node ID file on SD card */
    const char *config_path;        /**< Path to config file (for OpenMRN EEPROM emulation) */
    int twai_rx_gpio;               /**< TWAI RX GPIO pin */
    int twai_tx_gpio;               /**< TWAI TX GPIO pin */
} lcc_config_t;

/**
 * @brief Default LCC configuration
 */
#define LCC_CONFIG_DEFAULT() { \
    .nodeid_path = "/sdcard/nodeid.txt", \
    .config_path = "/sdcard/lcc_config.bin", \
    .twai_rx_gpio = 16, \
    .twai_tx_gpio = 15, \
}

/**
 * @brief Initialize the LCC node
 * 
 * Reads node ID from SD card, initializes TWAI (CAN) hardware,
 * and starts the OpenMRN stack.
 * 
 * @param config Configuration options
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t lcc_node_init(const lcc_config_t *config);

/**
 * @brief Get the current LCC node status
 */
lcc_status_t lcc_node_get_status(void);

/**
 * @brief Get the node ID
 * 
 * @return 48-bit node ID, or 0 if not initialized
 */
uint64_t lcc_node_get_node_id(void);

/**
 * @brief Get screen backlight timeout from CDI config
 * 
 * @return Timeout in seconds (0 = disabled)
 */
uint16_t lcc_node_get_screen_timeout_sec(void);

/**
 * @brief Get stale timeout from CDI config
 * 
 * @return Timeout in seconds (0 = disabled)
 */
uint16_t lcc_node_get_stale_timeout_sec(void);

/**
 * @brief Get query pace from CDI config
 * 
 * @return Pace in milliseconds
 */
uint16_t lcc_node_get_query_pace_ms(void);

// ----- Turnout Event Operations -----

/**
 * @brief Send a turnout command event
 * 
 * Produces the given event ID on the LCC bus. Use the turnout's
 * event_normal or event_reverse ID to command the turnout.
 * 
 * @param event_id The full 64-bit LCC event ID to send
 * @return ESP_OK on success
 */
esp_err_t lcc_node_send_event(uint64_t event_id);

/**
 * @brief Register turnout event IDs for consumption
 * 
 * Tells the LCC stack to listen for the given event pair.
 * When either event is seen (EventReport or ProducerIdentified),
 * the turnout manager will be notified.
 * 
 * @param event_normal Event ID for NORMAL/CLOSED state
 * @param event_reverse Event ID for REVERSE/THROWN state
 * @return ESP_OK on success
 */
esp_err_t lcc_node_register_turnout_events(uint64_t event_normal, uint64_t event_reverse);

/**
 * @brief Unregister all turnout event listeners
 * 
 * Call before re-registering after turnout list changes.
 */
void lcc_node_unregister_all_turnout_events(void);

/**
 * @brief Query state of all registered turnouts
 * 
 * Sends IdentifyProducer messages for all registered turnout events,
 * paced by the configured query_pace_ms to avoid bus flooding.
 * Runs asynchronously — state updates arrive via the event handler.
 */
void lcc_node_query_all_turnout_states(void);

/**
 * @brief Set discovery mode on/off
 * 
 * When enabled, unknown events seen on the bus will be reported
 * to the discovery callback (if set).
 * 
 * @param enabled true to enable discovery mode
 */
void lcc_node_set_discovery_mode(bool enabled);

/**
 * @brief Check if discovery mode is active
 */
bool lcc_node_is_discovery_mode(void);

/**
 * @brief Discovery callback type
 * 
 * Called when an unknown event is observed during discovery mode.
 * The event_id is the observed event, and state indicates whether
 * it was a ProducerIdentified VALID (NORMAL) or INVALID (REVERSE).
 */
typedef void (*lcc_discovery_callback_t)(uint64_t event_id, uint8_t state);

/**
 * @brief Set the discovery callback
 * 
 * @param cb Callback function, or NULL to unregister
 */
void lcc_node_set_discovery_callback(lcc_discovery_callback_t cb);

/**
 * @brief Request reboot into bootloader mode for firmware update
 */
void lcc_node_request_bootloader(void);

/**
 * @brief Shutdown the LCC node
 */
void lcc_node_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // LCC_NODE_H_
