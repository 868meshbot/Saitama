// Saitama — GpsMgr.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "GpsMgr.h"
#include <sys/time.h>
#include <time.h>
#include "Config.h"
#include "Log.h"
#include "../hardware/Board.h"

namespace ops {

GpsMgr& GpsMgr::instance()
{
    static GpsMgr s;
    return s;
}

void GpsMgr::init()
{
    _mode = config::get().gpsMode;
    _enterMode(_mode);
}

void GpsMgr::applyMode(uint8_t mode)
{
    if (mode == _mode) return;
    _mode = mode;
    _enterMode(mode);
}

void GpsMgr::_enterMode(uint8_t mode)
{
    auto& board = Board::instance();
    switch (mode) {
        case GPS_MODE_OFF:
            board.gpsStandby();
            OPS_LOG("GPS", "Mode: OFF (standby)");
            break;

        case GPS_MODE_ON:
            board.gpsWake();
            OPS_LOG("GPS", "Mode: ON");
            break;

        case GPS_MODE_INTER:
            // Start active — GPS is on, wait for first sync.
            board.gpsWake();
            _interState    = INTER_ACTIVE;
            _lastSyncMs    = board.gpsLastSyncMs();
            _sleepUntilMs  = 0;
            _syncDeadlineMs = 0;
            OPS_LOG("GPS", "Mode: INTERMITTENT (waiting for sync)");
            break;

        default:
            break;
    }
}

void GpsMgr::tick()
{
    // Stay in sync with runtime cfg changes (e.g. from terminal command).
    uint8_t cfgMode = config::get().gpsMode;
    if (cfgMode != _mode) {
        applyMode(cfgMode);
    }

    if (_mode == GPS_MODE_INTER) {
        _tickInter();
    }
    _tickClock();
}

// Board::tick() syncs the clock from the first GPS time it decodes and then
// at most every 10 minutes. That first time can arrive before a fix (the
// module's own clock, which may be stale), and nothing corrected it for
// the next 10 minutes. Every 5 s: with live NMEA and a position fix, put
// the clock right if it is more than 5 s out.
void GpsMgr::_tickClock()
{
    uint32_t now = millis();
    if (now - _clockCheckMs < 5000) return;
    _clockCheckMs = now;

    auto& board = Board::instance();
    uint32_t nmea = board.gpsNmeaCount();
    bool fresh = (nmea != _clockNmea);
    _clockNmea = nmea;
    if (!fresh || !board.hasGPSFix()) return;

    uint16_t y; uint8_t mo, d, h, mi, s;
    if (!board.gpsDateTime(y, mo, d, h, mi, s) || y < 2024) return;
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = s;
    time_t gpsT = mktime(&t);          // TZ is UTC, as in Board::tick()
    time_t sysT = time(nullptr);
    long diff = (long)(gpsT - sysT);
    if (diff > 5 || diff < -5) {
        struct timeval tv = { gpsT, 0 };
        settimeofday(&tv, nullptr);
        OPS_LOG("GPS", "Clock corrected by %ld s: %04u-%02u-%02u %02u:%02u:%02u UTC",
                diff, y, mo, d, h, mi, s);
    }
}

void GpsMgr::_tickInter()
{
    auto& board = Board::instance();
    uint32_t now = millis();
    uint32_t syncMs = board.gpsLastSyncMs();

    switch (_interState) {
        case INTER_ACTIVE:
            // Wait for the GPS to obtain a valid RTC sync.
            if (syncMs != 0 && syncMs != _lastSyncMs) {
                _lastSyncMs = syncMs;
                board.gpsStandby();
                _sleepUntilMs = now + SLEEP_MS;
                _interState   = INTER_SLEEPING;
                OPS_LOG("GPS", "Intermittent: synced — sleeping 1 hr");
            }
            break;

        case INTER_SLEEPING:
            if (now >= _sleepUntilMs) {
                board.gpsWake();
                _syncDeadlineMs = now + RESYNC_MS;
                _interState     = INTER_SYNCING;
                OPS_LOG("GPS", "Intermittent: woke — resync window 5 min");
            }
            break;

        case INTER_SYNCING:
            if (syncMs != 0 && syncMs != _lastSyncMs) {
                // Resync succeeded.
                _lastSyncMs   = syncMs;
                board.gpsStandby();
                _sleepUntilMs = now + SLEEP_MS;
                _interState   = INTER_SLEEPING;
                OPS_LOG("GPS", "Intermittent: resynced — sleeping 1 hr");
            } else if (now >= _syncDeadlineMs) {
                // Failed to resync within 5 min — revert to off.
                board.gpsStandby();
                OPS_LOG("GPS", "Intermittent: resync timeout — reverting to OFF");
                auto& cfg = const_cast<Config&>(config::get());
                cfg.gpsMode = GPS_MODE_OFF;
                config::save();
                _mode = GPS_MODE_OFF;
            }
            break;
    }
}

uint8_t GpsMgr::estimatedCurrentMA() const
{
    if (_mode == GPS_MODE_OFF)  return 0;
    if (_mode == GPS_MODE_ON)   return 20;
    // Intermittent: 0 while sleeping, 20 while active or syncing
    return (_interState == INTER_SLEEPING) ? 0 : 20;
}

}  // namespace ops
