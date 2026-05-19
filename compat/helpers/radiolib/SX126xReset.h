// Saitama — compat/helpers/radiolib/SX126xReset.h
// Copyright 2026 Saitama — MIT License
//
// Shadows lib/MeshCore/src/helpers/radiolib/SX126xReset.h.
// RadioLib 7.6.0 moved Module* mod and float freqMHz out of the public/protected
// interface. Use getMod() (public since 7.6.0) and LORA_FREQ build flag instead.

#pragma once

#include <RadioLib.h>

inline void sx126xResetAGC(SX126x* radio) {
    radio->sleep(true);
    radio->standby(RADIOLIB_SX126X_STANDBY_RC, true);

    uint8_t calData = RADIOLIB_SX126X_CALIBRATE_ALL;
    radio->getMod()->SPIwriteStream(RADIOLIB_SX126X_CMD_CALIBRATE, &calData, 1, true, false);
    radio->getMod()->hal->delay(5);
    uint32_t start = millis();
    while (radio->getMod()->hal->digitalRead(radio->getMod()->getGpio())) {
        if (millis() - start > 50) break;
        radio->getMod()->hal->yield();
    }

    // Re-calibrate image for the operating frequency.
    // freqMHz is private in RadioLib >= 7.6.0 — use LORA_FREQ build flag.
#ifndef LORA_FREQ
#define LORA_FREQ 868.0f
#endif
    radio->calibrateImage(LORA_FREQ);

#ifdef SX126X_DIO2_AS_RF_SWITCH
    radio->setDio2AsRfSwitch(SX126X_DIO2_AS_RF_SWITCH);
#endif
#ifdef SX126X_RX_BOOSTED_GAIN
    radio->setRxBoostedGainMode(SX126X_RX_BOOSTED_GAIN);
#endif
#ifdef SX126X_REGISTER_PATCH
    uint8_t r_data = 0;
    radio->readRegister(0x8B5, &r_data, 1);
    r_data |= 0x01;
    radio->writeRegister(0x8B5, &r_data, 1);
#endif
}
