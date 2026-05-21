// Saitama — main entry point
// Copyright 2026 Saitama — MIT License
//
// Initialises hardware, MeshCore, and the UI task loop.

#include <Arduino.h>
#include <WiFi.h>
#include "version.h"
#include "hardware/Board.h"
#include "mesh/MeshService.h"
#include "ui/UIScreen.h"
#include "utils/Log.h"
#include "utils/Config.h"
#include "utils/Contacts.h"
#include "utils/Repeaters.h"
#include "utils/SDCard.h"
#include "utils/Sound.h"
#include "ui/ScreenTerminal.h"

// ── Setup ───────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
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
    ops::config::init();
    ops::contacts::init();
    ops::repeaters::init();

    // Apply saved keyboard backlight state on boot
    ops::Board::instance().setKeyboardBacklight(ops::config::get().kbBrightness);

    // Play startup jingle now that config is loaded (I2S was init'd in step 1a).
    // Audio queues into DMA here and finishes playing during ui::init() below.
    ops::sound::playStartupJingle();

    // 4) Initialise UI (LVGL + screen driver) BEFORE LoRa.
    //    Both share the FSPI bus (SCK=40 MISO=38 MOSI=41). tft.begin() inside
    //    ui::init() reconfigures FSPI; if LoRa is initialised first its SX1262
    //    is taken out of RX mode when the TFT later re-init's the bus.
    ops::ui::init();

    // 5) Initialise MeshCore radio + protocol stack (after TFT owns FSPI)
    ops::MeshService::instance().init();

    // 6) Start BT companion if configured
    if (ops::config::get().bluetoothEnabled)
        ops::MeshService::instance().startCompanionBLE();

    OPS_LOG("main", "Ready");
    Serial.println("\r\nSaitama serial console ready — type /help and press Enter");
    Serial.print("OPS> ");
}

// ── Loop ────────────────────────────────────────────────────────────
void loop() {
    ops::Board::instance().tick();
    ops::MeshService::instance().tick();
    ops::ui::tick();
    ops::ui::ScreenTerminal::tickSerial();
}
