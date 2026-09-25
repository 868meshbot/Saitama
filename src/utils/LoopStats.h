// Saitama — LoopStats.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Measures the gap between successive loop() passes. MeshCore reads a
// received packet out of the SX1262 only when loop() gets round to it, and
// the chip holds just one packet, so a long gap is a window in which a second
// packet overwrites the first. Shown on the Signal screen.

#pragma once
#include <stdint.h>

namespace ops { namespace loopstats {

    // Call once at the top of loop().
    void mark();
    // Call after a deliberate light sleep so it isn't counted as a stall.
    void skipGap();

    // Longest loop gap in the last ~10 s, and since boot (ms).
    uint32_t recentMaxMs();
    uint32_t peakMs();

    // Number of loop gaps over 50 ms / over 250 ms since boot.
    uint32_t stallsOver50();
    uint32_t stallsOver250();

    // Per-section timing, to find which part of loop() a stall is in.
    // UI_DRAW (lv_timer_handler) runs inside UI, so UI includes it.
    enum Section : uint8_t { BOARD = 0, MESH, UI, UI_DRAW, SERIAL_IO, SECTION_COUNT };
    void     begin(Section s);
    void     end(Section s);
    // Longest single pass of a section in the last ~10 s (ms).
    uint32_t sectionRecentMaxMs(Section s);
    const char* sectionName(Section s);

}}  // namespace ops::loopstats
