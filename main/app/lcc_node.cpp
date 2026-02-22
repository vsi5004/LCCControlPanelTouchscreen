/**
 * @file lcc_node.cpp
 * @brief LCC/OpenMRN Node Implementation for Turnout Panel
 * 
 * Implements the OpenMRN/LCC stack for the turnout control panel.
 * This node is bidirectional: it produces turnout command events and
 * consumes turnout state feedback (ProducerIdentified, EventReport).
 */

#include "lcc_node.h"
#include "lcc_config.hxx"
#include "bootloader_hal.h"
#include "turnout_manager.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_timer.h"

#include "openlcb/SimpleStack.hxx"
#include "openlcb/SimpleNodeInfoDefs.hxx"
#include "openlcb/ConfigUpdateFlow.hxx"
#include "openlcb/EventHandlerTemplates.hxx"
#include "utils/ConfigUpdateListener.hxx"
#include "freertos_drivers/esp32/Esp32HardwareTwai.hxx"
#include "utils/format_utils.hxx"

static const char *TAG = "lcc_node";

namespace {

// ============================================================================
// Internal state
// ============================================================================

static lcc_status_t s_status = LCC_STATUS_UNINITIALIZED;
static openlcb::NodeID s_node_id = 0;
static Esp32HardwareTwai *s_twai = nullptr;
static openlcb::SimpleCanStack *s_stack = nullptr;
static openlcb::ConfigDef *s_cfg = nullptr;

/// Cached CDI config values
static uint16_t s_screen_timeout_sec = openlcb::DEFAULT_SCREEN_TIMEOUT_SEC;
static uint16_t s_stale_timeout_sec = openlcb::DEFAULT_STALE_TIMEOUT_SEC;
static uint16_t s_query_pace_ms = openlcb::DEFAULT_QUERY_PACE_MS;

/// Config file path
static std::string s_config_path;

/// Custom memory spaces
static class SyncingFileMemorySpace* s_config_space = nullptr;
static class SyncingFileMemorySpace* s_acdi_usr_space = nullptr;

/// Discovery mode state
static bool s_discovery_mode = false;
static lcc_discovery_callback_t s_discovery_callback = nullptr;

// ============================================================================
// Node ID parsing (same as original)
// ============================================================================

static bool parse_node_id(const char *str, openlcb::NodeID *out_id)
{
    if (!str || !out_id) return false;
    while (*str && (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')) str++;

    unsigned int bytes[6];
    if (sscanf(str, "%02x.%02x.%02x.%02x.%02x.%02x",
               &bytes[0], &bytes[1], &bytes[2], 
               &bytes[3], &bytes[4], &bytes[5]) == 6) {
        *out_id = ((uint64_t)bytes[0] << 40) | ((uint64_t)bytes[1] << 32) |
                  ((uint64_t)bytes[2] << 24) | ((uint64_t)bytes[3] << 16) |
                  ((uint64_t)bytes[4] << 8)  | ((uint64_t)bytes[5]);
        return true;
    }

    char *endptr;
    uint64_t val = strtoull(str, &endptr, 16);
    if (endptr != str && val != 0) { *out_id = val; return true; }
    return false;
}

static bool read_node_id_from_file(const char *path, openlcb::NodeID *out_id)
{
    struct stat st;
    if (stat(path, &st) != 0) { ESP_LOGW(TAG, "Node ID file not found: %s", path); return false; }

    FILE *file = fopen(path, "r");
    if (!file) { ESP_LOGE(TAG, "Failed to open node ID file: %s", path); return false; }

    char buf[64];
    size_t read_size = fread(buf, 1, sizeof(buf) - 1, file);
    fclose(file);
    if (read_size == 0) { ESP_LOGE(TAG, "Empty node ID file"); return false; }
    buf[read_size] = '\0';

    if (!parse_node_id(buf, out_id)) {
        ESP_LOGE(TAG, "Invalid node ID format in file: %s", buf);
        return false;
    }
    ESP_LOGI(TAG, "Read node ID from file: %012llx", (unsigned long long)*out_id);
    return true;
}

static void create_default_nodeid_file(const char *path)
{
    ESP_LOGI(TAG, "Creating default nodeid.txt");
    FILE *file = fopen(path, "w");
    if (!file) { ESP_LOGE(TAG, "Failed to create nodeid.txt"); return; }
    fprintf(file, "%02X.%02X.%02X.%02X.%02X.%02X\n",
            (unsigned)((LCC_DEFAULT_NODE_ID >> 40) & 0xFF),
            (unsigned)((LCC_DEFAULT_NODE_ID >> 32) & 0xFF),
            (unsigned)((LCC_DEFAULT_NODE_ID >> 24) & 0xFF),
            (unsigned)((LCC_DEFAULT_NODE_ID >> 16) & 0xFF),
            (unsigned)((LCC_DEFAULT_NODE_ID >> 8) & 0xFF),
            (unsigned)(LCC_DEFAULT_NODE_ID & 0xFF));
    fclose(file);
}

// ============================================================================
// SyncingFileMemorySpace (same as original)
// ============================================================================

class SyncingFileMemorySpace : public openlcb::MemorySpace
{
public:
    SyncingFileMemorySpace(int fd, openlcb::MemorySpace::address_t len)
        : fd_(fd), fileSize_(len) {}

    bool read_only() override { return false; }
    openlcb::MemorySpace::address_t max_address() override { return fileSize_; }

    size_t write(openlcb::MemorySpace::address_t destination, const uint8_t *data,
                 size_t len, errorcode_t *error, Notifiable *again) override
    {
        if (fd_ < 0) { *error = openlcb::Defs::ERROR_PERMANENT; return 0; }
        off_t actual = lseek(fd_, destination, SEEK_SET);
        if ((openlcb::MemorySpace::address_t)actual != destination) {
            *error = openlcb::MemoryConfigDefs::ERROR_OUT_OF_BOUNDS; return 0;
        }
        ssize_t ret = ::write(fd_, data, len);
        if (ret < 0) { *error = openlcb::Defs::ERROR_PERMANENT; return 0; }
        fsync(fd_);
        return ret;
    }

    size_t read(openlcb::MemorySpace::address_t destination, uint8_t *dst,
                size_t len, errorcode_t *error, Notifiable *again) override
    {
        if (fd_ < 0) { *error = openlcb::Defs::ERROR_PERMANENT; return 0; }
        off_t actual = lseek(fd_, destination, SEEK_SET);
        if ((openlcb::MemorySpace::address_t)actual != destination) {
            *error = openlcb::Defs::ERROR_PERMANENT; return 0;
        }
        if (destination >= fileSize_) {
            *error = openlcb::MemoryConfigDefs::ERROR_OUT_OF_BOUNDS; return 0;
        }
        ssize_t ret = ::read(fd_, dst, len);
        if (ret < 0) { *error = openlcb::Defs::ERROR_PERMANENT; return 0; }
        return ret;
    }

private:
    int fd_;
    openlcb::MemorySpace::address_t fileSize_;
};

// ============================================================================
// Turnout Event Handler
// ============================================================================

/**
 * @brief Custom event handler for turnout state consumption
 * 
 * Listens for EventReport and ProducerIdentified messages for all
 * registered turnout event IDs. Routes state updates to the turnout manager.
 * Supports discovery mode for detecting unknown events on the bus.
 */
class TurnoutEventHandler : public openlcb::SimpleEventHandler
{
public:
    TurnoutEventHandler(openlcb::Node *node) : node_(node) {}

    /// Register a turnout event pair for consumption
    /// (With the global listener active, specific registration is not
    ///  strictly needed for receiving events, but we keep the list so
    ///  that handle_identify_consumer/global can report them.)
    void register_turnout(uint64_t event_normal, uint64_t event_reverse)
    {
        registered_events_.push_back(event_normal);
        registered_events_.push_back(event_reverse);
    }

    /// Unregister all events
    void unregister_all()
    {
        registered_events_.clear();
    }

    /// Register a global catch-all listener so we receive ALL events.
    /// Called once at init. This is safe because it runs on the same
    /// thread that creates the stack, before the executor starts or
    /// right after it starts from the same context.
    void register_global_listener()
    {
        openlcb::EventRegistry::instance()->register_handler(
            openlcb::EventRegistryEntry(this, 0), 64);
        ESP_LOGI(TAG, "Global event listener registered");
    }

    /// Handle an event report (someone sent a turnout command or state update)
    void handle_event_report(const openlcb::EventRegistryEntry &entry,
                            openlcb::EventReport *event,
                            BarrierNotifiable *done) override
    {
        AutoNotify n(done);
        route_event(event->event);
    }

    /// Handle ProducerIdentified - learn state from producing nodes.
    /// Producers respond to IdentifyProducer with ProducerIdentified for
    /// BOTH the normal and reverse events. Only the VALID one indicates
    /// the actual current state; the INVALID one is the inactive event.
    void handle_producer_identified(const openlcb::EventRegistryEntry &entry,
                                   openlcb::EventReport *event,
                                   BarrierNotifiable *done) override
    {
        AutoNotify n(done);
        // Only act on the VALID (active) producer state.
        if (event->state != openlcb::EventState::VALID) return;
        route_event(event->event);
    }

    /// Handle identify consumer - respond that we consume these events
    void handle_identify_consumer(const openlcb::EventRegistryEntry &entry,
                                 openlcb::EventReport *event,
                                 BarrierNotifiable *done) override
    {
        AutoNotify n(done);
        // Report that we consume this event (state unknown from our perspective)
        event->event_write_helper<1>()->WriteAsync(
            node_, openlcb::Defs::MTI_CONSUMER_IDENTIFIED_UNKNOWN,
            openlcb::WriteHelper::global(),
            openlcb::eventid_to_buffer(entry.event),
            done->new_child());
    }

    /// Handle identify global - report all consumed events
    void handle_identify_global(const openlcb::EventRegistryEntry &entry,
                               openlcb::EventReport *event,
                               BarrierNotifiable *done) override
    {
        AutoNotify n(done);
        if (event->dst_node && event->dst_node != node_) return;
        
        event->event_write_helper<1>()->WriteAsync(
            node_, openlcb::Defs::MTI_CONSUMER_IDENTIFIED_UNKNOWN,
            openlcb::WriteHelper::global(),
            openlcb::eventid_to_buffer(entry.event),
            done->new_child());
    }

private:
    void route_event(uint64_t event_id)
    {
        // Try to match to a known turnout
        int idx = turnout_manager_find_by_event(event_id);
        if (idx >= 0) {
            // Determine state from which event was received
            turnout_t t;
            if (turnout_manager_get_by_index(idx, &t) == ESP_OK) {
                if (event_id == t.event_normal) {
                    turnout_manager_set_state_by_event(event_id, TURNOUT_STATE_NORMAL);
                } else {
                    turnout_manager_set_state_by_event(event_id, TURNOUT_STATE_REVERSE);
                }
            }
        } else if (s_discovery_mode && s_discovery_callback) {
            // Unknown event in discovery mode - report it
            s_discovery_callback(event_id, 0);
        }
    }

    openlcb::Node *node_;
    std::vector<uint64_t> registered_events_;
};

static TurnoutEventHandler *s_event_handler = nullptr;

// ============================================================================
// Config listener
// ============================================================================

class LccConfigListener : public DefaultConfigUpdateListener
{
public:
    UpdateAction apply_configuration(
        int fd, bool initial_load, BarrierNotifiable *done) override
    {
        AutoNotify n(done);
        
        s_screen_timeout_sec = s_cfg->seg().panel().screen_timeout_sec().read(fd);
        s_stale_timeout_sec = s_cfg->seg().panel().stale_timeout_sec().read(fd);
        s_query_pace_ms = s_cfg->seg().panel().query_pace_ms().read(fd);
        
        if (initial_load) {
            ESP_LOGI(TAG, "Panel config: screen_timeout=%u sec, stale_timeout=%u sec, query_pace=%u ms",
                     s_screen_timeout_sec, s_stale_timeout_sec, s_query_pace_ms);
        }
        return UPDATED;
    }

    void factory_reset(int fd) override
    {
        ESP_LOGI(TAG, "Factory reset - restoring defaults");
        s_cfg->userinfo().name().write(fd, "LCC Turnout Panel");
        s_cfg->userinfo().description().write(fd, "ESP32-S3 Touch LCD Turnout Controller");
        
        s_cfg->seg().panel().screen_timeout_sec().write(fd, openlcb::DEFAULT_SCREEN_TIMEOUT_SEC);
        s_cfg->seg().panel().stale_timeout_sec().write(fd, openlcb::DEFAULT_STALE_TIMEOUT_SEC);
        s_cfg->seg().panel().query_pace_ms().write(fd, openlcb::DEFAULT_QUERY_PACE_MS);
        
        s_screen_timeout_sec = openlcb::DEFAULT_SCREEN_TIMEOUT_SEC;
        s_stale_timeout_sec = openlcb::DEFAULT_STALE_TIMEOUT_SEC;
        s_query_pace_ms = openlcb::DEFAULT_QUERY_PACE_MS;
        
        fsync(fd);
    }
};

static LccConfigListener *s_config_listener = nullptr;

} // anonymous namespace

// ============================================================================
// OpenMRN required external symbols
// ============================================================================

static const char LCC_CONFIG_FILE[] = "/sdcard/openmrn_config";

namespace openlcb {

extern const SimpleNodeStaticValues SNIP_STATIC_DATA = {
    4,                                    // version
    "IvanBuilds",                         // manufacturer_name
    "LCC Turnout Panel",                  // model_name
    "ESP32S3 TouchLCD 4.3",              // hardware_version
    "2.0.0"                               // software_version
};

const char CDI_DATA[] =
    R"xmldata(<?xml version="1.0"?>
<cdi xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://openlcb.org/schema/cdi/1/1/cdi.xsd">
<identification>
  <manufacturer>IvanBuilds</manufacturer>
  <model>LCC Turnout Panel</model>
  <hardwareVersion>Waveshare ESP32-S3 Touch LCD 4.3B</hardwareVersion>
  <softwareVersion>2.0.0</softwareVersion>
</identification>
<acdi/>
<segment space="251" origin="1">
  <group>
    <name>User Info</name>
    <string size="63"><name>User Name</name></string>
    <string size="64"><name>User Description</name></string>
  </group>
</segment>
<segment space="253" origin="128">
  <group offset="4">
    <name>Panel Configuration</name>
    <int size="2">
      <name>Screen Backlight Timeout (seconds)</name>
      <description>Time in seconds before the screen backlight turns off when idle. Touch the screen to wake. Set to 0 to disable (always on). Range: 0 or 10-3600 seconds. Default: 60 seconds.</description>
      <min>0</min>
      <max>3600</max>
      <default>60</default>
    </int>
    <int size="2">
      <name>Stale Timeout (seconds)</name>
      <description>Time in seconds before a turnout is marked STALE if no state update is received. Set to 0 to disable. Default: 300 seconds (5 minutes).</description>
      <min>0</min>
      <max>3600</max>
      <default>300</default>
    </int>
    <int size="2">
      <name>Query Pace (milliseconds)</name>
      <description>Minimum interval in milliseconds between turnout state queries during refresh. Lower values are faster but generate more bus traffic. Range: 20-1000 ms. Default: 100 ms.</description>
      <min>20</min>
      <max>1000</max>
      <default>100</default>
    </int>
  </group>
</segment>
</cdi>)xmldata";

const char *const CONFIG_FILENAME = LCC_CONFIG_FILE;
const size_t CONFIG_FILE_SIZE = ConfigDef::size() + 128;
const char *const SNIP_DYNAMIC_FILENAME = LCC_CONFIG_FILE;

} // namespace openlcb

// ============================================================================
// C API implementation
// ============================================================================

extern "C" {

esp_err_t lcc_node_init(const lcc_config_t *config)
{
    if (s_status != LCC_STATUS_UNINITIALIZED) {
        ESP_LOGW(TAG, "LCC node already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_status = LCC_STATUS_INITIALIZING;

    lcc_config_t cfg;
    if (config) { cfg = *config; } else { cfg = LCC_CONFIG_DEFAULT(); }

    ESP_LOGI(TAG, "Initializing LCC turnout panel node...");
    ESP_LOGI(TAG, "  Node ID file: %s", cfg.nodeid_path);
    ESP_LOGI(TAG, "  TWAI RX: GPIO%d, TX: GPIO%d", cfg.twai_rx_gpio, cfg.twai_tx_gpio);

    s_config_path = cfg.config_path;

    // Read node ID from SD card
    if (!read_node_id_from_file(cfg.nodeid_path, &s_node_id)) {
        ESP_LOGW(TAG, "Using default node ID: %012llx", (unsigned long long)LCC_DEFAULT_NODE_ID);
        s_node_id = LCC_DEFAULT_NODE_ID;
        create_default_nodeid_file(cfg.nodeid_path);
    }
    ESP_LOGI(TAG, "Node ID: %012llx", (unsigned long long)s_node_id);

    s_cfg = new openlcb::ConfigDef(0);

    // Initialize TWAI hardware
    ESP_LOGI(TAG, "Initializing TWAI hardware...");
    s_twai = new Esp32HardwareTwai(cfg.twai_rx_gpio, cfg.twai_tx_gpio, true);
    s_twai->hw_init();

    // Create OpenMRN stack
    ESP_LOGI(TAG, "Creating OpenMRN stack...");
    s_stack = new openlcb::SimpleCanStack(s_node_id);
    
    s_config_listener = new LccConfigListener();

    // Create config file
    ESP_LOGI(TAG, "Checking config file...");
    int config_fd = s_stack->create_config_file_if_needed(
        s_cfg->seg().internal_config(),
        openlcb::CANONICAL_VERSION,
        openlcb::CONFIG_FILE_SIZE);
    
    if (config_fd < 0) {
        ESP_LOGE(TAG, "Failed to create/open config file");
        s_status = LCC_STATUS_ERROR;
        return ESP_FAIL;
    }
    fsync(config_fd);

    // Create turnout event handler
    s_event_handler = new TurnoutEventHandler(s_stack->node());

    // Register global event listener BEFORE starting executor to avoid races
    s_event_handler->register_global_listener();

    // Add CAN port
    ESP_LOGI(TAG, "Adding CAN port...");
    s_stack->add_can_port_select("/dev/twai/twai0");

    // Start executor
    ESP_LOGI(TAG, "Starting executor thread...");
    s_stack->start_executor_thread("lcc_exec", 5, 4096);

    // Register custom memory spaces
    s_config_space = new SyncingFileMemorySpace(config_fd, openlcb::CONFIG_FILE_SIZE);
    s_stack->memory_config_handler()->registry()->insert(
        s_stack->node(), openlcb::MemoryConfigDefs::SPACE_CONFIG, s_config_space);
    
    s_acdi_usr_space = new SyncingFileMemorySpace(config_fd, 128);
    s_stack->memory_config_handler()->registry()->insert(
        s_stack->node(), openlcb::MemoryConfigDefs::SPACE_ACDI_USR, s_acdi_usr_space);

    s_status = LCC_STATUS_RUNNING;
    ESP_LOGI(TAG, "LCC turnout panel node initialized and running");
    return ESP_OK;
}

lcc_status_t lcc_node_get_status(void)
{
    return s_status;
}

uint64_t lcc_node_get_node_id(void)
{
    return s_node_id;
}

uint16_t lcc_node_get_screen_timeout_sec(void)
{
    return s_screen_timeout_sec;
}

uint16_t lcc_node_get_stale_timeout_sec(void)
{
    return s_stale_timeout_sec;
}

uint16_t lcc_node_get_query_pace_ms(void)
{
    return s_query_pace_ms;
}

esp_err_t lcc_node_send_event(uint64_t event_id)
{
    if (s_status != LCC_STATUS_RUNNING || !s_stack) {
        ESP_LOGW(TAG, "LCC node not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Sending event: %016llx", (unsigned long long)event_id);
    s_stack->send_event(event_id);
    return ESP_OK;
}

esp_err_t lcc_node_register_turnout_events(uint64_t event_normal, uint64_t event_reverse)
{
    if (!s_event_handler) return ESP_ERR_INVALID_STATE;
    
    s_event_handler->register_turnout(event_normal, event_reverse);
    ESP_LOGD(TAG, "Registered turnout events: N=%016llx R=%016llx",
             (unsigned long long)event_normal, (unsigned long long)event_reverse);
    return ESP_OK;
}

void lcc_node_unregister_all_turnout_events(void)
{
    if (s_event_handler) {
        s_event_handler->unregister_all();
        ESP_LOGI(TAG, "Unregistered all turnout events");
    }
}

void lcc_node_query_all_turnout_states(void)
{
    if (s_status != LCC_STATUS_RUNNING || !s_stack) return;

    // Query states in a background task to avoid blocking
    // We iterate turnouts and send IdentifyProducer for each event
    size_t count = turnout_manager_get_count();
    uint16_t pace_ms = s_query_pace_ms;

    ESP_LOGI(TAG, "Querying state for %d turnouts (pace=%u ms)", (int)count, pace_ms);

    for (size_t i = 0; i < count; i++) {
        turnout_t t;
        if (turnout_manager_get_by_index(i, &t) == ESP_OK) {
            // Send IdentifyProducer (NOT EventReport!) for each event.
            // MTI_PRODUCER_IDENTIFY asks "who produces this event?" and
            // producers respond with ProducerIdentified carrying state info.
            // This does NOT trigger turnout movement.
            auto *b1 = s_stack->node()->iface()->global_message_write_flow()->alloc();
            b1->data()->reset(openlcb::Defs::MTI_PRODUCER_IDENTIFY,
                              s_stack->node()->node_id(),
                              openlcb::eventid_to_buffer(t.event_normal));
            s_stack->node()->iface()->global_message_write_flow()->send(b1);
            vTaskDelay(pdMS_TO_TICKS(pace_ms / 2));

            auto *b2 = s_stack->node()->iface()->global_message_write_flow()->alloc();
            b2->data()->reset(openlcb::Defs::MTI_PRODUCER_IDENTIFY,
                              s_stack->node()->node_id(),
                              openlcb::eventid_to_buffer(t.event_reverse));
            s_stack->node()->iface()->global_message_write_flow()->send(b2);
            vTaskDelay(pdMS_TO_TICKS(pace_ms / 2));
        }
    }

    ESP_LOGI(TAG, "State query complete for %d turnouts", (int)count);
}

void lcc_node_query_turnout_state(uint64_t event_normal, uint64_t event_reverse)
{
    if (s_status != LCC_STATUS_RUNNING || !s_stack) return;

    auto *b1 = s_stack->node()->iface()->global_message_write_flow()->alloc();
    b1->data()->reset(openlcb::Defs::MTI_PRODUCER_IDENTIFY,
                      s_stack->node()->node_id(),
                      openlcb::eventid_to_buffer(event_normal));
    s_stack->node()->iface()->global_message_write_flow()->send(b1);

    auto *b2 = s_stack->node()->iface()->global_message_write_flow()->alloc();
    b2->data()->reset(openlcb::Defs::MTI_PRODUCER_IDENTIFY,
                      s_stack->node()->node_id(),
                      openlcb::eventid_to_buffer(event_reverse));
    s_stack->node()->iface()->global_message_write_flow()->send(b2);

    ESP_LOGI(TAG, "Queried state for turnout events %016llx / %016llx",
             (unsigned long long)event_normal, (unsigned long long)event_reverse);
}

void lcc_node_set_discovery_mode(bool enabled)
{
    s_discovery_mode = enabled;
    ESP_LOGI(TAG, "Discovery mode %s", enabled ? "enabled" : "disabled");
}

bool lcc_node_is_discovery_mode(void)
{
    return s_discovery_mode;
}

void lcc_node_set_discovery_callback(lcc_discovery_callback_t cb)
{
    s_discovery_callback = cb;
}

void lcc_node_request_bootloader(void)
{
    ESP_LOGI(TAG, "Bootloader mode requested");
    bootloader_hal_request_reboot();
}

void lcc_node_shutdown(void)
{
    if (s_status == LCC_STATUS_UNINITIALIZED) return;
    ESP_LOGI(TAG, "Shutting down LCC node...");
    s_status = LCC_STATUS_UNINITIALIZED;
}

} // extern "C"
