// Saitama — compat/helpers/radiolib/CustomSX1262Wrapper.h
// Copyright 2026 Saitama — MIT License
//
// Shadows lib/MeshCore/src/helpers/radiolib/CustomSX1262Wrapper.h.
// RadioLib 7.6.0 made SX126x::spreadingFactor private. Use LORA_SF build flag
// (the value we configured at init time) instead of the private field.

#pragma once

#include "CustomSX1262.h"
#include <helpers/radiolib/RadioLibWrappers.h>
#include "SX126xReset.h"

#ifndef USE_SX1262
#define USE_SX1262
#endif

#ifndef LORA_SF
#define LORA_SF 8
#endif

class CustomSX1262Wrapper : public RadioLibWrapper {
public:
    CustomSX1262Wrapper(CustomSX1262& radio, mesh::MainBoard& board)
        : RadioLibWrapper(radio, board) { }

    bool isReceivingPacket() override {
        return ((CustomSX1262*)_radio)->isReceiving();
    }
    float getCurrentRSSI() override {
        return ((CustomSX1262*)_radio)->getRSSI(false);
    }
    float getLastRSSI() const override { return ((CustomSX1262*)_radio)->getRSSI(); }
    float getLastSNR() const override { return ((CustomSX1262*)_radio)->getSNR(); }

    float packetScore(float snr, int packet_len) override {
        // spreadingFactor is private in RadioLib >= 7.6.0 — use build flag.
        return packetScoreInt(snr, LORA_SF, packet_len);
    }
    virtual void powerOff() override {
        ((CustomSX1262*)_radio)->sleep(false);
    }

    void doResetAGC() override { sx126xResetAGC((SX126x*)_radio); }

    void setRxBoostedGainMode(bool en) override {
        ((CustomSX1262*)_radio)->setRxBoostedGainMode(en);
    }
    bool getRxBoostedGainMode() const override {
        return ((CustomSX1262*)_radio)->getRxBoostedGainMode();
    }
};
