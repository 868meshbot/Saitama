# Roadmap — Saitama

Current firmware version: **0.1.0-alpha.1** (first compile, not flashed to hardware)

## Phase 0: Foundation Cleanup (v0.1.0 — Current)

- [x] Project structure and PlatformIO config
- [x] Hardware abstraction layer (Board.h/cpp)
- [x] Config system (SPIFFS JSON)
- [x] LVGL 9 + TFT_eSPI display driver stub
- [x] Theme system (dark mode colours)
- [x] Home screen layout (chat bubbles, tabs, input)
- [x] MapEngine coordinate math
- [x] MeshCore as git submodule
- [x] First successful compile without errors
- [x] CI pipeline (build, test, security audit, release workflow)
- [x] CodeQL security scanning (weekly, Copilot Autofix enabled)
- [x] MapEngine unit tests (34 tests passing)
- [x] Config export/import via SD card (MeshCore-compatible format)
- [x] Branching model (main/dev/alpha/beta)
- [x] Release v0.1.0-alpha.1 (draft firmware on GitHub)
- [ ] **Human review needed**: Validate pin assignments against T-Deck schematic
- [ ] **Human review needed**: Test TFT_eSPI User_Setup.h configuration
- [ ] **Human review needed**: Verify LVGL 9 integration with TFT_eSPI on ESP32-S3
- [ ] First flash to T-Deck hardware

## Phase 1: Core Chat (v0.2.0)

Goal: Send and receive messages on the mesh.

- [ ] MeshCore MainBoard implementation for T-Deck
  - Battery voltage read (ADC on GPIO)
  - MCU temperature
  - Reboot/powerOff
- [ ] MeshCore RTCClock implementation
  - Sync from GPS (T-Deck Plus)
  - Sync from BLE companion app
  - Manual set via settings
- [ ] MeshCore radio init (SX1262, region config)
- [ ] Message send: channel messages via MeshCore
- [ ] Message send: direct messages via MeshCore
- [ ] Message receive: display incoming messages in chat UI
- [ ] Message ring buffer (PSRAM, 1000 messages max)
- [ ] Keyboard input: BBQ10KB I2C driver
  - Key events → LVGL textarea
  - Special keys (Enter = send, Esc = back)
- [ ] Channel switching (trackball left/right on status bar)
- [ ] BLE companion app connectivity
- [x] Config import/export (MeshCore companion app format)
- [x] Unit tests for coordinate math (MapEngine)

## Phase 2: Map (v0.3.0)

Goal: Offline GPS map with node positions.

- [ ] GPS serial driver (T-Deck Plus built-in GPS)
- [ ] TinyGPSPlus integration
- [ ] SD card initialization and tile directory scan
- [ ] PNG tile decoder (lodepng or LVGL PNG decoder)
- [ ] Tile rendering to LVGL canvas
- [ ] Pan (trackball left/right/up/down)
- [ ] Zoom (trackball center press → zoom menu, or +/- buttons)
- [ ] Node marker overlay (self, contacts, repeaters)
- [ ] Node tap/select → info popup
- [ ] Tile caching in PSRAM (LRU, 8 tiles)
- [ ] Progressive tile loading (center first, edges after)
- [x] Tile download helper script (`scripts/download_tiles.py`)
- [x] Map screen accessible from home screen navigation

## Phase 3: Settings & Polish (v0.4.0)

Goal: Full settings, terminal, and polish.

- [ ] Settings screen implementation
  - Radio region selector (EU868, US915, AU915, etc.)
  - Channel selector
  - TX power slider
  - Callsign editor
  - Brightness slider
  - Screen timeout selector
  - Sound toggle
  - Theme toggle (dark/light)
  - BLE toggle
- [ ] Repeater scanner
  - Discover repeaters on the mesh
  - Show signal strength, distance, uptime
  - Whitelist management
- [ ] Terminal screen
  - Full MeshCore CLI
  - Command history (up/down arrows)
  - Multi-colour output (errors red, warnings orange, data green)
- [ ] Notifications
  - Sound on incoming message (buzzer)
  - Screen wake on incoming message
  - Auto-dimming after timeout
- [ ] Lock screen
  - Time, date, battery, node count
  - Press any key to unlock
- [ ] OTA firmware update (via SD card or BLE)

## Phase 4: Multi-Device (v0.5.0)

Goal: Support other ESP32-S3 LoRa devices.

- [ ] Abstract Board interface
  - `Board.h` becomes `BoardTDeck.h`
  - New `BoardGeneric.h` for Heltec V3, RAK WisBlock, etc.
- [ ] PlatformIO environments for:
  - `t-deck` (current)
  - `t-deck-plus` (GPS variant)
  - `heltec-v3`
  - `rak-wisblock`
- [ ] Display driver abstraction (different screens, different resolutions)
- [ ] Input abstraction (some devices have no keyboard, only touch)
- [ ] Build matrix in CI

## Phase 5: Hardening (v1.0.0)

Goal: Production-ready firmware.

- [ ] Power optimization (sleep modes, LoRa TX burst timing)
- [ ] Memory audit (no fragmentation after 72h runtime)
- [ ] Long-duration stress test (48h continuous operation)
- [ ] SD card corruption prevention (proper unmount, wear leveling)
- [ ] SPIFFS wear minimization (config writes only on explicit save)
- [ ] Watchdog timer (auto-reboot on hang after 30s)
- [ ] Crash logging (save stack trace to SPIFFS, show on next boot)
- [ ] Security audit (MeshCore encryption, no plaintext key storage)
- [ ] User documentation (flashing guide, settings reference, FAQ)
- [x] Release binaries on GitHub with SHA-256 checksums
- [x] Web flasher page (like MeshCore's) — instructions in release notes

## Version Numbering

- `0.x.y` — Development builds, things may break
- `1.0.0` — First production release
- Versions only go forward, never backward