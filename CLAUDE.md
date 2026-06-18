# CLAUDE.md — Claude Code guide for Saitama

Saitama is open-source standalone firmware for the LilyGo T-Deck Plus (ESP32-S3, SX1262 LoRa, ST7789 320×240, BBQ10 keyboard, trackball, built-in GPS). It runs MeshCore as a git submodule and LVGL 9.5 for the UI.

---

## Hard Constraints

- **Do not modify `src/hardware/Board.cpp`** — it is considered hardware-stable. All hardware workarounds go in the layer that calls into Board (e.g. UIScreen.cpp).
- **Do not modify the GPS screen layout in `src/ui/ScreenPlaceholder.cpp`** — the skyplot design is intentional and signed off. Only touch it for build errors.
- **Do not modify anything inside `lib/MeshCore/`** — it is a git submodule. Contribute upstream if changes are needed there.
- **LVGL version is 9.5.0** — use LVGL 9 APIs. Key API changes from LVGL 8: `lv_display_create`/`lv_indev_create` (not `lv_disp_drv_t`); `lv_obj_send_event` (not `lv_event_send`); `lv_tick_set_cb(millis)` called after `lv_init()` (no `LV_TICK_CUSTOM`); canvas uses `lv_canvas_init_layer`/`lv_canvas_finish_layer`; `lv_color_t` is RGB888 (use `lv_color_to_u16()` for RGB565). See `src/ui/lv_alloc.cpp` for the custom PSRAM allocator.
- **LVGL config is `lib/lv_conf.h`** — there is also a root-level `lv_conf.h` kept in sync with it. Always edit both when changing LVGL settings. The build picks up whichever the compiler finds first on the include path.

---

## Hardware — LilyGo T-Deck Plus

| Signal | GPIO | Notes |
|---|---|---|
| BOARD_POWERON | 10 | Drive HIGH first — powers all peripherals |
| BOARD_BAT_ADC | 4 | Battery voltage divider (100k/100k) |
| TFT_CS | 12 | ST7789 |
| TFT_DC | 11 | |
| TFT_BL | 42 | Backlight |
| SPI_SCK | 40 | Shared: TFT + LoRa + SD |
| SPI_MOSI | 41 | |
| SPI_MISO | 38 | |
| LORA_CS | 9 | SX1262 |
| LORA_RST | 17 | |
| LORA_DIO1 | 45 | |
| LORA_BUSY | 13 | |
| I2C_SDA | 18 | BBQ10 keyboard (0x1F) + GT911 touch |
| I2C_SCL | 8 | |
| TOUCH_INT | 16 | GT911 interrupt |
| KB_INT | 46 | BBQ10 interrupt |
| TRACKBALL UP | 3 | Optical encoder |
| TRACKBALL DOWN | 2 | |
| TRACKBALL LEFT | 15 | |
| TRACKBALL RIGHT | 1 | |
| GPS_TX | 43 | Built-in GPS |
| GPS_RX | 44 | |
| SD_CS | 39 | |
| SPK_BCLK | 7 | MAX98357A I2S amp — I2S_NUM_0 |
| SPK_WS | 5 | |
| SPK_DOUT | 6 | |

**GT911 touchscreen**: Address is non-deterministic at boot (0x5D or 0x14 depending on INT pin state when POWERON goes high). Always probe both addresses at startup. See `src/ui/UIScreen.cpp` for the probe implementation.

**SPI bus is shared** — TFT, LoRa, and SD all share SCK/MOSI/MISO. Never hold a CS pin low across blocking operations.

**Contacts capacity** — `contacts::CAPACITY = 50` in `src/utils/Contacts.h`. Stored as NVS blobs in namespace `opsct`. Safe to raise; NVS can handle ~100 blobs per namespace on the 16 MB flash layout.

---

## Project Layout

```
src/
  main.cpp                  — setup() + loop()
  hardware/Board.h/cpp      — hardware abstraction (READ-ONLY — do not edit Board.cpp)
  mesh/MeshService.h/cpp    — MeshCore bridge, RxMessage queue, RadioStats
  ui/UIScreen.h/cpp         — LVGL init, display/touch/trackball drivers, tick loop
  ui/ScreenBoot.h/cpp       — Splash screen
  ui/ScreenLauncher.h/cpp   — 4×3 app grid
  ui/ScreenHome.h/cpp       — Chat screen (channel tabs + DM)
  ui/ScreenTerminal.h/cpp   — Serial-style terminal + CDC echo
  ui/ScreenSettings.h/cpp   — Settings
  ui/ScreenHeard.h/cpp      — Peer list (RSSI, last-seen, type badge)
  ui/ScreenContacts.h/cpp   — Contact directory with DM / path actions
  ui/ScreenRepeaters.h/cpp  — Repeater directory
  ui/ScreenTrace.h/cpp      — Hop-by-hop trace tool
  ui/ScreenSignal.h/cpp     — Live radio stats (RSSI/SNR/noise, packet counts, airtime, HW info)
  ui/ScreenPlaceholder.h/cpp — GPS skyplot (do not redesign)
  ui/Theme.h/cpp            — Colour palette
  utils/Config.h/cpp        — Persistent config via NVS; SD backup /ops/settings.json
  utils/Contacts.h/cpp      — 50-slot NVS contact list; SD archive /ops/contacts.json
  utils/Repeaters.h/cpp     — Repeater list (NVS-backed)
  utils/SDCard.h/cpp        — SD mount (CS=39, FSPI), /ops/ dir, binary file helpers
  utils/Sound.h/cpp         — I2S audio (MAX98357A); playPing()
  utils/Log.h               — OPS_LOG(tag, fmt, ...) macro → CDC serial
lib/
  MeshCore/                 — Git submodule — never modify
```

---

## Architecture

**Boot flow:** `ScreenBoot` (2.5 s splash) → `ScreenLauncher` (4×3 grid) → individual screens.

**Screen pattern — lazy create:** Each screen's static `show()` creates LVGL objects on the first call and reuses them on subsequent calls. Navigation back to the launcher calls `ScreenLauncher::show()` directly (not `ops::ui::showLauncher()` — that wrapper exists but `UIScreen.h` is not typically included in screen files; `ScreenLauncher.h` is always present).

**Incoming mesh messages:** `UIScreen::tick()` drains `MeshService::dequeueMessage()` each frame (up to 8 per tick) and routes each `RxMessage` to `ScreenTerminal::appendLine()` and `ScreenHome::appendMessage()`. `ScreenTerminal::appendLine()` also echoes every line to CDC serial — it is the central logging point for received messages. If `cfg.notifyPopup` is set, `_showNotifyPopup()` overlays a dismissible toast on the current screen.

**ScreenHome chat tabs:** A channel tab is shown only when the channel has both `name[0]` and `shortname[0]` set. Tabs display the shortname. The tab bar uses slots 0 (Public), 1–4 (custom channels), 5 (DM). Opening a DM or channel tab lazily loads that conversation's SD log via `_loadChannelHistory(tag)` — each tag is loaded at most once per session (tracked in `s_loadedTags[10]`). The `s_loadingFromSD` flag suppresses SD writes during replay. DM history tags use the format `DM_<contactName>`.

**Screensaver:** `UIScreen::tick()` tracks `s_lastActivityMs` (reset on any trackball/key/touch). When `cfg.screenTimeoutSec > 0` and the device is idle past that threshold, `_activateScreensaver()` saves `lv_scr_act()` to `s_prevScreen`, creates a black LVGL screen with the time in 36pt Montserrat, and loads it. Any input calls `_deactivateScreensaver()` which restores `s_prevScreen` and deletes the saver screen. Screensaver clock updates every 30 s alongside the launcher clock tick.

**Key APIs:**
```cpp
// Receive
bool MeshService::dequeueMessage(RxMessage& out);  // pops one from ring buffer

// Transmit
bool MeshService::sendChannel(const char* text);
bool MeshService::sendDirect(const uint8_t* pubKeyPrefix4, const char* text);
bool MeshService::sendAdvert(int delayMs = 0);

// ACK tracking (for DM sent-bubble ✓ tick)
bool MeshService::pollAck(uint32_t& acked_crc);  // returns true once per ACK

// Repeater queries (async — poll responses each tick)
bool MeshService::sendRepeatersStatus(int timeoutSecs = 30);  // GET_STATUS to all repeaters
bool MeshService::sendRepeaterLogin(const uint8_t* prefix4, const char* password);
bool MeshService::sendAdminCommand(const uint8_t* prefix4, const char* command);
bool MeshService::pollContactResponse(char* out, int outMax);  // dequeues one response line

// Logging
OPS_LOG("Tag", "fmt %d", value);   // → Serial.printf("[OPS] Tag: fmt N\n")
```

**RxMessage fields:** `senderName[32]`, `text[160]`, `rssi`, `snr`, `isDirect`, `channelName[16]` (empty when isDirect), `timestamp`, `hops`, `pubKeyPrefix[4]`.

**RadioStats fields** (returned by `MeshService::radioStats()`): `packetsSent`, `packetsRecv`, `packetsRecvError`, `lastRssi`, `lastSnr`, `noiseFloor`, `radioOk`, `active` — plus per-type counters sourced from the MeshCore `Dispatcher` base class: `floodSent`, `floodRecv`, `directSent`, `directRecv`, `airtimeTxMs`, `airtimeRxMs`.

**ScreenSignal back-navigation pattern:** The scrollable body container is added to the default LVGL input group (`lv_group_focus_obj(_body)`). The keyboard handler in `UIScreen::tick()` routes backspace → `LV_KEY_ESC` to the focused widget. `_onKey` on the body checks for `LV_KEY_ESC` and calls `ScreenLauncher::show()`. Trackball up/down scrolls via the encoder indev naturally because the body is the focused scrollable object.

**Async contact response pipeline (repeater queries):** `OPSMesh::onContactResponse()` decodes repeater replies — a single `0x00` byte means login OK; any other bytes are treated as null-terminated stats text (JSON from `StatsFormatHelper`). Decoded lines are enqueued in a `_respBuf[8][132]` ring buffer inside `OPSMesh`. `UIScreen::tick()` drains the queue each frame via `MeshService::pollContactResponse()` and routes lines to `ScreenTerminal::appendLine()`. This means repeater query results always appear in the Terminal regardless of which screen is active.

**Repeater routing table preload:** The `Repeater` struct stores a full `pubKey[32]` (populated when a repeater advert is heard). `preloadNvsRepeaters()` runs at `OPSMesh::init()` right after `preloadNvsContacts()` and inserts every saved repeater with a non-zero key into MeshCore's contact routing table. This ensures `sendRepeaterLogin()` and `sendAdminCommand()` can find the contact immediately on boot without waiting for another advert. SD JSON stores the full key as `"pubKey64"` (64 hex chars); the field is optional so old JSON files load gracefully with the key zeroed until the repeater is heard again.

**ScreenRepeaters popup:** Tapping a row shows a 5-button action popup (auto-height via `LV_SIZE_CONTENT`): Admin Login, Set Path, Reset Path (orange), Delete Repeater (red), Close. Admin Login opens a password-textarea dialog — on confirm calls `sendRepeaterLogin()` and `ScreenTerminal::setAdminTarget()` so `/repadmin` picks up the target immediately. Set Path opens a hex-input dialog with a 1-byte/2-byte hash-size toggle — on Save calls `setContactPath()` and echoes the result to Terminal. Reset Path calls `resetContactPath()` directly and echoes a confirmation.

**ScreenContacts popup:** Same Set Path dialog and working Reset Path as ScreenRepeaters. Popup also uses `LV_SIZE_CONTENT` for auto-height.

**SD card storage** — `/ops/` directory on the SD card acts as a reflash-proof backup:
- `/ops/contacts.json` — full contact archive (ArduinoJson 7, written on every `contacts::save()`)
- `/ops/repeaters.json` — repeater archive; includes `"pubKey64"` (64 hex chars) for full 32-byte key; field is optional for backward compatibility
- `/ops/settings.json` — config backup (written on every `config::save()`)
- `/ops/identity.bin`  — raw copy of LittleFS `/mesh/self.id` (128 bytes: pub_key[32] + prv_key[64] + name[32]); restored to LittleFS on boot if missing
- `/ops/msgs/<tag>.log` — per-channel/DM message history; one compact JSON line per message; written when `cfg.saveMsgs` is true; tag is channel name or `DM_<contactName>` for direct messages
- `sdcard::init()` must run before `config::init()` and `contacts::init()` (see `main.cpp`)
- ArduinoJson dependency: `bblanchon/ArduinoJson @ ^7.0` in `platformio.ini`

---

## Code Conventions

1. **Allman/BSD braces** — opening brace on its own line.
2. **4-space indentation** — no tabs.
3. **`ops::` namespace** — all project code. Never global namespace.
4. **C++14 only** — the ESP32-S3 Arduino toolchain does not support C++17.
5. **Reserved macro names** — `MAX_CONTACTS`, `MR`, `IR` are already `#define`d by SDK/MeshCore headers. Avoid these as identifiers; use namespaced names instead (e.g. `contacts::CAPACITY`).
6. **No `String` (Arduino class)** in hot paths — use `snprintf()` into fixed stack buffers.
6. **No `new`/`malloc` after `setup()`** — use `ps_malloc()` for anything over 4 KB that needs the heap. Internal DRAM is 320 KB shared.
7. **No `delay()`** in the main loop — use `millis()` timers or state machines.
8. **PSRAM is available** (8 MB OPI) — use `ps_malloc()` for large, long-lived allocations (map tiles, message queues).
9. **License header on every source file:**
   ```cpp
   // Saitama — FileName.h
   // Copyright 2026 Saitama — GPL-3.0-or-later
   ```

---

## Build

```bash
pio run -e t-deck            # full build
pio run -e t-deck -t upload  # upload (normal, overwrites everything)
pio run -e t-deck-launcher -t upload  # upload to ota_1 slot (launcher-safe)
```

Monitor: `pio device monitor` at 115200 baud.

---

## What Not To Do

- Don't add libraries without discussion — binary size and supply chain risk matter on embedded.
- Don't use LVGL 9 APIs — the project is pinned to 8.3.4 and it works.
- Don't edit `Board.cpp` — changes go in callers instead.
- Don't edit `lib/MeshCore/` — contribute upstream.
- Don't commit `.bin`/`.elf`/`.hex` binaries — CI builds them.
- Don't use `new`/`malloc` for large buffers after setup — use `ps_malloc()`.
- Don't modify CI workflows without maintainer approval.


## Versioning

See [docs/VERSIONING.md](docs/VERSIONING.md). Key points:

- Semantic Versioning with pre-release tags: `MAJOR.MINOR.PATCH-alpha.N`
- Current: `0.1.0-alpha.1` (first compile, not flashed)
- Update `src/version.h` before tagging a release
- Version numbers only go forward. Never re-tag.

## Firmware Variants

Each release has two binaries:

1. **App-only** (`saitama-X.Y.Z.bin`) — flash at `0x10000`, for OTA updates
2. **Merged** (`saitama-X.Y.Z-merged.bin`) — flash at `0x0`, bootloader + partitions + app

The merged binary is required for first-time flash or full recovery. The app-only binary is smaller and faster for updates.

## Security

- Report vulnerabilities privately to maintainers, not in public issues
- The CI security audit workflow scans incoming PRs for suspicious patterns (binary files, workflow modifications, credential leaks, new dependencies)
- **Never commit secrets, tokens, or API keys** — even in test code
