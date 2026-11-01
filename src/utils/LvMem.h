// Saitama — LvMem.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Low-memory guard for boards without PSRAM, where LVGL objects share the
// internal heap with BLE, the radio and the SD driver. LVGL ran on after a
// failed allocation and crashed on the NULL object (LoadProhibited in
// lv_obj_add_style). Instead:
//  - a reserve block is held from boot; when an LVGL allocation fails, the
//    reserve is freed and the allocation retried, so the screen still opens;
//  - ui::tick() then sees lowMemHit(), frees the hidden screens, shows a
//    notice and later takes the reserve back (restoreReserve()).
// With PSRAM every call here is a no-op.

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ops { namespace lvmem {

static constexpr size_t RESERVE_BYTES = 12 * 1024;
// Below this largest free block the launcher frees hidden screens before
// opening an app (an app screen needs a few KB in one piece).
static constexpr size_t OPEN_MIN_BLOCK = 16 * 1024;

void   init();             // take the reserve (after the display buffers)
bool   lowMemHit();        // true once after the reserve was spent
void   restoreReserve();   // take it back if there is room again
bool   reserveHeld();
uint32_t hits();           // reserve spends since boot
uint32_t failedAllocs();   // any failed heap allocation since boot
size_t lastFailedSize();
size_t largestFree();      // largest free internal block

}}  // namespace ops::lvmem
