# Agent Operating Rules

## Implementation Status

| Component | Status | Notes |
|-----------|--------|-------|
| Board Drivers | ✓ Complete | CH422G, LCD, Touch, SD all working |
| LCC/OpenMRN | ✓ Complete | Node init, TWAI, event production/consumption, CDI/ACDI |
| LVGL UI | ✓ Complete | Panel screen (default), Settings screen (3-tab) |
| Turnout Storage | ✓ Complete | JSON parse/save on SD card + JMRI roster.xml import |
| Turnout Manager | ✓ Complete | Thread-safe state management with FreeRTOS mutex, swap/reorder/flip polarity |
| Panel Layout | ✓ Complete | Pure data model singleton with query/mutation operations |
| Panel Screen | ✓ Complete | Live control panel with auto-fit scaling, Y-shape turnouts, tracks, tap-to-toggle |
| Panel Builder | ✓ Complete | WYSIWYG editor: place/drag/rotate/delete turnouts, endpoints, tracks |
| Panel Storage | ✓ Complete | JSON persistence of panel layout to SD card (with write retry) |
| Panel Geometry | ✓ Complete | Y-shape connection point calculations with rotation/mirror |
| SD Error Screen | ✓ Complete | Displays when SD card missing |
| Splash Screen | ✓ Complete | JPEG decode splash + SD card error screen (ui_splash.c) |
| Screen Timeout | ✓ Complete | Backlight power saving with touch-to-wake |
| Stale Detection | ✓ Complete | Marks turnouts stale after configurable timeout |
| Firmware Update | ✓ Complete | OTA via LCC Memory Config Protocol, JMRI compatible |
| Inline Edit/Delete | ✓ Complete | Edit (rename + flip polarity) and delete turnouts from tile icons |

### Recent Changes (Session 2026-02-22c)
- Added polarity flip feature: `turnout_manager_flip_polarity()` swaps Normal/Reverse
  event IDs. Edit Turnout dialog (renamed from "Rename Turnout") now includes a
  "Flip Polarity" button that swaps events, updates the panel layout key if placed,
  re-registers LCC events, and saves both turnouts.json and panel.json
- Added swap button (amber ↔) between Normal/Reverse event ID fields on the Add
  Turnout form to swap field contents before adding
- Fixed turnout Y-shape geometry: reverse leg changed from (42, −30) to (40, −40)
  so the diverging branch spans exactly 2 grid spacings (40px) vertically, ensuring
  connecting tracks align with the snap grid
- Updated all documentation

### Recent Changes (Session 2026-02-22b)
- Refactored `main.c` (704→365 lines): extracted splash screen and SD card error
  screen into `ui/ui_splash.c`, extracted bootloader check into helper function,
  cleaned up verbose logging, removed dead code
- Added `ui_splash.c` with `ui_splash_show_image()` (JPEG decode + framebuffer blit)
  and `ui_splash_show_sd_error()` (LVGL error screen)
- Fixed turnout deletion bug: deleting a turnout now removes its panel layout item
  (cascade-deleting connected tracks) and saves panel.json
- Added SD card write retry (3 attempts, 100ms delay) to `panel_storage_save()` and
  `turnout_storage_save()` to handle SPI timeout after idle periods
- Added auto-fit scaling to control panel screen: layout is uniformly scaled and
  centered to fill the 800×440 canvas with 20px margins; line widths and hitbox
  sizes scale proportionally (minimums: 2px lines, 40×30 hitboxes)
- Updated panel.json format documentation to reflect event_normal-based keys
- Updated all documentation

### Recent Changes (Session 2026-02-23)
- Extracted `panel_layout` data model from `ui_common.h` into standalone `app/panel_layout.h/.c`
- Added `panel_storage` for JSON persistence of panel layout to `/sdcard/panel.json`
- Added `panel_geometry` for Y-shape turnout connection point calculations
- Added `ui_panel.c` — control panel screen as default boot screen
- Added `ui_panel_builder.c` — WYSIWYG panel layout editor (place, drag, rotate, mirror, delete)
- Restructured navigation: Panel Screen (default) ↔ Settings Screen (3-tab with back button)
- Fixed reboot-on-click crash: added `ui_turnouts_invalidate()` and `ui_panel_invalidate()` to
  NULL out dangling LVGL pointers during screen transitions (prevented use-after-free from async callbacks)
- Moved panel builder nav buttons (pan/home) to bottom of sidebar with flex-grow spacer
- Reduced `PANEL_MAX_ITEMS`/`ENDPOINTS`/`TRACKS` from 150/100/300 to 50/20/100 to prevent
  BSS overflow starving RGB LCD DMA bounce buffer allocation (fixed black screen crash)
- Updated all documentation

### Recent Changes (Session 2026-02-22)
- Added inline edit (rename) and delete for turnout tiles with full-screen modals
- Restructured tile layout: 150×110px, 3 rows (Name / State / Edit+Delete buttons)
- Changed state display text from "NORMAL"/"REVERSE" to "CLOSED"/"THROWN"
- Custom delete confirmation dialog with Material Red warning styling
- Added `turnout_manager_swap()` for future reorder support
- Fixed LCC event loopback: removed source filtering on EventReport (loopback is harmless)
- Fixed startup state polling: only process ProducerIdentified with EventState::VALID
- Added JMRI roster.xml import (parses OlcbTurnoutManager section, deduplicates, respects inverted)
- Updated all documentation

### Recent Changes (Session 2026-02-21)
- Converted from lighting scene controller to turnout control panel
- Replaced scene_storage with turnout_storage (JSON persistence)
- Replaced fade_controller with turnout_manager (thread-safe state)
- Rewrote lcc_node.cpp with TurnoutEventHandler for event consumption
- Created turnout switchboard UI (color-coded tiles, tap-to-toggle)
- Created Add Turnout tab (manual entry + discovery mode)
- Updated CDI config: PanelConfig with screen_timeout, stale_timeout, query_pace
- Removed: fade_controller, scene_storage, ui_manual, ui_scenes
- Updated all documentation for turnout panel

### Previous Changes (Session 2026-01-19)
- Added LCC firmware update support (FR-060 to FR-064)
- Created `bootloader_hal.cpp` for ESP32 OTA integration
- Created `bootloader_display.c` for LCD status during firmware updates
- Updated partition table for dual OTA partitions (ota_0, ota_1)

---

## Component Ownership

| Component | Files | Scope |
|-----------|-------|-------|
| Board Drivers | `components/board_drivers/*` | CH422G, LCD, Touch, SD |
| LCC/OpenMRN | `main/app/lcc_node.*`, `main/app/lcc_config.hxx` | Node init, event prod/consume, TWAI, CDI |
| Bootloader HAL | `main/app/bootloader_hal.*` | OTA firmware updates via LCC |
| Bootloader Display | `main/app/bootloader_display.*` | LCD status during OTA updates |
| Panel Layout | `main/app/panel_layout.*` | Panel data model singleton (items, endpoints, tracks) |
| Panel Storage | `main/app/panel_storage.*` | JSON persistence of panel layout to SD card |
| Turnout Storage | `main/app/turnout_storage.*` | JSON parse/save on SD card, JMRI XML import |
| Turnout Manager | `main/app/turnout_manager.*` | Thread-safe turnout state, stale detection, swap/reorder/flip polarity |
| Panel Screen | `main/ui/ui_panel.c` | Live control panel rendering (auto-fit) and interaction |
| Panel Builder | `main/ui/ui_panel_builder.c` | WYSIWYG panel layout editor |
| Panel Geometry | `main/ui/panel_geometry.*` | Y-shape turnout geometry calculations |
| Splash Screen | `main/ui/ui_splash.c` | Boot splash JPEG display + SD card error screen |
| LVGL UI (core) | `main/ui/ui_common.*`, `main/ui/ui_main.c` | Init, mutex, settings screen |
| Turnout UI | `main/ui/ui_turnouts.c`, `main/ui/ui_add_turnout.c` | Switchboard grid, add turnout form |

---

## Definition of Done

- Builds under ESP-IDF v5.1.6 with no errors (warnings acceptable in OpenMRN)
- Requirement IDs (FR-xxx) referenced in code comments
- No blocking I/O in UI task
- LVGL calls protected by mutex when called from non-UI tasks
- LVGL callbacks use `lv_async_call()` for cross-task UI updates
- Memory allocations use appropriate heap (PSRAM for large buffers)
- Turnout state changes propagated via callback (not polling)

---

## Dependencies

### External Components (idf_component.yml)
- `lvgl/lvgl: "^8"` — UI framework
- `espressif/esp_lcd_touch: "*"` — Touch abstraction
- `espressif/esp_lcd_touch_gt911: "*"` — GT911 driver
- `espressif/esp_jpeg: "*"` — JPEG decoding for splash

### Git Submodules
- `components/OpenMRN` — LCC/OpenLCB stack

---

## Change Control

| Change Type | Required Updates |
|-------------|------------------|
| Event handling | SPEC.md §3, INTERFACES.md §7 |
| Task model | ARCHITECTURE.md §2 |
| GPIO assignments | INTERFACES.md, sdkconfig.defaults |
| Config file format | SPEC.md §4-5, turnout_storage / panel_storage |
| New component | AGENTS.md, CMakeLists.txt |
| Turnout UI changes | SPEC.md §6 (FR-020+), README.md UI section |
| Panel layout changes | SPEC.md §6 (FR-040+), ARCHITECTURE.md §9-10 |
| JMRI import | SPEC.md FR-026, ARCHITECTURE.md §5 |

---

## Build & CI

### Local Build
```bash
# Ensure ESP-IDF 5.1.6 is installed and exported
. $HOME/esp/v5.1.6/export.sh   # Linux/macOS
# or: C:\Users\<user>\esp\v5.1.6\export.ps1  # Windows PowerShell

idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

### Windows-Specific Setup

OpenMRN's `include/esp-idf/` directory contains Unix symlinks that appear as 
plain text files on Windows. Before building, verify these files contain 
`#include` directives rather than bare paths. If not, convert them:

```
// Bad (broken symlink):
../openmrn_features.h

// Good (proper wrapper):
#include "../openmrn_features.h"
```

Files requiring this fix:
- `openmrn_features.h`, `freertos_includes.h`, `can_frame.h`, `can_ioctl.h`
- `ifaddrs.h`, `i2c.h`, `i2c-dev.h`, `nmranet_config.h`, `stropts.h`
- `CDIXMLGenerator.hxx`, `sys/tree.hxx`

### GitHub Actions
- Workflow: `.github/workflows/build.yml`
- Container: `espressif/idf:v5.1.6`
- Artifacts: `build/*.bin`
- Submodules: Recursive checkout required
