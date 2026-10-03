// Saitama — main entry point
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Initialises hardware, MeshCore, and the UI task loop.

#include <Arduino.h>
#include <WiFi.h>
#include <nvs.h>
#include "version.h"
#include "hardware/Board.h"
#include "mesh/MeshService.h"
#include "ui/UIScreen.h"
#include "utils/Log.h"
#include "utils/LoopStats.h"
#include "utils/Config.h"
#include "utils/Contacts.h"
#include "utils/Crypto.h"
#include "utils/BtPin.h"
#include "utils/Regions.h"
#include "utils/Repeaters.h"
#include "utils/SDCard.h"
#include "utils/Sound.h"
#include "ui/ScreenTerminal.h"
#include "bt/BTCompanionService.h"

// ── Setup ───────────────────────────────────────────────────────────
void setup() {
    // Must precede begin(): HWCDC only creates its TX ring (default 256 B) if
    // none exists. With the zero timeout below, any burst larger than the
    // ring is silently truncated — multi-line terminal replies (help, card,
    // identity show) lost their tail. 4 KB absorbs the largest reply.
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    // Never block on USB CDC. Once a host has been seen, HWCDC defaults to a
    // 100 ms write timeout, so every log line stalls the loop (and delays
    // reading the radio) whenever the port is plugged in but nobody is
    // reading it. With 0, output is dropped instead when the buffer is full.
    Serial.setTxTimeoutMs(0);
    delay(500);   // Short settle; ARDUINO_USB_CDC_ON_BOOT=1 means CDC is up at boot

    OPS_LOG("main", "Saitama v" OPS_VERSION_STRING " starting");

    // 0) Kill WiFi modem — this build uses SX1262 LoRa, not ESP-NOW/WiFi.
    //    Without this, the WiFi radio idles powered-on and wastes ~1-2 mA.
    WiFi.mode(WIFI_OFF);

    // 1) Initialise board-level hardware (power rail, GPIO, I2C, GPS serial)
    ops::Board::instance().init();

    // 1a) Initialise I2S speaker (after BOARD_POWERON is high, before UI)
    ops::sound::init();

    // 2) Mount SD card (must come before config/contacts so SD JSON is available)
    ops::sdcard::init();

    // 3) Load persistent config and contacts from NVS (falls back to SD if NVS empty)
    // Storage key first: settings.json (channel PSKs) and repeaters.json
    // (admin passwords) hold sealed secrets.
    ops::crypto::init();
    ops::btpin::init();
    ops::config::init();
    ops::config::scrubSdSecrets();
    ops::contacts::init();
    ops::regions::init();
    ops::repeaters::init();
    {
        // NVS is 16 KB: a full partition makes config/contact saves fail.
        nvs_stats_t st;
        if (nvs_get_stats(nullptr, &st) == ESP_OK)
            OPS_LOG("NVS", "%u/%u entries used, %u free, %u namespaces",
                    (unsigned)st.used_entries, (unsigned)st.total_entries,
                    (unsigned)st.free_entries, (unsigned)st.namespace_count);
    }

    // Apply saved keyboard backlight state on boot
    ops::Board::instance().setKeyboardBacklight(ops::config::get().kbBrightness);

    // Play startup jingle now that config is loaded (I2S was init'd in step 1a).
    // Audio queues into DMA here and finishes playing during ui::init() below.
    ops::sound::playStartupJingle();

    // 4) Pre-initialise BLE controller BEFORE LVGL allocates DMA SRAM.
    //    BLEDevice::init() (inside BTCompanionService::init) claims ~60 KB of
    //    DMA-capable internal SRAM for the BT controller workspace.  LVGL also
    //    needs ~51 KB of DMA SRAM for its double draw-buffer.  If BLE waits
    //    until after ui::init(), there is not enough contiguous DRAM left and
    //    the HCI host layer fails to start ("Start HCI Host Layer Failure").
    //    Only the hardware-level BLE stack is started here; the companion GATT
    //    service and MeshCore wiring happen in step 6 after the mesh is up.
    if (ops::config::get().bluetoothEnabled) {
        const auto& cfg = ops::config::get();
        ops::BTCompanionService::instance().init(
            cfg.callsign[0] ? cfg.callsign : "OPS-NODE", ops::btpin::get());
    }

    // 5) Initialise UI (LVGL + screen driver) BEFORE LoRa.
    //    Both share the FSPI bus (SCK=40 MISO=38 MOSI=41). tft.begin() inside
    //    ui::init() reconfigures FSPI; if LoRa is initialised first its SX1262
    //    is taken out of RX mode when the TFT later re-init's the bus.
    ops::ui::init();

    // 6) Initialise MeshCore radio + protocol stack (after TFT owns FSPI)
    ops::MeshService::instance().init();

    // 7) Wire BLE companion to MeshCore (GATT service start + advertising).
    //    BTCompanionService::init() was already called above so _bleInited=true;
    //    this second call skips the hardware re-init and only restarts advertising
    //    + wires the serial interface now that the mesh is initialised.
    if (ops::config::get().bluetoothEnabled)
        ops::MeshService::instance().startCompanionBLE();

    OPS_LOG("main", "Ready");
    Serial.println("\r\nSaitama serial console ready — type help and press Enter");
    Serial.print("OPS> ");
}

// ── Loop ────────────────────────────────────────────────────────────
void loop() {
    using namespace ops::loopstats;
    mark();
    begin(BOARD);     ops::Board::instance().tick();             end(BOARD);
    begin(MESH);      ops::MeshService::instance().tick();       end(MESH);
    begin(UI);        ops::ui::tick();                           end(UI);
    begin(SERIAL_IO); ops::ui::ScreenTerminal::tickSerial();     end(SERIAL_IO);

    // Yield one RTOS tick (1 ms) per pass. Everything above is polled and
    // finishes in well under a frame, so without this the loop spun at full
    // clock between 33 ms LVGL frames; the idle task now halts the core
    // (WAITI) instead. Not a delay() in the "no delay in loop" sense: the radio
    // (DIO1 interrupt + RX buffer), keyboard and GPS UART all buffer for far
    // longer than 1 ms. Screen-off light sleep in ui::tick() is unaffected.
    vTaskDelay(1);
}
