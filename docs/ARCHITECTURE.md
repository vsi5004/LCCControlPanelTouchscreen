# Architecture

## 1. Project Structure

```
LCCControlPanelTouchscreen/
├── CMakeLists.txt
├── sdkconfig.defaults
├── lv_conf.h                 # LVGL configuration (root level)
├── components/
│   ├── OpenMRN/              # Git submodule
│   └── board_drivers/        # Hardware abstraction
│       ├── ch422g.c/.h       # I2C expander driver
│       ├── waveshare_lcd.c/.h
│       ├── waveshare_touch.c/.h
│       └── waveshare_sd.c/.h
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml     # LVGL 8.x, esp_lcd_touch, esp_jpeg
│   ├── Kconfig.projbuild
│   ├── main.c                # Entry point, hardware init, SD error screen
│   ├── lv_conf.h             # LVGL configuration (main level)
│   ├── app/                  # Application logic
│   │   ├── lcc_node.cpp/.h   # OpenMRN integration, event prod/consume
│   │   ├── lcc_config.hxx    # CDI configuration (PanelConfig)
│   │   ├── turnout_manager.c/.h  # Thread-safe turnout state management
│   │   ├── turnout_storage.c/.h  # SD card JSON persistence
│   │   ├── screen_timeout.c/.h   # Backlight power saving
│   │   ├── bootloader_hal.cpp/.h # OTA bootloader support
│   │   └── bootloader_display.c/.h # LCD status during OTA updates
│   └── ui/                   # LVGL screens
│       ├── ui_common.c/.h    # LVGL init, mutex, flush callbacks, data types
│       ├── ui_main.c         # Main tabview container (Turnouts + Add Turnout)
│       ├── ui_turnouts.c     # Turnout switchboard grid (color-coded tiles)
│       └── ui_add_turnout.c  # Manual turnout entry + event discovery
├── sdcard/                   # SD card template files
│   ├── nodeid.txt            # LCC node ID
│   └── turnouts.json         # Turnout definitions
└── docs/
```

---

## 2. Task Model

### Tasks

| Task | Priority | Stack | Core | Responsibility |
|------|----------|-------|------|----------------|
| lvgl_task | 2 | 6KB | CPU1 | LVGL rendering via `lv_timer_handler()` |
| openmrn_task | 5 | 8KB | Any | OpenMRN executor loop (event prod/consume) |
| main_task | 1 | 4KB | CPU1 | Hardware init, stale checking, screen timeout |

**CPU Affinity Strategy:**
- **CPU0**: Dedicated to RGB LCD DMA ISRs (bounce buffer transfers)
- **CPU1**: Main task + LVGL rendering (avoids contention with display DMA)

### Task Implementation Notes
- **lvgl_task**: Created by `ui_init()`, runs continuously calling `lv_timer_handler()`
- **openmrn_task**: Created by `lcc_node_init()`, runs OpenMRN's internal executor
- **main_task**: Runs `app_main()`, then enters polling loop for screen timeout + stale checking

---

## 3. Inter-Task Communication

- **LCC → Turnout Manager**: `turnout_manager_set_state_by_event()` called from TurnoutEventHandler
- **Turnout Manager → UI**: State change callback via `lv_async_call()` (packs index+state into uint32)
- **UI → LCC**: `lcc_node_send_event()` called from tile tap handler
- **LVGL mutex**: Required for all LVGL API access from non-UI tasks

---

## 4. State Machines

### Application State

```
BOOT → SPLASH → LCC_INIT → MAIN_UI
         ↓          ↓
     (timeout)  (timeout)
         ↓          ↓
      MAIN_UI ← MAIN_UI (degraded)
```

### Turnout State

Each turnout can be in one of four states:

```
UNKNOWN ──(EventReport/ProducerIdentified)──→ NORMAL or REVERSE
NORMAL  ──(EventReport: reverse event)──────→ REVERSE
REVERSE ──(EventReport: normal event)───────→ NORMAL
ANY     ──(stale timeout exceeded)──────────→ STALE
STALE   ──(EventReport/ProducerIdentified)──→ NORMAL or REVERSE
```

**State Colors:**
| State | Color | Hex |
|-------|-------|-----|
| NORMAL | Green | 0x4CAF50 |
| REVERSE | Yellow | 0xFFC107 |
| UNKNOWN | Grey | 0x9E9E9E |
| STALE | Red | 0xF44336 |
| PENDING | Blue border | 0x2196F3 |

---

## 5. OpenMRN Integration

### Build Configuration

OpenMRN requires ESP-IDF 5.1.6 (GCC 12.2.0) due to newlib/libstdc++ incompatibility 
in GCC 13.x/14.x. The component CMakeLists.txt excludes `EspIdfWiFi.cxx` (uses 
ESP-IDF 5.3+ APIs) since this project uses CAN, not WiFi.

Key compile options:
- C++ Standard: C++14
- `-fno-strict-aliasing` — Required for OpenMRN compatibility
- `-D_GLIBCXX_USE_C99` — C99 compatibility defines
- `-Wno-volatile` — Suppress deprecated volatile warnings

### LVGL Performance Tuning

The following settings optimize scroll and animation performance:

| Setting | Value | Source | Purpose |
|---------|-------|--------|--------|
| `LV_DISP_DEF_REFR_PERIOD` | 10ms | sdkconfig | 100Hz refresh for smooth animations |
| `LV_INDEV_DEF_READ_PERIOD` | 10ms | lv_conf.h | Fast touch polling |
| `LV_INDEV_DEF_SCROLL_THROW` | 5 | lv_conf.h | Reduced scroll momentum |
| `LV_INDEV_DEF_SCROLL_LIMIT` | 30 | lv_conf.h | Lower scroll sensitivity |
| `LV_MEM_CUSTOM` | 1 | sdkconfig | Use stdlib malloc (PSRAM-aware) |
| `LV_MEMCPY_MEMSET_STD` | 1 | sdkconfig | Use optimized libc memory functions |
| `LV_ATTRIBUTE_FAST_MEM` | IRAM | sdkconfig | Place critical functions in IRAM |

**Additional Optimizations:**
- LVGL task pinned to CPU1 to avoid contention with LCD DMA on CPU0
- Fade animations for screen timeout use 20 discrete opacity steps

### CAN Driver
- Uses `Esp32HardwareTwai` from OpenMRN
- VFS path: `/dev/twai/twai0`
- Pins: TX=GPIO15, RX=GPIO16

### Initialization Sequence
1. Read `nodeid.txt` from SD for 12-digit hex Node ID
2. Create `Esp32HardwareTwai` instance
3. Call `twai.hw_init()`
4. Initialize OpenMRN SimpleCanStack
5. Add CAN port via `add_can_port_async("/dev/twai/twai0")`

### TurnoutEventHandler

Custom `openlcb::SimpleEventHandler` subclass that handles:
- **EventReport**: Updates turnout state when events are received on the bus
- **ProducerIdentified**: Updates state from query responses
- **IdentifyConsumer**: Responds to consumer identification requests
- **IdentifyGlobal**: Responds to global identification requests

Events are registered/unregistered dynamically as turnouts are added or removed.

### Power Saving (Screen Timeout)
The `screen_timeout` module provides automatic backlight control with smooth transitions:

| Setting | Default | Range | Description |
|---------|---------|-------|-------------|
| Timeout | 60 sec | 0, 10-3600 | Idle time before backlight off (0=disabled) |
| Fade Duration | 1 sec | Fixed | Visual fade-to-black transition time |

**State Machine:**
```
ACTIVE ──(timeout)──→ FADING_OUT ──(fade complete)──→ OFF
   ↑                      │                            │
   │                      └──(touch)──→ FADING_IN ─────┘
   │                                        │
   └────────────(fade complete)─────────────┘
```

**Implementation:**
- `screen_timeout_init()`: Initialize with CH422G handle and timeout from LCC config
- `screen_timeout_tick()`: Called every 500ms from main loop to check timeout
- `screen_timeout_notify_activity()`: Called from touch callback to reset timer

**Fade Animation:**
- Uses LVGL overlay on `lv_layer_top()` for smooth black fade effect
- 1 second fade-out before backlight turns off
- 1 second fade-in when waking
- Touch during fade-out aborts and transitions to fade-in
- Animation uses `lv_anim` with opacity interpolation (LV_OPA_TRANSP ↔ LV_OPA_COVER)
- **Stepped Opacity**: Uses 20 discrete opacity levels to reduce banding artifacts

**Hardware Limitation:** The CH422G I/O expander provides only digital on/off control
for the backlight pin. PWM dimming is not possible with this hardware design. The fade
effect is achieved via LVGL overlay opacity animation while backlight remains on.

### Event Production & Consumption
- **Production**: `lcc_node_send_event(uint64_t event_id)` — sends turnout commands
- **Consumption**: TurnoutEventHandler processes incoming EventReport and ProducerIdentified
- **Query**: `lcc_node_query_all_turnout_states()` — sends IdentifyConsumer for each registered event at startup
- **Discovery**: `lcc_node_set_discovery_mode()` — enables/disables capturing unknown events for the Add Turnout tab

---

## 6. Turnout Manager

### Thread Safety

The turnout manager uses a FreeRTOS mutex to protect all state access. The LCC
event handler (running on the OpenMRN executor) and the LVGL UI task both access
turnout state through this mutex.

### State Update Flow

```
LCC Event Received
     │
     ▼
TurnoutEventHandler::handle_event_report()
     │
     ▼
turnout_manager_set_state_by_event()  [acquires mutex]
     │
     ▼
state_change_callback(index, new_state)
     │
     ▼
lv_async_call(ui_turnouts_update_tile_async, packed_data)
     │
     ▼
LVGL task unpacks index+state, updates tile color
```

### Stale Detection

The main task periodically calls `turnout_manager_check_stale(timeout_ms)` to mark
turnouts as stale when no state update has been received within the timeout period.
`last_update_us` is tracked per turnout using `esp_timer_get_time()`.

---

## 7. LVGL Thread Safety

All LVGL API calls must occur from the LVGL task context. When modifying UI from 
non-UI tasks, use `lv_async_call()` to schedule updates on the LVGL task.

**Cross-task UI updates** pack data into a `uint32_t` parameter:
- Bits 31-16: turnout index
- Bits 15-0: turnout state enum value

This avoids heap allocation for simple state update notifications.

---

## 8. Firmware Update (OTA) Architecture

### Overview

The device supports over-the-air (OTA) firmware updates via the LCC Memory Configuration
Protocol. This enables firmware updates through JMRI's "Firmware Update" tool or the
OpenMRN `bootloader_client` command-line utility without physical access to the device.

### Boot Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ ESP32 ROM Bootloader                                            │
│ - Always available for USB/UART recovery                        │
├─────────────────────────────────────────────────────────────────┤
│ ESP-IDF Second-Stage Bootloader (0x1000)                        │
│ - Selects active OTA partition (ota_0 or ota_1)                 │
│ - Handles rollback on boot failure                              │
├─────────────────────────────────────────────────────────────────┤
│ Application (ota_0 or ota_1)                                    │
│ ├── Check RTC flag: bootloader_request                          │
│ │   ├── TRUE  → Run bootloader mode (CAN receive loop)          │
│ │   └── FALSE → Normal application startup (UI, LCC, etc.)      │
└─────────────────────────────────────────────────────────────────┘
```

### Partition Table

| Name     | Type | SubType | Offset   | Size    | Purpose                    |
|----------|------|---------|----------|---------|----------------------------|
| nvs      | data | nvs     | 0x9000   | 0x6000  | NVS key-value storage      |
| otadata  | data | ota     | 0xf000   | 0x2000  | OTA boot selection data    |
| phy_init | data | phy     | 0x11000  | 0x1000  | PHY calibration            |
| ota_0    | app  | ota_0   | 0x20000  | 0x1E0000| Application slot A (~1.9MB)|
| ota_1    | app  | ota_1   | 0x200000 | 0x1E0000| Application slot B (~1.9MB)|

### Components

| Component | File | Responsibility |
|-----------|------|----------------|
| Bootloader HAL | `app/bootloader_hal.cpp` | ESP32-specific OTA integration |
| LCC Node | `app/lcc_node.cpp` | Handles "enter bootloader" command |
| OpenMRN | `Esp32BootloaderHal.hxx` | LCC Memory Config Protocol handler |

### Firmware Update Process

1. **JMRI/Client** sends "Enter Bootloader" datagram to target node
2. **lcc_node** receives command, calls `bootloader_hal_request_reboot()`
3. **bootloader_hal** sets RTC memory flag, calls `esp_restart()`
4. **app_main** on reboot detects flag, calls `bootloader_hal_run()`
5. **Bootloader mode** runs minimal CAN stack (no LCD, no LVGL)
6. **JMRI/Client** streams firmware binary to memory space 0xEF
7. **Esp32BootloaderHal** validates and writes to alternate OTA partition
8. **On completion** sets new partition as active, reboots into new firmware

### Safety Features

| Feature | Implementation |
|---------|----------------|
| Rollback on failure | ESP-IDF `CONFIG_APP_ROLLBACK_ENABLE` |
| Chip ID validation | Rejects firmware for wrong ESP32 variant |
| Image magic check | Validates ESP-IDF image header |
| USB recovery | ROM bootloader always accessible |
| Partition isolation | New firmware written to inactive slot |

### JMRI Usage

1. Connect JMRI to LCC network (CAN-USB adapter or LCC hub)
2. Open **LCC Menu → Firmware Update**
3. Select target node by Node ID
4. Choose firmware `.bin` file (from `build/LCCControlPanelTouchscreen.bin`)
5. Click "Download" to start update
6. Device shows progress on LCD (blue header, status text, progress bar)
7. Device reboots automatically when complete

### Bootloader Display Module

Since this board has no LEDs, the `bootloader_display` module provides visual
feedback during firmware updates:

| Status | Display |
|--------|--------|
| Waiting | "Waiting for firmware..." (white) |
| Receiving | "Receiving firmware" + progress bar (yellow) |
| Writing | "Writing to flash..." + progress bar (orange) |
| Success | "Update successful! Rebooting..." (green) |
| Error | "Update failed!" / "Checksum error!" (red) |

The display uses direct framebuffer rendering with an embedded 8x8 bitmap font,
avoiding the overhead of LVGL during the update process.
