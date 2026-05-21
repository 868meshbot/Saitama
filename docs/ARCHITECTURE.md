# Architecture — Saitama

## System Overview

Saitama is a single-binary ESP32-S3 firmware. No RTOS tasks beyond what
LVGL and the Arduino framework provide. One `loop()`, one thread of execution.

```
┌──────────────────────────────────────────────────────────────┐
│                        main.cpp                               │
│  setup() → Board::init() → MeshService::init() → ui::init() │
│  loop()  → Board::tick() → MeshService::tick() → ui::tick()  │
└──────────────────────────────────────────────────────────────┘
         │                │                    │
         ▼                ▼                    ▼
┌─────────────┐  ┌────────────────┐  ┌──────────────────┐
│   Board     │  │  MeshService   │  │    UI (LVGL 9)    │
│             │  │                │  │                    │
│ Display     │  │ MeshCore lib   │  │ ScreenHome        │
│ Keyboard    │  │ (submodule)    │  │ ScreenMap         │
│ Trackball   │  │                │  │ ScreenSettings     │
│ GPS         │  │ Radio config   │  │ ScreenTerminal     │
│ LoRa radio  │  │ Identity/keys   │  │ ScreenLock        │
│ Battery ADC │  │ Message queue   │  │                    │
│ SD card     │  │ Routing table   │  │ Theme (colours)    │
└─────────────┘  └────────────────┘  └──────────────────┘
         │                │                    │
         ▼                ▼                    ▼
┌──────────────────────────────────────────────────────────────┐
│                      ESP32-S3 Hardware                        │
│  240MHz dual-core │ 16MB flash │ 8MB PSRAM │ SX1262 LoRa    │
│  ST7789 320x240 │ BBQ10KB keyboard │ GT911 touch │ GPS     │
└──────────────────────────────────────────────────────────────┘
```

## Memory Layout

```
16MB Flash:
┌───────────┬─────────────┐
│ 0x0000    │ Bootloader  │  ~20KB
├───────────┼─────────────┤
│ 0x8000    │ Partitions  │  ~4KB
├───────────┼─────────────┤
│ 0x10000   │ OTA slot 0  │  ~6.4MB (active firmware)
├───────────┼─────────────┤
│ 0x650000  │ OTA slot 1  │  ~6.4MB (update staging)
├───────────┼─────────────┤
│ 0xC90000  │ SD card FS  │  ~3.6MB (unused, tiles on real SD)
└───────────┴─────────────┘

8MB PSRAM:
┌──────────┬──────────────┐
│ 0        │ LVGL buffers │  ~600KB (2x 320*40 lines)
├──────────┼──────────────┤
│ ~600KB   │ Map tile cache│  ~8 tiles × 16KB = 128KB
├──────────┼──────────────┤
│ ~728KB+  │ Message buffer│  ~100KB
├──────────┼──────────────┤
│ ~828KB+  │ Free heap     │  ~7.2MB
└──────────┴──────────────┘
```

We have generous PSRAM. The constraint is flash write cycles (SPIFFS) and
SD card read speed for map tiles.

## Data Flow

### Incoming Message

```
SX1262 radio
    │
    ▼
MeshCore ISR
    │
    ▼
MeshService::tick()
    │
    ├── Decrypt (MeshCore handles)
    ├── Determine type (channel msg / DM / advert)
    ├── Store in message buffer (PSRAM ring)
    └── Signal UI → lv_obj_add_event (new message)
         │
         ▼
    ScreenHome adds bubble to message list
```

### Outgoing Message

```
User types in textarea
    │
    ▼
Send button callback
    │
    ▼
MeshService::sendChannel() or sendDirect()
    │
    ├── MeshCore encrypts
    ├── SX1262 transmits
    └── UI shows "sent" indicator
```

### GPS → Map

```
Board::tick() reads GPS serial
    │
    ├── TinyGPSPlus parses NMEA
    └── Updates Board::gpsLat/gpsLng
         │
         ▼
ScreenMap::tick()
    │
    ├── Reads Board::instance().gpsLat/gpsLng
    ├── MapEngine::setCenter(lat, lng)
    ├── MapEngine::renderFrame()
    │     ├── Calculates visible tile grid
    │     ├── Loads PNG from SD card if not cached
    │     ├── Draws tiles to LVGL canvas
    │     └── Overlays node positions
    └── LVGL flushes to display
```

## Threading Model

**Single-threaded.** Everything runs in `loop()`:

```
loop() {
    Board::tick()        // ~1ms: poll trackball, read GPS
    MeshService::tick()  // ~5ms: process packets, radio housekeeping
    ui::tick()           // ~10ms: LVGL task handler, input dispatch
    yield()              // feed watchdog
}
```

LVGL timer handles screen transitions and animations. No FreeRTOS tasks needed
because the radio ISR is handled by MeshCore internally.

If GPS parsing or SD card reads cause latency spikes, we can offload to a
single background task later. Not needed for v0.1.

## Storage

| What | Where | Format | Size |
|------|-------|--------|------|
| Config | SPIFFS `/ops.cfg` | JSON | ~1KB |
| Identity keys | SPIFFS `/ops.id` or `/identity.id` | Binary (MeshCore format) | ~96B |
| Region map | SPIFFS `/regions.bin` | Binary (MeshCore format) | Varies |
| Message log | SPIFFS `/msgs/` | JSON lines | ~100KB max |
| Map tiles | SD card `/map/z/x/y.png` | PNG 256x256 | Varies |
| Config export | SD card `/ops/config.json` | JSON | ~1KB |
| Identity export | SD card `/ops/identity.id` | Binary (MeshCore format) | ~96B |
| Region export | SD card `/ops/regions.bin` | Binary (MeshCore format) | Varies |
| Contact exports | SD card `/ops/contacts/*.id` | Binary (MeshCore advert packets) | ~100B each |
| Firmware OTA | Flash OTA slot 1 | Binary | ~6.4MB max |

SPIFFS is wear-levelled by ESP-IDF but we minimise writes. Messages are
append-only logs, rotated when they exceed quota.

Config export/import (`ConfigExport.h/cpp`) copies all settings between
SPIFFS and SD card using MeshCore-compatible binary formats. This lets users
back up their config, share identities between devices, and restore after
a reflash. See `docs/VERSIONING.md` for flash/recovery procedures.

## Build Flags

| Flag | Purpose |
|------|---------|
| `OPS_PLATFORM_TDECK` | Target T-Deck hardware |
| `OPS_SCREEN_W/H` | Display dimensions |
| `OPS_HAS_BUILTIN_GPS` | T-Deck Plus has GPS on-board |
| `MESH_DEBUG` | Enable MeshCore debug logging |
| `OPS_VERSION` | Version string |

All board-specific pin definitions live in `Board.cpp`, not in build flags.
Adding a new board means writing a new `Board.cpp` and a new PlatformIO
environment. The rest of the codebase stays the same.