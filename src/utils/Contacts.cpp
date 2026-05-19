// Saitama — Contacts.cpp
// Copyright 2026 Saitama — MIT License

#include "Contacts.h"
#include "SDCard.h"
#include "Log.h"
#include <Preferences.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <cstring>

namespace ops {

static Contact s_contacts[contacts::CAPACITY];
static int     s_count = 0;

// ── SD JSON helpers ────────────────────────────────────────────────

static void _saveToSD() {
    if (!sdcard::isMounted()) return;
    JsonDocument doc;
    JsonArray arr = doc["contacts"].to<JsonArray>();
    for (int i = 0; i < s_count; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = s_contacts[i].name;
        char hex[9];
        snprintf(hex, sizeof(hex), "%02X%02X%02X%02X",
                 s_contacts[i].pubKeyPrefix[0], s_contacts[i].pubKeyPrefix[1],
                 s_contacts[i].pubKeyPrefix[2], s_contacts[i].pubKeyPrefix[3]);
        obj["key"]      = hex;
        obj["lastSeen"] = s_contacts[i].lastSeen;
        obj["rssi"]     = s_contacts[i].lastRssi;
        obj["unread"]   = s_contacts[i].hasUnread;
        if (s_contacts[i].favourite)
            obj["fav"]  = true;
        if (s_contacts[i].lat != 0 || s_contacts[i].lon != 0) {
            obj["lat"] = s_contacts[i].lat;
            obj["lon"] = s_contacts[i].lon;
        }
        // Full 32-byte public key — needed for mesh routing after SD restore
        bool hasKey = false;
        for (int b = 0; b < 32; b++) if (s_contacts[i].pubKey[b]) { hasKey = true; break; }
        if (hasKey) {
            char fk[65] = {};
            for (int b = 0; b < 32; b++)
                snprintf(fk + b*2, 3, "%02X", s_contacts[i].pubKey[b]);
            obj["fk"] = fk;
        }
        if (s_contacts[i].outPathValid && s_contacts[i].outPathLen != 0xFF) {
            obj["pathLen"] = s_contacts[i].outPathLen;
            if (s_contacts[i].outPathLen > 0) {
                char pathHex[129] = {};
                for (int b = 0; b < s_contacts[i].outPathLen && b < 64; b++)
                    snprintf(pathHex + b * 2, 3, "%02X", s_contacts[i].outPath[b]);
                obj["path"] = pathHex;
            }
        }
    }
    File f = SD.open("/ops/contacts.json", FILE_WRITE);
    if (!f) { OPS_LOG("SD", "contacts.json open failed"); return; }
    serializeJson(doc, f);
    f.close();
    OPS_LOG("Contacts", "SD backup: %d contacts", s_count);
}

static bool _loadFromSD() {
    if (!sdcard::isMounted()) return false;
    File f = SD.open("/ops/contacts.json", FILE_READ);
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { OPS_LOG("SD", "contacts.json parse: %s", err.c_str()); return false; }

    JsonArray arr = doc["contacts"].as<JsonArray>();
    s_count = 0;
    for (JsonObject obj : arr) {
        if (s_count >= contacts::CAPACITY) break;
        Contact& c = s_contacts[s_count];
        memset(&c, 0, sizeof(Contact));
        strncpy(c.name, obj["name"] | "", sizeof(c.name) - 1);

        const char* hex = obj["key"] | "";
        for (int b = 0; b < 4; b++) {
            char byte_str[3] = {hex[b * 2], hex[b * 2 + 1], '\0'};
            c.pubKeyPrefix[b] = (uint8_t)strtol(byte_str, nullptr, 16);
        }

        // Restore full 32-byte public key when available
        const char* fk = obj["fk"] | "";
        if (strlen(fk) == 64) {
            for (int b = 0; b < 32; b++) {
                char hs[3] = {fk[b*2], fk[b*2+1], '\0'};
                c.pubKey[b] = (uint8_t)strtol(hs, nullptr, 16);
            }
        }

        c.lastSeen  = obj["lastSeen"]  | (uint32_t)0;
        c.lastRssi  = obj["rssi"]      | 0.0f;
        c.hasUnread = obj["unread"]    | false;
        c.favourite = obj["fav"]       | false;
        c.lat       = obj["lat"]       | (int32_t)0;
        c.lon       = obj["lon"]       | (int32_t)0;
        c.outPathValid = false;
        c.outPathLen   = 0xFF;
        memset(c.outPath, 0, sizeof(c.outPath));
        if (obj["pathLen"].is<int>()) {
            uint8_t pl = (uint8_t)(int)obj["pathLen"];
            if (pl <= 64) {
                c.outPathLen = pl;
                if (pl > 0) {
                    const char* ph = obj["path"] | "";
                    size_t phLen = strlen(ph);
                    if (phLen == (size_t)pl * 2) {
                        for (int b = 0; b < pl; b++) {
                            char hb[3] = { ph[b * 2], ph[b * 2 + 1], '\0' };
                            c.outPath[b] = (uint8_t)strtol(hb, nullptr, 16);
                        }
                    }
                }
                c.outPathValid = true;
            }
        }
        s_count++;
    }
    OPS_LOG("Contacts", "Loaded %d from SD", s_count);
    return true;  // parse succeeded — caller decides what to do with s_count
}

// ── Public API ─────────────────────────────────────────────────────

int  contacts::count() { return s_count; }

bool contacts::anyUnread()
{
    for (int i = 0; i < s_count; i++)
        if (s_contacts[i].hasUnread) return true;
    return false;
}

bool contacts::get(int idx, Contact& out)
{
    if (idx < 0 || idx >= s_count) return false;
    out = s_contacts[idx];
    return true;
}

bool contacts::findByKey(const uint8_t prefix[4], int* outIdx)
{
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_contacts[i].pubKeyPrefix, prefix, 4) == 0) {
            if (outIdx) *outIdx = i;
            return true;
        }
    }
    return false;
}

void contacts::add(const Contact& c)
{
    int idx;
    if (findByKey(c.pubKeyPrefix, &idx)) {
        s_contacts[idx] = c;
    } else if (s_count < contacts::CAPACITY) {
        s_contacts[s_count++] = c;
    } else {
        OPS_LOG("Contacts", "Full (%d)", contacts::CAPACITY);
        return;
    }
    save();
}

void contacts::setUnread(int idx, bool unread)
{
    if (idx < 0 || idx >= s_count) return;
    if (s_contacts[idx].hasUnread == unread) return;
    s_contacts[idx].hasUnread = unread;
    save();
}

void contacts::setFavourite(int idx, bool fav)
{
    if (idx < 0 || idx >= s_count) return;
    if (s_contacts[idx].favourite == fav) return;
    s_contacts[idx].favourite = fav;
    save();
}

void contacts::remove(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    for (int i = idx; i < s_count - 1; i++)
        s_contacts[i] = s_contacts[i + 1];
    s_count--;
    save();
}

void contacts::init()
{
    // SD complete backup set is authoritative — overwrite NVS on restore
    if (sdcard::hasCompleteBackup() && _loadFromSD()) {
        save();
        OPS_LOG("Contacts", "Restored %d from SD", s_count);
        return;
    }

    // v0: name[32]+prefix[4]+lastSeen[4]+rssi[4]+unread[1]+pad[3] = 48
    struct LegacyContact {
        char     name[32];
        uint8_t  pubKeyPrefix[4];
        uint32_t lastSeen;
        float    lastRssi;
        bool     hasUnread;
        uint8_t  _pad[3];
    };
    static_assert(sizeof(LegacyContact) == 48, "legacy size assumption wrong");

    // v1: added pubKey[32], still no lat/lon = 80
    struct PreLatLonContact {
        char     name[32];
        uint8_t  pubKeyPrefix[4];
        uint8_t  pubKey[32];
        uint32_t lastSeen;
        float    lastRssi;
        bool     hasUnread;
        uint8_t  _pad[3];
    };
    static_assert(sizeof(PreLatLonContact) == 80, "pre-lat/lon size assumption wrong");
    // v2 layout (88 bytes) — before path fields
    struct PrePathContact {
        char     name[32];
        uint8_t  pubKeyPrefix[4];
        uint8_t  pubKey[32];
        uint32_t lastSeen;
        float    lastRssi;
        bool     hasUnread;
        uint8_t  _pad[3];
        int32_t  lat;
        int32_t  lon;
    };
    static_assert(sizeof(PrePathContact) == 88, "v2 contact size wrong — check migration");
    static_assert(sizeof(Contact) == 156, "Contact size changed — add a new migration case");

    Preferences prefs;
    if (!prefs.begin("opsct", true)) {
        prefs.end();
        // NVS empty — try SD archive (covers reflash scenario)
        if (!_loadFromSD())
            OPS_LOG("Contacts", "No saved contacts");
        return;
    }
    s_count = prefs.getInt("n", 0);
    if (s_count > contacts::CAPACITY) s_count = contacts::CAPACITY;
    bool needResave = false;
    for (int i = 0; i < s_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "c%d", i);
        size_t blobSz = prefs.getBytesLength(key);
        if (blobSz == sizeof(Contact)) {
            prefs.getBytes(key, &s_contacts[i], sizeof(Contact));
        } else if (blobSz == sizeof(PrePathContact)) {
            // v2 → v3: added outPathValid/outPathLen/outPath
            PrePathContact old{};
            prefs.getBytes(key, &old, sizeof(PrePathContact));
            memset(&s_contacts[i], 0, sizeof(Contact));
            memcpy(s_contacts[i].name,         old.name,         32);
            memcpy(s_contacts[i].pubKeyPrefix,  old.pubKeyPrefix, 4);
            memcpy(s_contacts[i].pubKey,        old.pubKey,       32);
            s_contacts[i].lastSeen  = old.lastSeen;
            s_contacts[i].lastRssi  = old.lastRssi;
            s_contacts[i].hasUnread = old.hasUnread;
            s_contacts[i].lat       = old.lat;
            s_contacts[i].lon       = old.lon;
            s_contacts[i].outPathLen = 0xFF;  // path unknown
            needResave = true;
        } else if (blobSz == sizeof(PreLatLonContact)) {
            // v1 → v2: copy all fields, lat/lon default to 0
            PreLatLonContact old{};
            prefs.getBytes(key, &old, sizeof(PreLatLonContact));
            memset(&s_contacts[i], 0, sizeof(Contact));
            memcpy(s_contacts[i].name,        old.name,        32);
            memcpy(s_contacts[i].pubKeyPrefix, old.pubKeyPrefix, 4);
            memcpy(s_contacts[i].pubKey,       old.pubKey,      32);
            s_contacts[i].lastSeen  = old.lastSeen;
            s_contacts[i].lastRssi  = old.lastRssi;
            s_contacts[i].hasUnread = old.hasUnread;
            needResave = true;
        } else if (blobSz == sizeof(LegacyContact)) {
            // v0 → v2: no pubKey, no lat/lon
            LegacyContact old{};
            prefs.getBytes(key, &old, sizeof(LegacyContact));
            memset(&s_contacts[i], 0, sizeof(Contact));
            memcpy(s_contacts[i].name,         old.name,         32);
            memcpy(s_contacts[i].pubKeyPrefix,  old.pubKeyPrefix, 4);
            s_contacts[i].lastSeen  = old.lastSeen;
            s_contacts[i].lastRssi  = old.lastRssi;
            s_contacts[i].hasUnread = old.hasUnread;
            needResave = true;
        }
    }
    prefs.end();
    // NVS opened but had 0 contacts — prefer SD if available
    // (covers: reflash preserved NVS namespace but cleared its data)
    if (s_count == 0 && sdcard::isMounted()) {
        _loadFromSD();
        if (s_count > 0) {
            save();
            OPS_LOG("Contacts", "Restored %d from SD (NVS was empty)", s_count);
            return;
        }
    }
    if (needResave) {
        OPS_LOG("Contacts", "Migrating %d contacts to new format", s_count);
        save();  // writes NVS + SD
    } else {
        _saveToSD();  // ensure SD backup exists even when NVS was the source
    }
    OPS_LOG("Contacts", "Loaded %d from NVS", s_count);
}

int contacts::reloadFromSD()
{
    if (!sdcard::isMounted()) return -1;
    static Contact snap[CAPACITY];
    int snapN = s_count;
    if (snapN > 0) memcpy(snap, s_contacts, (size_t)snapN * sizeof(Contact));
    if (!_loadFromSD()) {
        s_count = snapN;
        if (snapN > 0) memcpy(s_contacts, snap, (size_t)snapN * sizeof(Contact));
        return -1;
    }
    save();
    return s_count;
}

void contacts::setPosition(int idx, int32_t lat, int32_t lon)
{
    if (idx < 0 || idx >= s_count) return;
    s_contacts[idx].lat = lat;
    s_contacts[idx].lon = lon;
}

void contacts::setPath(int idx, uint8_t pathLen, const uint8_t* path)
{
    if (idx < 0 || idx >= s_count) return;
    if (pathLen == 0xFF) return;  // OUT_PATH_UNKNOWN — don't persist
    // Skip save if nothing changed
    if (s_contacts[idx].outPathValid && s_contacts[idx].outPathLen == pathLen &&
        (pathLen == 0 || memcmp(s_contacts[idx].outPath, path, pathLen) == 0)) return;
    s_contacts[idx].outPathValid = true;
    s_contacts[idx].outPathLen   = pathLen;
    if (path && pathLen > 0 && pathLen <= 64)
        memcpy(s_contacts[idx].outPath, path, pathLen);
    else
        memset(s_contacts[idx].outPath, 0, 64);
    save();
    OPS_LOG("Contacts", "Path saved: %s len=%d", s_contacts[idx].name, pathLen);
}

void contacts::clearPath(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    if (!s_contacts[idx].outPathValid) return;  // already clear — skip NVS write
    s_contacts[idx].outPathValid = false;
    s_contacts[idx].outPathLen   = 0xFF;
    memset(s_contacts[idx].outPath, 0, sizeof(s_contacts[idx].outPath));
    save();
    OPS_LOG("Contacts", "Path cleared: %s", s_contacts[idx].name);
}

void contacts::save()
{
    Preferences prefs;
    if (!prefs.begin("opsct", false)) {
        OPS_LOG("Contacts", "NVS write failed");
        return;
    }
    prefs.putInt("n", s_count);
    for (int i = 0; i < s_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "c%d", i);
        prefs.putBytes(key, &s_contacts[i], sizeof(Contact));
    }
    prefs.end();
    _saveToSD();
}

}  // namespace ops
