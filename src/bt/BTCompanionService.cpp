// Saitama — BTCompanionService.cpp
// Copyright 2026 Saitama — MIT License

#include "BTCompanionService.h"
#include "../utils/Log.h"
#include <cstring>

namespace ops {

BTCompanionService& BTCompanionService::instance()
{
    static BTCompanionService s;
    return s;
}

void BTCompanionService::init(const char* deviceName, uint32_t pinCode)
{
    if (!_bleInited) {
        char name[32];
        strncpy(name, deviceName ? deviceName : "OMS-NODE", sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        _ble.begin("OMS", name, pinCode);
        _bleInited = true;
        OPS_LOG("BT", "BLE stack init as 'OMS %s'", name);
        // First call: enable() is invoked by startCompanionInterface() next,
        // which starts the GATT service and advertising for the first time.
    } else {
        // Second+ call: GATT service is already registered.
        // enable() (called next by startCompanionInterface) has an internal
        // guard — if (_isEnabled) return — so it is a safe no-op here because
        // we never call disable(), keeping _isEnabled true at all times.
        // Restart advertising directly so the device becomes discoverable again.
        BLEDevice::getAdvertising()->start();
        OPS_LOG("BT", "BLE advertising restarted");
    }
    _running = true;
}

void BTCompanionService::stop()
{
    if (!_bleInited) return;
    // Stop advertising via BLEDevice::getAdvertising() instead of _ble.disable().
    // _ble.disable() calls pService->stop() (GATT layer teardown); the next
    // _ble.enable() then calls pService->start() which re-registers already-
    // registered characteristics and corrupts the ESP32 BLE heap —
    // observed as "Malloc failed" / "hash_map_set" assert on the second connect.
    // stopCompanionInterface() has already nulled _btSerial, preventing
    // checkRecvFrame() from running and suppressing the auto-restart timer.
    BLEDevice::getAdvertising()->stop();
    _running = false;
    OPS_LOG("BT", "BLE advertising stopped");
}

bool BTCompanionService::isConnected() const
{
    return _running && _ble.isConnected();
}

}  // namespace ops
