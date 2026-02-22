# ESP32-S3 LCC Turnout Control Panel — Specification

## 1. Purpose and Scope

This project implements an ESP32-S3–based LCC turnout control panel with a touch LCD user interface.

The device:
- Connects to an LCC/OpenLCB CAN bus using OpenMRN
- Sends and receives turnout control events
- Provides a touchscreen interface for turnout monitoring and control
- Stores configuration and turnout definitions on an SD card
- Uses ESP-IDF v5.1.6, FreeRTOS, LVGL, and OpenMRN

Out of scope:
- Acting as a turnout decoder (motor driver)
- Direct servo/motor control
- Signal or lighting control

---

## 2. Target Platform and Constraints

### Hardware
- Board: Waveshare ESP32-S3 Touch LCD 4.3B
- MCU: ESP32-S3
- Display: RGB LCD with backlight via CH422G I/O expander
- Touch: Capacitive
- Storage: SD card (SPI, CS via CH422G)
- CAN: LCC bus via onboard CAN transceiver

### Software Stack
- ESP-IDF: v5.1.6 (mandatory — see Build Notes below)
- RTOS: FreeRTOS
- UI: LVGL 8.x (via ESP-IDF component registry)
- LCC: OpenMRN (git submodule at `components/OpenMRN`)
- Filesystem: FATFS on SD card
- Image decoding: esp_jpeg (ESP-IDF component)

### Build Notes

**ESP-IDF Version Constraint**: This project requires **ESP-IDF 5.1.6** (GCC 12.2.0). 
ESP-IDF 5.3+ (GCC 13.x) and 5.4+ (GCC 14.x) have a newlib/libstdc++ incompatibility 
that causes OpenMRN compilation failures with errors in `<bits/char_traits.h>` 
related to `std::construct_at` and `std::pointer_traits::pointer_to`.

**Windows Symlink Fix**: On Windows, the files in `components/OpenMRN/include/esp-idf/` 
may appear as plain text files containing relative paths (broken Unix symlinks). 
These must be converted to proper C `#include` wrapper files. Example:
```c
// components/OpenMRN/include/esp-idf/openmrn_features.h
// Wrapper for esp-idf platform - include parent openmrn_features.h
#include "../openmrn_features.h"
```

**WiFi Driver Exclusion**: `EspIdfWiFi.cxx` uses ESP-IDF 5.3+ APIs (`channel_bitmap`, 
`WIFI_EVENT_HOME_CHANNEL_CHANGE`) and must be excluded from the build in 
`components/OpenMRN/CMakeLists.txt` when using ESP-IDF 5.1.6.

**FAT Long Filename Support**: `turnouts.json` requires FAT LFN support. Enable in 
`sdkconfig`:
```
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=255
```

### Hard Constraints
- LVGL calls must only occur from the UI task
- SD card I/O must not occur in the UI task
- CAN traffic must obey defined rate limits
- Turnout file writes must be atomic

---

## 3. LCC Event Model (Authoritative)

### 3.1 Turnout Event Pairs

Each turnout is defined by two 64-bit LCC event IDs:
- **Normal Event**: Produced when closing turnout, consumed to detect closed state
- **Reverse Event**: Produced when throwing turnout, consumed to detect thrown state

Event IDs are fully user-configurable per turnout and stored in `turnouts.json`.

### 3.2 Event Consumption

The panel acts as an event consumer for state feedback:

| Message Type | Action |
|-------------|--------|
| EventReport | Update turnout state (Normal or Reverse) based on which event was received |
| ProducerIdentified (valid) | Update turnout state from query response (only VALID state is processed; INVALID responses are ignored) |
| IdentifyConsumer | Respond with ConsumerIdentified for registered events |
| IdentifyGlobal | Respond with ConsumerIdentified for all registered events |

### 3.3 Event Production

When the user taps a turnout tile, the panel sends a single event:
- Current state NORMAL → send Reverse event (to throw)
- Current state REVERSE, UNKNOWN, or STALE → send Normal event (to close)

### 3.4 State Query Protocol

On startup, the panel queries turnout positions:
1. For each registered turnout, send `IdentifyConsumer` for both Normal and Reverse events
2. Queries are paced at configurable interval (default 100ms) to avoid bus congestion
3. Turnout decoders respond with `ProducerIdentified` messages (one per event, with VALID or INVALID state)
4. Only `ProducerIdentified` messages with `EventState::VALID` are used to update tile state; `INVALID` responses are discarded
5. Panel updates tile colors based on responses

### 3.5 Discovery Mode

When enabled, the panel captures all `EventReport` messages on the bus regardless
of registration. This allows users to identify event IDs by operating turnouts
manually and observing which events appear.

---

## 4. Configuration Files

### File: `nodeid.txt` (SD Card)

Plain text file containing the 6-byte LCC node ID in dotted hex format:
```
05.01.01.01.9F.60.00
```

Rules:
- Read at boot before LCC initialization
- Missing file causes SD card error screen
- Changes require device restart

### LCC Configuration (CDI/ACDI)

The device uses OpenMRN's CDI (Configuration Description Information) for:
- **Panel Settings**: Screen timeout, stale timeout, query pace
- **User Name/Description**: Stored in ACDI user space (space 251)

**CDI Memory Layout (PanelConfig):**
| Offset | Size | Content |
|--------|------|---------|
| 0 | 4 | InternalConfigData (version, etc.) |
| 4 | 2 | Screen Backlight Timeout (seconds, 0=disabled, 10-3600) |
| 6 | 2 | Stale Timeout (seconds, 0=disabled, 0-65535) |
| 8 | 2 | Query Pace (milliseconds, 10-5000) |

**Panel Configuration:**
| Setting | Default | Range | Description |
|---------|---------|-------|-------------|
| Screen Timeout | 60 | 0, 10-3600 | Backlight timeout in seconds (0=disabled) |
| Stale Timeout | 300 | 0-65535 | Seconds without update before turnout marked stale (0=disabled) |
| Query Pace | 100 | 10-5000 | Milliseconds between event queries at startup |

**SNIP (Simple Node Information Protocol):**
- Manufacturer: "IvanBuilds"
- Model: "LCC Turnout Panel"
- Hardware Version: "ESP32S3 LCD 4.3B"
- Software Version: "2.0.0"

**Implementation Note:** User info (name/description) uses `space="251"` (ACDI user
space) with `origin="1"` to avoid conflicts with manufacturer info at origin 0.

---

## 5. Turnout File Format

### File: `turnouts.json`

```json
{
  "version": 1,
  "turnouts": [
    {
      "name": "Main Yard Lead",
      "event_normal": "05.01.01.01.40.00.00.00",
      "event_reverse": "05.01.01.01.40.00.00.01"
    },
    {
      "name": "Siding Switch",
      "event_normal": "05.01.01.01.40.00.01.00",
      "event_reverse": "05.01.01.01.40.00.01.01"
    }
  ]
}
```

Rules:
- Event IDs are 8-byte values in dotted hex format
- Turnout names should be unique (for user clarity)
- Maximum 150 turnouts supported
- Writes must be atomic (full file rewrite)
- Version mismatches must be detected

---

## 6. Functional Requirements

### Boot

#### FR-001
Display splashscreen.jpg from SD at boot using esp_jpeg decoder.
AC: Displayed within 1500 ms or fallback shown.

#### FR-002
Initialize OpenMRN using Node ID from SD card `/sdcard/nodeid.txt`.
AC: Node visible on LCC network with configured ID.

#### FR-003
Transition to main UI within 5 s of LCC readiness or timeout.

#### FR-004
If SD card is not detected at boot, display error screen with:
- Warning icon and "SD Card Not Detected" title
- Instructions listing required files (nodeid.txt, turnouts.json)
- Screen persists until device restart

**Implementation Note:** SD card retry is not performed after error screen is shown
because `waveshare_sd_init()` reinitializes CH422G and SPI, which interferes with
the RGB LCD display operation. User must insert card and restart device.

#### FR-005
Screen backlight timeout for power saving:
- Configurable timeout via LCC configuration (default: 60 seconds)
- Set to 0 to disable timeout (always on)
- Minimum enabled timeout: 10 seconds, maximum: 3600 seconds
- Touch-to-wake: Any touch restores backlight immediately
- Backlight is on/off only (not dimmable - hardware limitation)

AC: Backlight turns off after configured idle period; touch wakes screen.

#### FR-006
On boot, query all registered turnout positions:
- Send IdentifyConsumer for each registered turnout event
- Pace queries at configurable interval (default 100ms)
- Update tile states as responses arrive

AC: Turnout tiles reflect current positions within a few seconds of boot.

### Main UI

#### FR-010
Provide two tabs: Turnouts (left) and Add Turnout (right).
Turnouts tab is the default tab shown on startup.

### Turnout Switchboard

#### FR-020
Display a grid of color-coded turnout tiles loaded from turnouts.json.
- Tiles are 150×110 pixels in a flex-wrap layout
- Each tile shows three rows: turnout name (top), current state text (center), edit/delete buttons (bottom)
- State text uses "CLOSED" / "THROWN" / "UNKNOWN" / "STALE"
- Green = Closed, Yellow = Thrown, Grey = Unknown, Red = Stale

AC: All registered turnouts are shown with correct color coding.

#### FR-021
Tapping a turnout tile toggles its state:
- If NORMAL → send Reverse event
- If REVERSE/UNKNOWN/STALE → send Normal event
- Mark tile as pending (blue border) until state feedback received

AC: Tile sends correct event and shows pending indicator.

#### FR-022
Update tile state in real-time as EventReport and ProducerIdentified messages arrive.
AC: Tile color changes within one LVGL refresh cycle of event reception.

#### FR-023
Mark turnouts as STALE when no state update received within configurable timeout.
AC: Tile turns red after stale timeout expires without any state update.

#### FR-024
Provide inline rename for each turnout via an edit icon button on the tile:
- Tapping the edit icon opens a full-screen modal with a text input pre-filled with the current name
- On-screen keyboard for editing
- Save persists the new name to SD card and updates the tile label
- Cancel closes the modal without changes

AC: Renamed turnout is saved to turnouts.json and tile label updates immediately.

#### FR-025
Provide inline delete for each turnout via a trash icon button on the tile:
- Tapping the trash icon opens a confirmation dialog with warning styling
- Dialog shows turnout name and "This action cannot be undone" message
- Confirm removes the turnout, unregisters its LCC events, saves to SD card, and refreshes the grid
- Cancel closes the dialog without changes

AC: Deleted turnout is removed from turnouts.json and disappears from the grid.

#### FR-026
Support JMRI roster.xml import:
- On startup, if `/sdcard/roster.xml` exists, parse the OlcbTurnoutManager section
- Extract turnout definitions (systemName event pairs + userName)
- Respect the `inverted` attribute (swap Normal/Reverse events when true)
- Skip turnouts whose event IDs already exist in the turnout list
- Append newly discovered turnouts and save the merged list to turnouts.json

AC: Turnouts defined in JMRI roster.xml appear on the Turnouts tab after reboot with the file on SD card.

### Add Turnout

#### FR-030
Provide manual turnout entry form with:
- Name text input
- Normal Event ID text input (dotted hex format)
- Reverse Event ID text input (dotted hex format)
- On-screen keyboard for text entry
- Add button to save turnout

AC: Valid turnout is added to turnouts.json and appears on Turnouts tab.

#### FR-031
Provide discovery mode toggle:
- When enabled, capture all EventReport messages on the bus
- Display discovered events in a scrollable list
- User can select events from the list to populate the form

AC: Events seen on bus appear in discovery list within 1 second.

### CAN Rate Limiting

#### FR-050
Minimum transmission interval: configurable (default 20ms).

---

### Firmware Update (OTA)

#### FR-060
Device shall support over-the-air firmware updates via LCC Memory Configuration Protocol.
- Uses OpenLCB Firmware Upgrade Standard (memory space 0xEF)
- Compatible with JMRI Firmware Update tool
- Compatible with OpenMRN bootloader_client command-line tool

AC: Firmware update initiated from JMRI completes successfully and device boots new firmware.

#### FR-061
Firmware update shall use ESP-IDF OTA partition scheme.
- Dual OTA partitions (ota_0, ota_1) for safe updates
- Automatic rollback on boot failure (via esp_ota_mark_app_valid_cancel_rollback)
- USB/UART flashing remains available for development

AC: Failed firmware update can be recovered via USB flashing.

#### FR-062
Device shall validate firmware before accepting update.
- ESP32 chip ID validation (rejects firmware built for wrong chip variant)
- Image magic byte verification
- ESP-IDF OTA APIs handle checksum validation

AC: Firmware for ESP32 (non-S3) is rejected with error.

#### FR-063
Device shall enter bootloader mode when requested via LCC.
- "Enter Bootloader" command received via Memory Configuration datagram
- RTC memory flag set to persist across reboot
- Minimal bootloader mode runs CAN-only (no LCD/UI)

AC: Device reboots into bootloader mode within 1 second of command.

#### FR-064
Bootloader mode shall provide visual/log feedback.
- Serial console logs bootloader state
- Log messages indicate firmware receive progress
- Automatic reboot on completion or timeout

AC: Firmware update progress visible in serial monitor.

---

## 7. Non-Functional Requirements
- UI must not block > 50 ms
- Turnout file save must be power-loss safe
- CAN disconnect must not require reboot
- Maximum 150 turnouts supported
- State updates must reach UI within one LVGL refresh cycle
