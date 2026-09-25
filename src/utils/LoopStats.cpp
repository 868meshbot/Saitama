// Saitama — LoopStats.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "LoopStats.h"
#include "Log.h"
#include <Arduino.h>

namespace ops { namespace loopstats {

static constexpr uint32_t WINDOW_MS = 10000;

static uint32_t s_lastUs     = 0;
static uint32_t s_windowAt   = 0;
static uint32_t s_curMaxMs   = 0;   // max within the current window
static uint32_t s_prevMaxMs  = 0;   // max of the previous full window
static uint32_t s_peakMs     = 0;
static uint32_t s_over50     = 0;
static uint32_t s_over250    = 0;

static uint32_t s_secStartUs[SECTION_COUNT] = {};
static uint32_t s_secCurMax[SECTION_COUNT]  = {};
static uint32_t s_secPrevMax[SECTION_COUNT] = {};

void mark()
{
    uint32_t nowUs = micros();
    if (s_lastUs != 0) {
        uint32_t gapMs = (nowUs - s_lastUs) / 1000;
        if (gapMs > s_curMaxMs) s_curMaxMs = gapMs;
        if (gapMs > s_peakMs)   s_peakMs   = gapMs;
        if (gapMs > 50)  s_over50++;
        if (gapMs > 250) s_over250++;
    }
    s_lastUs = nowUs;

    uint32_t nowMs = millis();
    if (nowMs - s_windowAt >= WINDOW_MS) {
        s_windowAt  = nowMs;
        s_prevMaxMs = s_curMaxMs;
        s_curMaxMs  = 0;
        for (int i = 0; i < SECTION_COUNT; i++) {
            s_secPrevMax[i] = s_secCurMax[i];
            s_secCurMax[i]  = 0;
        }
    }
}

void skipGap()
{
    s_lastUs = 0;
    s_secStartUs[UI] = micros();   // the sleep happens inside ui::tick()
}

void begin(Section s)
{
    s_secStartUs[s] = micros();
}

void end(Section s)
{
    uint32_t ms = (micros() - s_secStartUs[s]) / 1000;
    if (ms > s_secCurMax[s]) s_secCurMax[s] = ms;
    // Serial is non-blocking (setTxTimeoutMs(0)), so this can't add a stall.
    if (ms > 100) OPS_LOG("Loop", "slow %s: %lu ms", sectionName(s), (unsigned long)ms);
}

uint32_t sectionRecentMaxMs(Section s)
{
    return s_secCurMax[s] > s_secPrevMax[s] ? s_secCurMax[s] : s_secPrevMax[s];
}

const char* sectionName(Section s)
{
    static const char* const kNames[SECTION_COUNT] = { "Board", "Mesh", "UI", "Draw", "Serial" };
    return s < SECTION_COUNT ? kNames[s] : "?";
}

uint32_t recentMaxMs()   { return s_curMaxMs > s_prevMaxMs ? s_curMaxMs : s_prevMaxMs; }
uint32_t peakMs()        { return s_peakMs; }
uint32_t stallsOver50()  { return s_over50; }
uint32_t stallsOver250() { return s_over250; }

}}  // namespace ops::loopstats
