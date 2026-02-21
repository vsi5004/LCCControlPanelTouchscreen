# ESP32 LCC Touchscreen Turnout Panel — High-Level Functional Specification

## Project Overview

Develop an embedded touchscreen control panel using a Waveshare ESP32-S3 4.3" LCD that interfaces with an LCC (OpenLCB) model railroad network. The panel will discover, display, and control layout turnouts defined in JMRI and implemented via hardware LCC turnout controller nodes.

The panel acts as an LCC node that:

* Sends turnout command events (NORMAL/REVERSE)
* Listens for turnout state responses
* Builds and maintains a live model of turnout availability and status
* Provides a touchscreen graphical switchboard UI for operators

This document defines functional behavior only (not implementation details or code).

---

# 1. System Architecture Context

## External Systems

* JMRI running on host PC
* Digitrax CS105 LCC command station
* Hardware LCC turnout controller nodes (servo-based)
* LCC/OpenLCB CAN network

## Data Flow

1. Turnout commands originate from:

   * Touchscreen panel
   * WiThrottle/JMRI
   * Other LCC nodes
2. Turnout state is persisted and produced by turnout controller nodes.
3. JMRI can request turnout state refresh on startup.
4. Touchscreen listens to all LCC traffic and maintains its own turnout state model.

---

# 2. Core Functional Goals

## 2.1 Discover available turnouts dynamically

The touchscreen must build a list of available turnouts automatically by observing LCC traffic.

Sources for discovery:

* Producer Identified responses to state queries
* Command events observed on the bus
* Explicit refresh initiated by JMRI at startup
* Manual refresh initiated from touchscreen UI

Each discovered turnout must include:

* Event ID for NORMAL command
* Event ID for REVERSE command
* Current known state
* Last update timestamp

Turnouts must persist locally once discovered.

---

## 2.2 Maintain accurate turnout state model

Each turnout must maintain:

State enum:

* NORMAL
* REVERSE
* UNKNOWN
* STALE (no update for configurable timeout)

State must update when:

* Producer Identified VALID/INVALID received
* Command observed and followed by confirmed state
* Explicit refresh response received

State must be timestamped.

If no state confirmation received after configurable timeout:

* Mark turnout as STALE

---

## 2.3 Startup initialization behavior

At system startup:

1. Panel enters DISCOVERY MODE
2. Panel listens for turnout state traffic
3. JMRI issues turnout state refresh requests
4. Turnout nodes respond with current persisted state
5. Panel builds turnout table

Discovery window default: ~10–20 seconds
After window:

* Any turnout seen is added to list
* Unknown turnouts remain discoverable later

User may manually trigger rediscovery.

---

## 2.4 Touchscreen control functionality

### Basic turnout control

For each turnout:

* Toggle NORMAL / REVERSE
* Send corresponding LCC event
* Await state confirmation
* Update UI accordingly

UI must not assume command success until confirmed via state response.

### Visual indicators

Each turnout tile/widget shows:

* Name or ID
* Current state (color coded)
* Stale indicator
* Activity indicator when command pending

Suggested colors:

* NORMAL: green
* REVERSE: red
* UNKNOWN: gray
* STALE: amber
* MOVING/PENDING: animated indicator

---

## 2.5 Graphical switchboard builder

User must be able to build layout graphically on device.

### Features

* Drag and place turnout widgets
* Rename turnout
* Assign to pages or panels
* Delete turnout from panel (not from network)
* Reorder

Configuration stored locally (SPIFFS or SD).

Discovery does NOT automatically place items on layout.
Discovered items appear in “available turnouts” list.

---

## 2.6 Manual refresh and diagnostics

### Refresh all states

User can press:
“Refresh turnout states”

Panel sends global identify/state queries for all known turnout events.

### Discovery mode

User can enable:
“Listen for new turnouts”

Panel captures new event pairs and offers to add them.

### Diagnostics screen

Show:

* LCC connection status
* Last bus activity timestamp
* Number of discovered turnouts
* Event traffic counters
* Error conditions

---

# 3. Persistence Requirements

Persist locally:

* Discovered turnout definitions
* User naming
* UI layout positions
* Page assignments
* Last known state (optional cache)

On reboot:

* Load configuration
* Begin state refresh
* Update UI when confirmations arrive

---

# 4. Performance Requirements

Must handle:

* 20–80 turnouts typical
* Up to 150 turnouts max

Constraints:

* No UI blocking from CAN/LCC tasks
* Event handling must be asynchronous
* UI updates must be queued safely to LVGL thread

State refresh pacing:

* Default 50–150 ms between turnout queries
* Configurable

---

# 5. Error Handling

### Missing turnout responses

If turnout does not respond to state query:

* Mark UNKNOWN
* Retry later
* Do not block system

### Bus disconnect

If LCC connection lost:

* Show global warning
* Freeze state updates
* Allow reconnect recovery

### Conflicting events

If multiple state responses conflict:

* Use latest timestamp
* Mark as uncertain if flapping

---

# 6. Future Expansion (not required for MVP)

Potential later additions:

* Route control (multiple turnout sets)
* Signal integration
* Sensor occupancy display
* Multi-panel synchronization
* Web configuration interface
* Firmware update via network

---

# 7. MVP Definition

Minimum viable product must:

* Discover turnouts from LCC traffic
* Request and receive initial state snapshot
* Display turnout list
* Allow control of turnouts
* Persist layout and naming
* Show correct real-time state

---

# End of Specification
