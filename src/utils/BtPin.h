// Saitama — BtPin.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Bluetooth companion pairing PIN. Generated randomly per device on first
// boot (it used to be a fixed 123456 on every node); settable in Settings.
// NVS only (namespace "opsbt") — never mirrored to the SD card.

#pragma once
#include <cstdint>

namespace ops {
namespace btpin {

static constexpr uint32_t MIN_PIN = 100000;
static constexpr uint32_t MAX_PIN = 999999;

// Loads the PIN, creating a random one if there is none.
void     init();
uint32_t get();
// Sets a 6-digit PIN (MIN_PIN..MAX_PIN). False if out of range or unsaved.
bool     set(uint32_t pin);
// Picks and saves a new random PIN; returns it.
uint32_t randomize();
// True (once) when existing pairings must be removed — the PIN changed, or
// this device has just moved off the old shared default. Persisted, so it
// still applies if Bluetooth is off until a later boot.
bool     takeBondClear();

}  // namespace btpin
}  // namespace ops
