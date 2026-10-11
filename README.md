# Saitama

[![License: GPL v3+](https://img.shields.io/badge/License-GPL%20v3%2B-blue.svg)](LICENSE)
[![Latest Release](https://img.shields.io/github/v/release/868meshbot/Saitama)](https://github.com/868meshbot/Saitama/releases/latest)
[![Release Date](https://img.shields.io/github/release-date/868meshbot/Saitama)](https://github.com/868meshbot/Saitama/releases)
[![Build](https://img.shields.io/github/actions/workflow/status/868meshbot/Saitama/platformio.yaml?branch=main)](https://github.com/868meshbot/Saitama/actions)
[![Platform](https://img.shields.io/badge/platform-T--Deck%20Plus-orange)](https://www.lilygo.cc/products/t-deck)
[![MeshCore](https://img.shields.io/badge/mesh-MeshCore-green)](https://github.com/meshcore-dev/MeshCore)

Your T-Deck, fully off-grid. Saitama is a standalone [MeshCore](https://github.com/meshcore-dev/MeshCore) messenger for the LilyGo T-Deck and T-Deck Plus: encrypted channels and DMs, a live view of every station around you, offline maps, and repeater and region tools, all on the device, with **full touchscreen support** alongside the keyboard and trackball. No phone, no internet, no subscription. Free and open source.

**A phone in your pocket, without the phone, and without the paywall.**

---

| Home | Apps | Chat | Channels |
|:----:|:----:|:----:|:--------:|
| ![Home screen — callsign and channel waterfall](screenshots/home.png) | ![2nd screen app launcher](screenshots/home1.png) |![Chat — public channel messages](screenshots/chat.png) | ![Channels — channel and DM list](screenshots/channels.png) | 

| Contacts | Map | MP3 Player |
|:---:|:----:|:----------:|
| ![Contacts — contact list](screenshots/contacts.png)| ![Map — offline tile map with mesh nodes](screenshots/maps.png) | ![MP3 Player — audio playback from SD](screenshots/mp3player.png) |

---

## What Is This?

Saitama is a free, open-source firmware that turns affordable LoRa devices into powerful standalone mesh communicators. It provides smartphone-grade messaging, GPS maps, encrypted comms, and more, all running directly on the device with no phone, no internet, and no license fees required.

Saitama is a community project. Development tooling includes AI coding assistants — contributions are reviewed and tested by humans on real hardware.

## Features

- **Full touchscreen support**: every screen works by touch as well as by keyboard and trackball. Tap tiles, buttons and list rows, swipe between launcher pages, scroll lists and chats, and use dropdowns and sliders by finger. Popups use large two-column buttons sized for fingers. GT911 capacitive touch, with the controller address detected automatically at boot and a calibration routine (`touch-cali` in the Terminal).
- **Home screen**: the stations heard this session fall like rain behind Chat / Contacts / Map / Settings — coloured by signal strength, channels in blue, with lines linking each new message to its channel, reply target or DM recipient. Swipe (or trackball past the edge) for the full app grids.
- **Chat**: Public plus up to 9 private channels, and direct messages, in a speech-bubble UI; unread counts and last-message previews; replies (`@[Name]`); DM auto-retries with delivery ticks; history saved to SD.
- **Region scopes**: a default flood scope plus up to 9 saved regions (Settings > Region Scope), a per-channel scope, and a **Regions** app that asks nearby repeaters which regions they serve — for meshes whose repeaters only forward scoped traffic.
- **Contacts, Repeaters & Heard**: contact and repeater directories with saved routes, repeater admin login (optional remembered password), path set/reset, and a live list of every station heard.
- **Network tools**: Finder (zero-hop neighbour scan), hop-by-hop Trace, Signal (RSSI/SNR/noise, airtime), channel scan, spectrum, PCAP capture, signal generator, BT foxhunt.
- **GPS Map**: offline tile map from the SD card with node positions.
- **Media & extras**: MP3 player, picture viewer, file manager, terminal, 2048.
- **Power**: CPU governor, screensaver and screen-off with radio light sleep, LoRa duty cycle or FHSS, RX boost, power estimates.
- **BLE Companion**: works with the MeshCore mobile apps over Bluetooth, paired with a per-device PIN.
- **Backup**: settings, contacts, repeaters and the node identity are mirrored to `/ops/` on the SD card and restored after a reflash.

## Security

- Mesh traffic is end-to-end encrypted by MeshCore.
- Secrets written to the SD card — channel keys, remembered repeater passwords and the node identity backup — are sealed with AES-256-GCM using a **storage password** (Settings > Storage Password). It starts as `changeme1`; **change it**, or a lifted SD card is as readable as that published default. See [docs/STORED_SECRETS.md](docs/STORED_SECRETS.md).
- The Bluetooth pairing PIN is random per device (Settings > Bluetooth PIN). Changing it unpairs all phones.
- Report vulnerabilities privately — see [docs/SECURITY.md](docs/SECURITY.md).

## Supported Hardware

| Device | Status | Notes |
|--------|--------|-------|
| LilyGo T-Deck (ESP32-S3) | Untested | 320x240 IPS, keyboard, trackball, SX1262 LoRa |
| LilyGo T-Deck Plus | **Working** | Primary target — compiled and hardware-tested |
| Other ESP32-S3 devices | Future | PlatformIO abstraction allows porting |

## Branches

| Branch | Purpose |
|--------|----------|
| `main` | Stable releases. Tagged with versions. Don't push directly. |
| `dev` | Active development. PRs go here. CI must pass before merge. |
| `lvgl9` | Experimental — LVGL 9.5.0 port. Not for production use. |

Workflow: contribute to `dev` via PR. When stable, merge to `main` and tag a release.

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/) installed (CLI or VS Code extension)
- USB-C cable
- A LilyGo T-Deck or T-Deck Plus

### Build

```bash
git clone --recurse-submodules https://github.com/868meshbot/Saitama.git
cd Saitama
pio run -e t-deck
```

### Flash

Hold the trackball center button, press the reset button on the side, then release both. The T-Deck is now in DFU mode.

**First time / recovery (merged binary):**
```bash
pio run -e t-deck -t upload
# Or manually with the merged binary:
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x0 saitama-merged.bin
```

See [docs/VERSIONING.md](docs/VERSIONING.md) for firmware variant details.

### Map Tiles

Map tiles go on a FAT32-formatted SD card, one folder per style:

```
/maps/osm/day/{z}/{x}/{y}.png      # standard OpenStreetMap
/maps/osm/night/{z}/{x}/{y}.png    # optional dark style (Thunderforest Transport Dark)
```

Tiles use the standard "slippy map" layout: zoom, then x, then y. Example (London, zoom 10): `/maps/osm/day/10/511/340.png`. Cards with the older flat layout (`/maps/osm/{z}/{x}/{y}.png`) still work and are used as day tiles.

**Settings > Map Style** chooses Day, Night or **Auto** (default): Auto shows night tiles while Night Mode is active. With no night tiles, the day tiles are shown colour-inverted at night, so a dark map needs no extra download.

**Option 1 — included CLI script:**

```bash
python3 scripts/download_tiles.py \
    --output /Volumes/SD/maps/osm \
    --lat 51.5 --lng -0.1 --radius 20 --zoom 10-14
```

`--output` is the `maps/osm` folder; the script adds `day/` (or `night/` with `--style night`). See `scripts/download_tiles.py --help` for full options. Respect the [OSM tile usage policy](https://operations.osmfoundation.org/policies/tiles/) — rate-limited to 2 req/s.

Night tiles (`--style night`) come from Thunderforest and need an API key (`--apikey` or `THUNDERFOREST_API_KEY`). Thunderforest only permits bulk/offline downloading on its [Small Business or higher plans](https://www.thunderforest.com/pricing/), so the script also requires `--i-have-a-bulk-download-plan`. Without such a plan, rely on the inverted day tiles.

**Option 2 — GUI tool ([map-tiles-downloader](https://github.com/tekk/map-tiles-downloader)):**

A graphical downloader with a map preview. Draw a bounding box, pick zoom levels, and export standard OSM tiles; make sure the result is laid out as `{z}/{x}/{y}.png` under `maps/osm/day`.

```
Output path: /Volumes/SD/maps/osm/day
```

## Project Structure

```
Saitama/
├── src/
│   ├── main.cpp                  # Entry point: setup() + loop()
│   ├── version.h                 # Release version
│   ├── hardware/
│   │   └── Board.h/cpp           # T-Deck hardware: power, display, keyboard, trackball, touch, GPS, battery
│   ├── mesh/
│   │   ├── MeshService.h/cpp     # MeshCore bridge: messages, contacts, routes, region scopes, BLE companion
│   │   └── Fhss.h/cpp            # Frequency hopping
│   ├── bt/
│   │   └── BTCompanionService.h/cpp  # BLE companion transport (MeshCore apps)
│   ├── map/
│   │   └── MapEngine.h/cpp       # Offline tile renderer
│   ├── games/                    # Game logic: 2048, tic-tac-toe
│   ├── ui/
│   │   ├── UIScreen.h/cpp        # LVGL init, input drivers, main UI tick, screensaver, notifications
│   │   ├── ScreenBoot            # Splash screen
│   │   ├── ScreenLauncher        # Home page (heard-station rain) + two app grids
│   │   ├── ScreenHome            # Chat: channel list, conversations, DMs
│   │   ├── ScreenContacts        # Contacts: filter, signal bars, actions, Details, Block
│   │   ├── ScreenRepeaters       # Repeaters: filter, signal bars, actions, admin login/panel
│   │   ├── ScreenHeard           # Stations heard this session
│   │   ├── ScreenRegions         # Region discovery + saved regions
│   │   ├── ScreenFinder          # Zero-hop neighbour scan
│   │   ├── ScreenTrace           # Hop-by-hop route trace
│   │   ├── ScreenMap             # Offline map with node positions
│   │   ├── ScreenSettings        # Settings
│   │   ├── ScreenTerminal        # Terminal + USB serial console
│   │   ├── ScreenSignal          # Radio stats: RSSI/SNR/noise, packets, airtime
│   │   ├── ScreenPower           # Battery history and power estimates
│   │   ├── ScreenSpectrum, ScreenChanScan, ScreenSigGen, ScreenPcap, ScreenFoxhunt  # Radio tools
│   │   ├── ScreenMP3Player, ScreenPicViewer, ScreenFileManager                    # Media and files
│   │   ├── Screen2048, ScreenZeroXZero                                           # Games
│   │   ├── ScreenPlaceholder     # GPS skyplot
│   │   ├── ScopePicker           # Shared region-scope dropdown
│   │   ├── ListFilter            # Type-to-filter box for lists
│   │   ├── QRPopup               # QR code sharing
│   │   ├── Emoji.h/cpp, emoji/   # Emoji image font (172 emoji)
│   │   ├── font_montserrat_*_ext.c  # Extended-Latin fonts
│   │   └── Theme.h/cpp           # Colours, fonts, text cleanup, signal bars
│   └── utils/
│       ├── Config.h/cpp          # Settings (NVS + /ops/settings.json)
│       ├── ConfigExport.h/cpp    # MeshCore-format import/export
│       ├── Contacts.h/cpp        # Saved contacts (/ops/contacts.json)
│       ├── Repeaters.h/cpp       # Saved repeaters (/ops/repeaters.json)
│       ├── Regions.h/cpp         # Saved region scopes
│       ├── Crypto.h/cpp          # AES-256-GCM sealing for secrets on SD
│       ├── IdentityBackup.h/cpp  # Encrypted node identity backup
│       ├── BtPin.h/cpp           # Bluetooth pairing PIN
│       ├── SDCard.h/cpp          # SD mount, /ops/ files, message logs
│       ├── Sound.h/cpp, MP3Player.h/cpp  # Speaker: notification sounds, MP3 playback
│       ├── GpsMgr.h/cpp          # GPS power modes
│       ├── Keymap.h/cpp, Lang.h/cpp  # Keyboard layouts, UI languages
│       ├── LoopStats.h/cpp       # Main-loop timing
│       ├── PsramJson.h           # PSRAM allocator for large JSON files
│       └── Log.h                 # Serial logger
├── lib/
│   ├── MeshCore/                 # Git submodule (mesh protocol) — not modified
│   └── lv_conf.h                 # LVGL configuration (the one the build uses)
├── docs/                         # ARCHITECTURE, HARDWARE, STORED_SECRETS, SECURITY, FHSS, MAP_SYSTEM, VERSIONING, …
├── scripts/                      # download_tiles.py (map tiles), gen_emoji.py (emoji assets)
├── platformio.ini
├── partitions.csv
└── LICENSE                       # GPL-3.0
```

## Architecture

Saitama is layered:

```
┌─────────────────────────────────┐
│          UI (LVGL 8.3)          │
│ Home │ Chat │ Map │ Settings │ …│
├─────────────────────────────────┤
│            App Logic            │
│   Messages │ Contacts │ Config  │
├─────────────────────────────────┤
│       Hardware Abstraction      │
│  Board │ Keyboard │ GPS │ LoRa  │
├─────────────────────────────────┤
│      MeshCore (C++ library)     │
│   Routing │ Encryption │ Radio  │
├─────────────────────────────────┤
│        ESP32-S3 Hardware        │
└─────────────────────────────────┘
```

- **MeshCore** handles all mesh networking: routing, encryption, packet handling
- **Board** abstracts T-Deck peripherals: display, keyboard, trackball, GPS, LoRa radio
- **App Logic** manages messages, contacts, settings, map state
- **UI** renders everything through LVGL with a consistent dark theme

No dynamic memory allocation after setup. No heap fragmentation. This is embedded software.

## Versioning

Saitama follows [Semantic Versioning](https://semver.org/) with pre-release tags:

- **`-rc.N`** — release candidate. Final testing.
- **(none)** — stable release.

Current version: **1.4.4** (compiled and tested on LilyGo T-Deck Plus)

Each release includes two firmware binaries:
1. **App-only** (`saitama-X.Y.Z.bin`) — for OTA updates, flash at `0x10000`
2. **Merged** (`saitama-X.Y.Z-merged.bin`) — bootloader + partitions + app, flash at `0x0`

See [docs/VERSIONING.md](docs/VERSIONING.md) for full details.

## Status

Compiled and hardware-tested on a LilyGo T-Deck Plus. Core features (chat, mesh, region scopes, GPS, repeater management, BLE companion) are functional. Edge cases and untested hardware variants are expected — contributions welcome.

## License

This project is licensed under the GNU General Public License v3.0 or later. See [LICENSE](LICENSE) for full text. Dependency licenses: MeshCore (MIT), LVGL (MIT), TFT_eSPI (MIT), ArduinoJson (MIT).

## Links

- [MeshCore](https://github.com/meshcore-dev/MeshCore) — the mesh networking library we depend on
- [LilyGo T-Deck](https://github.com/Xinyuan-LilyGO/T-Deck) — hardware reference
- [LVGL](https://lvgl.io/) — UI framework
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
