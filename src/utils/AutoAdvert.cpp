// Saitama — AutoAdvert.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "AutoAdvert.h"
#include "SDCard.h"
#include "Log.h"
#include "../mesh/MeshService.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>

namespace ops {
namespace autoadvert {

static constexpr const char* PATH          = "/ops/advert_config.json";
static constexpr uint32_t    RETRY_MS      = 60UL * 1000UL;   // mesh not ready yet
static constexpr uint32_t    CHECK_EVERY_MS = 5000;

static int      s_choice     = 0;
static uint32_t s_lastSentMs = 0;
static uint32_t s_lastCheck  = 0;
static bool     s_loaded     = false;   // false = SD was not readable yet

static int _indexForHours(int hours)
{
    for (int i = 0; i < NUM_CHOICES; i++) {
        if (HOURS[i] == hours) return i;
    }
    return 0;
}

// Reads the file into s_choice. True once the card was readable, whether or
// not the file exists (no file = Off, not an error).
static bool _load()
{
    if (!sdcard::isMounted() && !sdcard::tryMount()) return false;
    File f = SD.open(PATH, FILE_READ);
    if (!f) {
        OPS_LOG("AutoAdv", "%s not found - auto advert off", PATH);
        return true;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        OPS_LOG("AutoAdv", "%s parse: %s", PATH, err.c_str());
        return true;
    }
    s_choice = _indexForHours(doc["intervalHours"] | 0);
    OPS_LOG("AutoAdv", "Loaded interval: %d h", HOURS[s_choice]);
    return true;
}

void init()
{
    s_choice     = 0;
    s_lastSentMs = millis();
    s_loaded     = _load();
    if (!s_loaded) OPS_LOG("AutoAdv", "SD not mounted - will retry");
}

int choice()
{
    if (!s_loaded) s_loaded = _load();   // card mounted after boot
    return s_choice;
}

bool setChoice(int idx)
{
    if (idx < 0 || idx >= NUM_CHOICES) return false;
    s_choice     = idx;
    s_lastSentMs = millis();
    s_loaded     = true;   // the user's choice wins over a late load
    if (!sdcard::isMounted()) return false;
    File f = SD.open(PATH, FILE_WRITE);
    if (!f) return false;
    JsonDocument doc;
    doc["intervalHours"] = HOURS[idx];
    serializeJson(doc, f);
    f.close();
    OPS_LOG("AutoAdv", "Saved interval: %d h", HOURS[idx]);
    return true;
}

void noteAdvertSent()
{
    s_lastSentMs = millis();
}

uint32_t secondsUntilNext()
{
    if (s_choice == 0) return 0;
    uint32_t intervalMs = (uint32_t)HOURS[s_choice] * 3600UL * 1000UL;
    uint32_t elapsed    = millis() - s_lastSentMs;
    return elapsed >= intervalMs ? 0 : (intervalMs - elapsed) / 1000UL;
}

void tick()
{
    uint32_t now = millis();
    if (now - s_lastCheck < CHECK_EVERY_MS) return;
    s_lastCheck = now;
    if (!s_loaded) {
        // Retry the load at most once a minute; tryMount() touches the bus.
        static uint32_t lastTry = 0;
        if (now - lastTry < RETRY_MS) return;
        lastTry = now;
        s_loaded = _load();
        if (!s_loaded) return;
    }
    if (s_choice == 0) return;

    uint32_t intervalMs = (uint32_t)HOURS[s_choice] * 3600UL * 1000UL;
    if (now - s_lastSentMs < intervalMs) return;

    if (MeshService::instance().sendAdvert(0, true)) {
        OPS_LOG("AutoAdv", "Auto advert sent (every %d h)", HOURS[s_choice]);
    } else {
        // Mesh not up: try again in a minute rather than every check.
        s_lastSentMs = now - intervalMs + RETRY_MS;
    }
}

}  // namespace autoadvert
}  // namespace ops
