// Saitama — GpsMgr.h
// Copyright 2026 Saitama — MIT License
//
// GPS power management state machine.
// Modes: 0=off, 1=intermittent, 2=on
//
// Intermittent: wait for first RTC sync → standby 1 hr → wake → resync window
// 5 min → if fail → revert to off.

#pragma once
#include <Arduino.h>

// GPS mode constants (matches Config::gpsMode)
static constexpr uint8_t GPS_MODE_OFF   = 0;
static constexpr uint8_t GPS_MODE_INTER = 1;
static constexpr uint8_t GPS_MODE_ON    = 2;

namespace ops {

class GpsMgr {
public:
    static GpsMgr& instance();

    // Call once from UIScreen::init() after config is loaded.
    void init();

    // Call every frame from UIScreen::tick().
    void tick();

    // Call when the user changes cfg.gpsMode (e.g. from settings or terminal).
    void applyMode(uint8_t mode);

    // Estimated GPS current draw in mA: 20 mA when active, 0 when sleeping/off.
    uint8_t estimatedCurrentMA() const;

private:
    enum InterState { INTER_ACTIVE, INTER_SLEEPING, INTER_SYNCING };

    uint8_t     _mode          = GPS_MODE_ON;
    InterState  _interState    = INTER_ACTIVE;
    uint32_t    _lastSyncMs    = 0;   // last Board::gpsLastSyncMs() value we observed
    uint32_t    _sleepUntilMs  = 0;   // millis() when to wake from standby
    uint32_t    _syncDeadlineMs = 0;  // millis() by which resync must succeed

    static constexpr uint32_t SLEEP_MS   = 60UL * 60UL * 1000UL;  // 1 hour
    static constexpr uint32_t RESYNC_MS  =  5UL * 60UL * 1000UL;  // 5 minutes

    void _enterMode(uint8_t mode);
    void _tickInter();
};

}  // namespace ops
