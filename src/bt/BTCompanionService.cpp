// Saitama — BTCompanionService.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "BTCompanionService.h"
#include "../utils/Log.h"
#include <cstring>
#include <BLEDevice.h>
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_heap_caps.h>

namespace ops {

// ── GATTS connect hook ───────────────────────────────────────────────
// Bluedroid callback context: must NOT call any BLE API that re-enters
// the Bluedroid stack (causes assert emi.c + interrupt WDT crash).
// Only record the peer BDA; tick() issues esp_ble_set_encryption() safely
// from the main loop.
static void _gattsConnectHook(esp_gatts_cb_event_t  event,
                               esp_gatt_if_t         /*gatts_if*/,
                               esp_ble_gatts_cb_param_t* param)
{
    if (event == ESP_GATTS_CONNECT_EVT)
        BTCompanionService::instance().scheduleMitmEncrypt(param->connect.remote_bda);
}

BTCompanionService& BTCompanionService::instance()
{
    static BTCompanionService s;
    return s;
}

void BTCompanionService::scheduleMitmEncrypt(const uint8_t* remoteBda)
{
    memcpy(_pendingBda, remoteBda, sizeof(_pendingBda));
    _pendingEnc = true;
    OPS_LOG("BT", "Connected — free internal DRAM: %u B  PSRAM: %u B",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void BTCompanionService::init(const char* deviceName, uint32_t pinCode)
{
    if (!_bleInited) {
        char name[32];
        strncpy(name, deviceName ? deviceName : "OPS-NODE", sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        _ble.begin("", name, pinCode);
        BLEDevice::setCustomGattsHandler(_gattsConnectHook);
        _bleInited = true;
        OPS_LOG("BT", "BLE stack init as '%s' (PIN: %lu)", name, (unsigned long)pinCode);
    } else {
        BLEDevice::getAdvertising()->start();
        OPS_LOG("BT", "BLE advertising restarted");
    }
    _running = true;
}

void BTCompanionService::stop()
{
    if (!_bleInited) return;
    BLEDevice::getAdvertising()->stop();
    _pendingEnc = false;
    _running    = false;
    OPS_LOG("BT", "BLE advertising stopped");
}

void BTCompanionService::tick()
{
    if (!_pendingEnc) return;
    _pendingEnc = false;
    esp_ble_set_encryption(_pendingBda, ESP_BLE_SEC_ENCRYPT_MITM);
}

bool BTCompanionService::isConnected() const
{
    return _running && _ble.isConnected();
}

}  // namespace ops
