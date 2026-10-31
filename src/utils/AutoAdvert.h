// Saitama — AutoAdvert.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include <stdint.h>

namespace ops {
namespace autoadvert {

// Periodic flood advert (carries the position when Settings > Location is on).
// Chosen on the Advertise screen; stored in /ops/advert_config.json on the SD
// card as {"intervalHours": N}, N = 0 (off), 3, 6, 12 or 24.

static constexpr int NUM_CHOICES = 5;
// Hours for each choice index; index 0 = Off. Matches OPTIONS below.
static constexpr uint8_t HOURS[NUM_CHOICES] = { 0, 3, 6, 12, 24 };
static constexpr const char* OPTIONS = "Off\n3 hrs\n6 hrs\n12 hrs\n24 hrs";

void init();                  // load from SD (Off if missing); after sdcard::init()
int  choice();                // index into HOURS
bool setChoice(int idx);      // saves to SD; restarts the timer. False if the write failed.
void noteAdvertSent();        // any advert restarts the countdown
void tick();                  // call every UI frame; sends when due
uint32_t secondsUntilNext();  // 0 when auto advert is off

}  // namespace autoadvert
}  // namespace ops
