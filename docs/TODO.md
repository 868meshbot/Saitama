# TODO — Saitama

## Immediate (before first flash)

- [x] Verify all GPIO pin assignments against [T-Deck schematic](https://github.com/Xinyuan-LilyGO/T-Deck) — done from docs, needs hardware validation
- [x] Create TFT_eSPI `User_Setup.h` for T-Deck ST7789 config
- [x] Test first compile: `pio run -e t-deck` — compiles clean (586KB)
- [x] Fix all compile errors — all resolved
- [ ] Flash to T-Deck, verify serial output appears
- [ ] Confirm display shows something (even just a black screen with backlight)

## Keyboard Driver

- [ ] Implement BBQ10KB I2C driver (`src/hardware/Keyboard.h/cpp`)
- [ ] Key event mapping to LVGL
- [ ] Special keys: Enter, Esc, Tab, Backspace
- [ ] Test all keys produce correct input

## MeshCore Integration

- [ ] Create `TDeckBoard` class implementing `mesh::MainBoard`
  - `getBattMilliVolts()` — read ADC
  - `getMCUTemperature()` — ESP32 internal temp
  - `reboot()` — ESP.restart()
  - `getManufacturerName()` — return "LilyGo"
  - `getStartupReason()` — check RTC memory
- [ ] Create `TDeckClock` class implementing `mesh::RTCClock`
  - `getCurrentTime()` — from GPS or manual set
  - `setCurrentTime()` — set from BLE companion or GPS
- [ ] Wire MeshCore radio init with SX1262 pin config
- [ ] Wire MeshCore serial interface (BLE companion)
- [ ] Identity: generate key on first boot, store in SPIFFS
- [ ] Test: can we see adverts from other nodes?

## UI

- [ ] ScreenHome: wire send button to MeshService::sendChannel()
- [ ] ScreenHome: wire incoming messages to bubble list
- [ ] ScreenMap: create LVGL canvas for tile rendering
- [ ] ScreenMap: implement touch/trackball pan
- [ ] ScreenMap: implement zoom controls
- [ ] ScreenSettings: implement all config fields with live save
- [ ] ScreenTerminal: create MeshCore CLI passthrough
- [ ] ScreenLock: implement with auto-dimming
- [ ] Test: trackball navigation between all screens

## Map

- [ ] Integrate PNG decoder (lodepng or lvgl_png)
- [ ] SD card init and tile directory scan on boot
- [ ] Render first tile to screen (even one tile = milestone)
- [ ] Pan with trackball
- [ ] Zoom in/out
- [ ] Node markers
- [ ] Node info popup on tap
- [ ] PSRAM tile cache (LRU eviction)
- [ ] Write `scripts/download_tiles.py`

## Testing

- [x] Unit test: MapEngine coordinate conversion (lat/lng to/from tile) — 34 tests passing
- [x] Unit test: Config save/load round-trip
- [ ] Integration test: SPIFFS read/write under load
- [ ] Hardware test: keyboard scan produces expected keycodes
- [ ] Hardware test: GPS NMEA sentences decode correctly
- [ ] Stress test: 24h continuous operation, check heap fragmentation

## Polish

- [ ] Font size audit (nothing below 10px)
- [ ] Touch responsiveness tuning
- [ ] Trackball debounce and acceleration
- [ ] Sound feedback (buzzer) on message receive
- [ ] Battery icon in status bar with live voltage
- [x] OTA firmware update via SD card
- [x] Release binary + checksums on GitHub
- [x] Config export/import via SD card (ConfigExport.h/cpp)