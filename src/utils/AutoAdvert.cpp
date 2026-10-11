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

static int _indexForHours(int hours)
{
    for (int i = 0; i < NUM_CHOICES; i++) {
        if (HOURS[i] == hours) return i;
    }
    return 0;
}

void init()
{
    s_choice     = 0;
    s_lastSentMs = millis();
    if (!sdcard::isMounted()) return;
    File f = SD.open(PATH, FILE_READ);
    if (!f) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        OPS_LOG("AutoAdv", "%s parse: %s", PATH, err.c_str());
        return;
    }
    s_choice = _indexForHours(doc["intervalHours"] | 0);
    OPS_LOG("AutoAdv", "Interval: %d h", HOURS[s_choice]);
}

int choice()
{
    return s_choice;
}

bool setChoice(int idx)
{
    if (idx < 0 || idx >= NUM_CHOICES) return false;
    s_choice     = idx;
    s_lastSentMs = millis();
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

void tick()
{
    uint32_t now = millis();
    if (now - s_lastCheck < CHECK_EVERY_MS) return;
    s_lastCheck = now;
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
