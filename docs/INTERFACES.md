# Interfaces

## 1. I2C Bus

### Configuration
- SCL: GPIO9
- SDA: GPIO8
- Frequency: 400 kHz
- Port: I2C_NUM_0

### Devices
| Address | Device | Purpose |
|---------|--------|---------|
| 0x24 | CH422G | Mode register |
| 0x38 | CH422G | Output register |
| 0x5D | GT911 | Touch controller |

---

## 2. CH422G I/O Expander

The CH422G controls SD card CS, LCD backlight, and touch reset.

### Mode Configuration
Write `0x01` to address `0x24` to set output mode.

### Output Register (0x38) Bit Mapping
| Bit | Function |
|-----|----------|
| 0 | SD Card CS (active low) |
| 1 | Touch Reset |
| 2 | LCD Backlight |
| 3-7 | Reserved |

### Common Commands
| Action | Value | Notes |
|--------|-------|-------|
| Backlight ON | 0x1E | CS high, touch normal, BL on |
| Backlight OFF | 0x1A | CS high, touch normal, BL off |
| SD CS Low | 0x0A | Enable SD card |
| Touch Reset Start | 0x2C | Assert touch reset |
| Touch Reset End | 0x2E | Release touch reset |

---

## 3. SPI / SD Card

### SPI Pins
- MOSI: GPIO11
- MISO: GPIO13
- CLK: GPIO12
- CS: Via CH422G (not direct GPIO)

### Required SD Enable Sequence
1. Initialize I2C bus
2. Write `0x01` to CH422G address `0x24` (output mode)
3. Write `0x0A` to CH422G address `0x38` (SD CS low)
4. Initialize SPI bus (SDSPI_HOST_DEFAULT)
5. Mount FATFS at `/sdcard`

### SD Card Files
| File | Purpose |
|------|---------|
| `/sdcard/nodeid.txt` | LCC Node ID (plain text, dotted hex) |
| `/sdcard/turnouts.json` | Turnout definitions (name + event ID pairs) |
| `/sdcard/splash.jpg` | Boot splash image |
| `/sdcard/openmrn_config` | OpenMRN persistent config (auto-created) |

---

## 4. RGB LCD

### Timing Configuration (800x480 @ 16MHz)
- Pixel Clock: 16 MHz
- HSYNC Pulse: 4, Back Porch: 8, Front Porch: 8
- VSYNC Pulse: 4, Back Porch: 8, Front Porch: 8

### GPIO Mapping
| Signal | GPIO |
|--------|------|
| VSYNC | 3 |
| HSYNC | 46 |
| DE | 5 |
| PCLK | 7 |
| DATA0-4 | 14, 38, 18, 17, 10 |
| DATA5-7 | 39, 0, 45 |
| DATA8-11 | 48, 47, 21, 1 |
| DATA12-15 | 2, 42, 41, 40 |

### Frame Buffer
- Location: PSRAM (framebuffer), Internal RAM (bounce buffer)
- Format: RGB565 (16-bit)
- Size: 800 × 480 × 2 bytes × 2 buffers = 1.5MB
- Mode: Double buffering with DMA bounce buffer

### Bounce Buffer Configuration
The RGB LCD uses a bounce buffer in internal DMA-capable RAM to transfer
framebuffer data from PSRAM to the display controller:

| Setting | Value | Notes |
|---------|-------|-------|
| `CONFIG_LCD_RGB_BOUNCE_BUFFER_HEIGHT` | 60 lines | 96KB per buffer, fits in internal RAM |
| Framebuffer divisor (N) | 8 | 480 ÷ 60 = 8 (must be even integer) |

**Memory Constraints:**
- Bounce buffer must be in internal DMA-capable RAM (not PSRAM)
- Available internal RAM: ~250KB
- 60 lines × 800 pixels × 2 bytes = 96KB per buffer (×2 = 192KB total)
- 80+ line buffers cause out-of-memory crashes at boot

---

## 5. Touch Controller (GT911)

### Configuration
- I2C Address: 0x5D (after reset sequence)
- Interrupt: Not used (polling)
- Resolution: Matches LCD (800x480)

### Reset Sequence
1. Write `0x01` to CH422G `0x24`
2. Write `0x2C` to CH422G `0x38`
3. Delay 100ms
4. Set GPIO4 LOW
5. Delay 100ms
6. Write `0x2E` to CH422G `0x38`
7. Delay 200ms

---

## 6. CAN / TWAI (LCC Bus)

### GPIO Mapping
- TX: GPIO15
- RX: GPIO16

### Configuration
- Bit Rate: 125 kbps (LCC standard)
- Mode: Normal (with ACK)
- Driver: OpenMRN `Esp32HardwareTwai`
- VFS Path: `/dev/twai/twai0`

---

## 7. LCC Event Model

### Node ID
Configured in `/sdcard/nodeid.txt` (14 hex digits with dots, e.g., `05.01.01.01.9F.60.00`)

### Turnout Event Pairs

Each turnout is defined by two 64-bit LCC event IDs:
- **Normal Event**: Produced/consumed when turnout is in Normal (closed) position
- **Reverse Event**: Produced/consumed when turnout is in Reverse (thrown) position

Event IDs are user-configurable per turnout and stored in `turnouts.json` in dotted-hex format.

### Event Consumption

The panel consumes the following OpenLCB messages:
| Message | Purpose |
|---------|---------|
| EventReport | Turnout state changed (update tile immediately) |
| ProducerIdentified | Response to state query (update tile) |

### Event Production

The panel produces events when the user taps a turnout tile:
- If current state is NORMAL → sends Reverse event
- If current state is REVERSE or UNKNOWN/STALE → sends Normal event

### State Query at Startup

On boot, after LCC initialization, the panel sends `IdentifyConsumer` for each
registered turnout event to learn current positions from turnout decoders on the bus.
Queries are paced at a configurable interval (default 100ms) to avoid bus congestion.

### Discovery Mode

When enabled via the Add Turnout tab, the panel captures all EventReport messages
seen on the bus (regardless of registration) to help users identify event IDs for
turnouts they want to add.
