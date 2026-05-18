// Saitama — BTCompanionService.h
// Copyright 2026 Saitama — MIT License
//
// Thin singleton that owns the BLE hardware interface (SerialBLEInterface).
// All MeshCore companion protocol logic lives in OMSMesh (MeshService.cpp).

#pragma once
#include <cstdint>
#include <helpers/BaseSerialInterface.h>
#include <helpers/esp32/SerialBLEInterface.h>

namespace ops {

class BTCompanionService {
public:
    static BTCompanionService& instance();

    // Initialise BLE GATT server (once) and mark running. enable() is called
    // by OMSMesh::startCompanionInterface() immediately after.
    void init(const char* deviceName, uint32_t pinCode = 0);

    // Stop advertising and clear running state.
    void stop();

    bool isRunning()   const { return _running; }
    bool isConnected() const;

    SerialBLEInterface& getInterface() { return _ble; }

private:
    BTCompanionService() = default;
    SerialBLEInterface _ble;
    bool _running   = false;
    bool _bleInited = false;
};

}  // namespace oms
