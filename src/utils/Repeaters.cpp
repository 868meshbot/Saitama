// Saitama — Repeaters.cpp
// Copyright 2026 Saitama — MIT License

#include "Repeaters.h"
#include "SDCard.h"
#include "Log.h"
#include <Preferences.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <cstring>

namespace ops {

static Repeater s_reps[repeaters::CAPACITY];
static int      s_count = 0;

// ── SD JSON helpers ────────────────────────────────────────────────

static void _saveToSD() {
    if (!sdcard::isMounted()) return;
    JsonDocument doc;
    JsonArray arr = doc["repeaters"].to<JsonArray>();
    for (int i = 0; i < s_count; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"]     = s_reps[i].name;
        char hex[9];
        snprintf(hex, sizeof(hex), "%02X%02X%02X%02X",
                 s_reps[i].pubKeyPrefix[0], s_reps[i].pubKeyPrefix[1],
                 s_reps[i].pubKeyPrefix[2], s_reps[i].pubKeyPrefix[3]);
        obj["key"]      = hex;
        obj["lastSeen"] = s_reps[i].lastSeen;
        obj["rssi"]     = s_reps[i].lastRssi;
        if (s_reps[i].favourite)
            obj["fav"]  = true;
        if (s_reps[i].lat != 0 || s_reps[i].lon != 0) {
            obj["lat"] = s_reps[i].lat;
            obj["lon"] = s_reps[i].lon;
        }
        char fullHex[65] = {};
        for (int b = 0; b < 32; b++)
            snprintf(fullHex + b * 2, 3, "%02X", s_reps[i].pubKey[b]);
        obj["pubKey64"] = fullHex;

        if (s_reps[i].outPathValid && s_reps[i].outPathLen != 0xFF) {
            obj["pathLen"] = s_reps[i].outPathLen;
            if (s_reps[i].outPathLen > 0) {
                char pathHex[129] = {};
                for (int b = 0; b < s_reps[i].outPathLen && b < 64; b++)
                    snprintf(pathHex + b * 2, 3, "%02X", s_reps[i].outPath[b]);
                obj["path"] = pathHex;
            }
        }
    }
    File f = SD.open("/ops/repeaters.json", FILE_WRITE);
    if (!f) { OPS_LOG("SD", "repeaters.json open failed"); return; }
    serializeJson(doc, f);
    f.close();
    OPS_LOG("Repeaters", "SD backup: %d repeaters", s_count);
}

static bool _loadFromSD() {
    if (!sdcard::isMounted()) return false;
    File f = SD.open("/ops/repeaters.json", FILE_READ);
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { OPS_LOG("SD", "repeaters.json parse: %s", err.c_str()); return false; }

    JsonArray arr = doc["repeaters"].as<JsonArray>();
    s_count = 0;
    for (JsonObject obj : arr) {
        if (s_count >= repeaters::CAPACITY) break;
        Repeater& r = s_reps[s_count];
        strncpy(r.name, obj["name"] | "", sizeof(r.name) - 1);
        r.name[sizeof(r.name) - 1] = '\0';

        const char* hex = obj["key"] | "";
        for (int b = 0; b < 4; b++) {
            char byteStr[3] = { hex[b * 2], hex[b * 2 + 1], '\0' };
            r.pubKeyPrefix[b] = (uint8_t)strtol(byteStr, nullptr, 16);
        }

        r.lastSeen  = obj["lastSeen"] | (uint32_t)0;
        r.lastRssi  = obj["rssi"]     | 0.0f;
        r.favourite = obj["fav"]      | false;
        r.lat      = obj["lat"]      | (int32_t)0;
        r.lon      = obj["lon"]      | (int32_t)0;
        const char* pk64 = obj["pubKey64"] | "";
        if (strlen(pk64) == 64) {
            for (int b = 0; b < 32; b++) {
                char hb[3] = { pk64[b * 2], pk64[b * 2 + 1], '\0' };
                r.pubKey[b] = (uint8_t)strtol(hb, nullptr, 16);
            }
        }
        r.outPathValid = false;
        r.outPathLen   = 0xFF;
        memset(r.outPath, 0, sizeof(r.outPath));
        if (obj["pathLen"].is<int>()) {
            uint8_t pl = (uint8_t)(int)obj["pathLen"];
            if (pl <= 64) {
                r.outPathLen = pl;
                if (pl > 0) {
                    const char* ph = obj["path"] | "";
                    size_t phLen = strlen(ph);
                    if (phLen == (size_t)pl * 2) {
                        for (int b = 0; b < pl; b++) {
                            char hb[3] = { ph[b * 2], ph[b * 2 + 1], '\0' };
                            r.outPath[b] = (uint8_t)strtol(hb, nullptr, 16);
                        }
                    }
                }
                r.outPathValid = true;
            }
        }
        s_count++;
    }
    OPS_LOG("Repeaters", "Loaded %d from SD", s_count);
    return s_count > 0;
}

// ── Public API ─────────────────────────────────────────────────────

int  repeaters::count() { return s_count; }

bool repeaters::get(int idx, Repeater& out)
{
    if (idx < 0 || idx >= s_count) return false;
    out = s_reps[idx];
    return true;
}

bool repeaters::findByKey(const uint8_t prefix[4], int* outIdx)
{
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_reps[i].pubKeyPrefix, prefix, 4) == 0) {
            if (outIdx) *outIdx = i;
            return true;
        }
    }
    return false;
}

void repeaters::init()
{
    // NVS is primary — try it first so a crash between NVS write and SD write
    // never causes the stale SD backup to overwrite more recent NVS data.
    Preferences prefs;
    if (prefs.begin("opsrep", /*readOnly=*/true)) {
        int n = prefs.getInt("n", 0);
        if (n > 0) {
            s_count = (n > CAPACITY) ? CAPACITY : n;
            for (int i = 0; i < s_count; i++) {
                char key[8];
                snprintf(key, sizeof(key), "r%d", i);
                size_t blobLen = prefs.getBytesLength(key);
                if (blobLen > 0) {
                    size_t toRead = blobLen < sizeof(Repeater) ? blobLen : sizeof(Repeater);
                    prefs.getBytes(key, &s_reps[i], toRead);
                }
            }
            prefs.end();
            _saveToSD();  // keep SD backup in sync
            OPS_LOG("Repeaters", "Loaded %d from NVS", s_count);
            return;
        }
        prefs.end();
    } else {
        prefs.end();
    }

    // NVS empty or unreadable — SD is the recovery path (post-reflash restore).
    if (sdcard::hasCompleteBackup() && _loadFromSD()) {
        save();
        OPS_LOG("Repeaters", "Restored %d from SD", s_count);
        return;
    }

    if (!_loadFromSD())
        OPS_LOG("Repeaters", "No saved repeaters");
}

int repeaters::reloadFromSD()
{
    if (!sdcard::isMounted()) return -1;
    static Repeater snap[CAPACITY];
    int snapN = s_count;
    if (snapN > 0) memcpy(snap, s_reps, (size_t)snapN * sizeof(Repeater));
    if (!_loadFromSD()) {
        s_count = snapN;
        if (snapN > 0) memcpy(s_reps, snap, (size_t)snapN * sizeof(Repeater));
        return -1;
    }
    save();
    return s_count;
}

void repeaters::clearPath(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    if (!s_reps[idx].outPathValid) return;  // already clear — skip NVS write
    s_reps[idx].outPathValid = false;
    s_reps[idx].outPathLen   = 0xFF;
    memset(s_reps[idx].outPath, 0, sizeof(s_reps[idx].outPath));
    save();
    OPS_LOG("Repeaters", "Path cleared: %s", s_reps[idx].name);
}

void repeaters::save()
{
    Preferences prefs;
    if (!prefs.begin("opsrep", false)) {
        OPS_LOG("Repeaters", "NVS write failed");
        return;
    }
    prefs.putInt("n", s_count);
    for (int i = 0; i < s_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "r%d", i);
        prefs.putBytes(key, &s_reps[i], sizeof(Repeater));
    }
    prefs.end();
    _saveToSD();
}

void repeaters::setPosition(int idx, int32_t lat, int32_t lon)
{
    if (idx < 0 || idx >= s_count) return;
    s_reps[idx].lat = lat;
    s_reps[idx].lon = lon;
}

void repeaters::add(const Repeater& r)
{
    int idx = -1;
    findByKey(r.pubKeyPrefix, &idx);
    if (idx >= 0) {
        s_reps[idx] = r;
        OPS_LOG("Repeaters", "Updated: %s", r.name);
    } else if (s_count < CAPACITY) {
        s_reps[s_count++] = r;
        OPS_LOG("Repeaters", "Added: %s (%d total)", r.name, s_count);
    } else {
        OPS_LOG("Repeaters", "Capacity full (%d), dropping %s", CAPACITY, r.name);
        return;
    }
    save();
}

void repeaters::setFavourite(int idx, bool fav)
{
    if (idx < 0 || idx >= s_count) return;
    if (s_reps[idx].favourite == fav) return;
    s_reps[idx].favourite = fav;
    save();
}

void repeaters::remove(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    for (int i = idx; i < s_count - 1; i++)
        s_reps[i] = s_reps[i + 1];
    s_count--;
    save();
}

void repeaters::setPath(int idx, uint8_t pathLen, const uint8_t* path)
{
    if (idx < 0 || idx >= s_count) return;
    if (pathLen == 0xFF) return;  // OUT_PATH_UNKNOWN — don't persist
    // Skip save if nothing changed
    if (s_reps[idx].outPathValid && s_reps[idx].outPathLen == pathLen &&
        (pathLen == 0 || memcmp(s_reps[idx].outPath, path, pathLen) == 0)) return;
    s_reps[idx].outPathValid = true;
    s_reps[idx].outPathLen   = pathLen;
    if (path && pathLen > 0 && pathLen <= 64)
        memcpy(s_reps[idx].outPath, path, pathLen);
    else
        memset(s_reps[idx].outPath, 0, 64);
    save();
    OPS_LOG("Repeaters", "Path saved: %s len=%d", s_reps[idx].name, pathLen);
}

}  // namespace ops
