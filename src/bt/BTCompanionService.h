// Saitama — BTCompanionService.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Thin singleton that owns the BLE hardware interface (SerialBLEInterface).
// All MeshCore companion protocol logic lives in OPSMesh (MeshService.cpp).

#pragma once
#include <cstdint>
#include <cstring>
#include <helpers/BaseSerialInterface.h>
#include <helpers/esp32/SerialBLEInterface.h>
#include <esp_bt_defs.h>

namespace ops {

class BTCompanionService {
public:
    static BTCompanionService& instance();

    void init(const char* deviceName, uint32_t pinCode = 0);
    void stop();

    // Called from UIScreen::tick() — deferred esp_ble_set_encryption().
    // Must run from the main loop, NOT inside any Bluedroid callback.
    void tick();

    bool isRunning()   const { return _running; }
    bool isConnected() const;

    SerialBLEInterface& getInterface() { return _ble; }

    // Called by the GATTS connect hook (callback context — no BLE API calls allowed).
    void scheduleMitmEncrypt(const uint8_t* remoteBda);

private:
    BTCompanionService() = default;
    SerialBLEInterface _ble;
    bool _running    = false;
    bool _bleInited  = false;
    bool _pendingEnc = false;
    esp_bd_addr_t _pendingBda = {};
};

}  // namespace ops
