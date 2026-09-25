// Saitama — Repeaters.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "Repeaters.h"
#include "Crypto.h"
#include "SDCard.h"
#include "Log.h"
#include <Preferences.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <cstring>

namespace ops {

static Repeater* s_reps     = nullptr;  // PSRAM; allocated in init()
static Repeater* s_repsSnap = nullptr;  // PSRAM rollback buffer for reloadFromSD()
static int       s_count = 0;

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

        // Sealed admin password. Ciphertext only — see utils/Crypto.h.
        if (s_reps[i].hasAdminPw) {
            char pwHex[crypto::BLOB_HEX_LEN] = {};
            crypto::toHex(s_reps[i].adminPwEnc, crypto::BLOB_LEN, pwHex, sizeof(pwHex));
            obj["pwEnc"] = pwHex;
        }

        if (s_reps[i].outPathValid && s_reps[i].outPathLen != 0xFF) {
            obj["pathLen"] = s_reps[i].outPathLen;   // MeshCore encoding, not a byte count
            int nb = outPathByteCount(s_reps[i].outPathLen);
            if (nb > 0) {
                char pathHex[129] = {};
                for (int b = 0; b < nb; b++)
                    snprintf(pathHex + b * 2, 3, "%02X", s_reps[i].outPath[b]);
                obj["path"] = pathHex;
            }
            if (s_reps[i].pathAt) obj["pathAt"] = s_reps[i].pathAt;
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
        r.hasAdminPw = false;
        memset(r.adminPwEnc, 0, sizeof(r.adminPwEnc));
        const char* pwHex = obj["pwEnc"] | "";
        if (pwHex[0])
            r.hasAdminPw = crypto::fromHex(pwHex, r.adminPwEnc, crypto::BLOB_LEN);

        r.outPathValid = false;
        r.outPathLen   = 0xFF;
        r.pathAt       = 0;
        memset(r.outPath, 0, sizeof(r.outPath));
        if (obj["pathLen"].is<int>()) {
            // pathLen is MeshCore's encoding ((hashSz-1)<<6 | hops). Files from
            // before this fix treated it as a byte count and saved 2-byte-hash
            // paths zeroed — the hex length check below rejects those.
            uint8_t pl = (uint8_t)(int)obj["pathLen"];
            int nb = outPathByteCount(pl);
            const char* ph = obj["path"] | "";
            if (pl != 0xFF && nb >= 0 && strlen(ph) == (size_t)nb * 2) {
                r.outPathLen = pl;
                for (int b = 0; b < nb; b++) {
                    char hb[3] = { ph[b * 2], ph[b * 2 + 1], '\0' };
                    r.outPath[b] = (uint8_t)strtol(hb, nullptr, 16);
                }
                r.outPathValid = true;
                r.pathAt = obj["pathAt"] | (uint32_t)0;
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

static void _clearLegacyNvs()
{
    Preferences prefs;
    if (!prefs.begin("opsrep", true)) { prefs.end(); return; }
    bool hasData = prefs.getInt("n", 0) > 0;
    prefs.end();
    if (hasData) {
        if (prefs.begin("opsrep", false)) { prefs.clear(); prefs.end(); }
        OPS_LOG("Repeaters", "Legacy NVS namespace cleared");
    }
}

void repeaters::init()
{
    if (!s_reps) {
        s_reps     = (Repeater*)ps_malloc((size_t)repeaters::CAPACITY * sizeof(Repeater));
        s_repsSnap = (Repeater*)ps_malloc((size_t)repeaters::CAPACITY * sizeof(Repeater));
        memset(s_reps, 0, (size_t)repeaters::CAPACITY * sizeof(Repeater));
    }
    // ── SD is always authoritative ────────────────────────────────────────
    // SD JSON is named-key and more reliable than NVS: previous firmware's
    // save() wrote to SD even when NVS putBytes was failing, so SD is always
    // at least as fresh as NVS.
    if (_loadFromSD()) {
        OPS_LOG("Repeaters", "Loaded %d from SD", s_count);
        _clearLegacyNvs();
        return;
    }

    // SD may not have been mounted at boot (improper unmount / card absent).
    // Attempt a single remount and retry before falling back to NVS.
    if (!sdcard::isMounted()) {
        OPS_LOG("Repeaters", "SD not mounted at init, attempting remount");
        if (sdcard::tryMount() && _loadFromSD()) {
            OPS_LOG("Repeaters", "Loaded %d from SD after remount", s_count);
            _clearLegacyNvs();
            return;
        }
    }

    // ── NVS fallback (SD absent or no file yet) ───────────────────────────
    // One-time migration: read legacy per-blob NVS data, write to SD, clear namespace.
    Preferences prefs;
    if (prefs.begin("opsrep", true)) {
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
            if (prefs.begin("opsrep", false)) { prefs.clear(); prefs.end(); }
            _saveToSD();
            OPS_LOG("Repeaters", "Migrated %d from NVS to SD; NVS cleared", s_count);
            return;
        }
        prefs.end();
    } else {
        prefs.end();
    }

    OPS_LOG("Repeaters", "No saved repeaters");
}

int repeaters::reloadFromSD()
{
    if (!sdcard::isMounted()) return -1;
    int snapN = s_count;
    if (snapN > 0) memcpy(s_repsSnap, s_reps, (size_t)snapN * sizeof(Repeater));
    if (!_loadFromSD()) {
        s_count = snapN;
        if (snapN > 0) memcpy(s_reps, s_repsSnap, (size_t)snapN * sizeof(Repeater));
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
    s_reps[idx].pathAt       = 0;
    memset(s_reps[idx].outPath, 0, sizeof(s_reps[idx].outPath));
    save();
    OPS_LOG("Repeaters", "Path cleared: %s", s_reps[idx].name);
}

void repeaters::save()
{
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
        // A live advert arrives as a freshly built struct, so anything the
        // user set locally has to be carried across by hand or the next
        // advert silently wipes it.
        bool    fav   = s_reps[idx].favourite;
        bool    hasPw = s_reps[idx].hasAdminPw;
        uint8_t pwEnc[crypto::BLOB_LEN];
        memcpy(pwEnc, s_reps[idx].adminPwEnc, sizeof(pwEnc));
        s_reps[idx] = r;
        s_reps[idx].favourite  = fav;
        s_reps[idx].hasAdminPw = hasPw;
        memcpy(s_reps[idx].adminPwEnc, pwEnc, sizeof(pwEnc));
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

void repeaters::setLiveData(int idx, const char* name, uint32_t lastSeen, float lastRssi)
{
    if (idx < 0 || idx >= s_count) return;
    if (name && name[0]) {
        strncpy(s_reps[idx].name, name, sizeof(s_reps[idx].name) - 1);
        s_reps[idx].name[sizeof(s_reps[idx].name) - 1] = '\0';
    }
    s_reps[idx].lastSeen = lastSeen;
    s_reps[idx].lastRssi = lastRssi;
}

void repeaters::setFullKey(int idx, const uint8_t* pubKey32)
{
    if (idx < 0 || idx >= s_count || !pubKey32) return;
    bool alreadySet = false;
    for (int b = 0; b < 32; b++) if (s_reps[idx].pubKey[b]) { alreadySet = true; break; }
    if (alreadySet) return;
    memcpy(s_reps[idx].pubKey, pubKey32, 32);
    save();
    OPS_LOG("Repeaters", "Full key set: %s", s_reps[idx].name);
}

void repeaters::setPath(int idx, uint8_t pathLen, const uint8_t* path, uint32_t learnedAt)
{
    if (idx < 0 || idx >= s_count) return;
    if (pathLen == 0xFF) return;  // OUT_PATH_UNKNOWN — don't persist
    int nb = outPathByteCount(pathLen);   // pathLen is encoded, not a byte count
    if (nb < 0 || (nb > 0 && !path)) return;
    bool same = s_reps[idx].outPathValid && s_reps[idx].outPathLen == pathLen &&
                (nb == 0 || memcmp(s_reps[idx].outPath, path, nb) == 0);
    // Re-confirming the same path only refreshes its age in RAM; the file is
    // rewritten on the next real change. A hand-set path is never downgraded.
    if (same) {
        if (s_reps[idx].pathAt != PATH_AT_PINNED || learnedAt == PATH_AT_PINNED)
            s_reps[idx].pathAt = learnedAt;
        return;
    }
    s_reps[idx].outPathValid = true;
    s_reps[idx].outPathLen   = pathLen;
    s_reps[idx].pathAt       = learnedAt;
    memset(s_reps[idx].outPath, 0, 64);
    if (nb > 0) memcpy(s_reps[idx].outPath, path, nb);
    save();
    OPS_LOG("Repeaters", "Path saved: %s len=%d", s_reps[idx].name, pathLen);
}

// ── Remembered admin passwords ─────────────────────────────────────

bool repeaters::hasAdminPassword(int idx)
{
    if (idx < 0 || idx >= s_count) return false;
    return s_reps[idx].hasAdminPw;
}

bool repeaters::setAdminPassword(int idx, const char* plain)
{
    if (idx < 0 || idx >= s_count) return false;

    if (!plain || !plain[0]) {
        if (!s_reps[idx].hasAdminPw) return true;   // already forgotten
        s_reps[idx].hasAdminPw = false;
        memset(s_reps[idx].adminPwEnc, 0, sizeof(s_reps[idx].adminPwEnc));
        save();
        OPS_LOG("Repeaters", "Admin password forgotten: %s", s_reps[idx].name);
        return true;
    }

    uint8_t blob[crypto::BLOB_LEN];
    if (!crypto::seal(plain, s_reps[idx].pubKeyPrefix, 4, blob)) {
        OPS_LOG("Repeaters", "Seal failed for %s", s_reps[idx].name);
        return false;
    }
    memcpy(s_reps[idx].adminPwEnc, blob, sizeof(blob));
    s_reps[idx].hasAdminPw = true;
    save();
    OPS_LOG("Repeaters", "Admin password stored: %s", s_reps[idx].name);
    return true;
}

bool repeaters::getAdminPassword(int idx, char* out, size_t outMax)
{
    if (!out || outMax == 0) return false;
    out[0] = '\0';
    if (idx < 0 || idx >= s_count || !s_reps[idx].hasAdminPw) return false;
    return crypto::unseal(s_reps[idx].adminPwEnc, s_reps[idx].pubKeyPrefix, 4,
                          out, outMax);
}

int repeaters::rekeyAdminPasswords(int* outLost)
{
    int done = 0, lost = 0;
    for (int i = 0; i < s_count; i++) {
        if (!s_reps[i].hasAdminPw) continue;
        char plain[crypto::SECRET_MAX] = {};
        // unseal() still uses the old key here; seal() already uses the new
        // one (see crypto::beginRekey), so one pass re-encrypts the record.
        if (!crypto::unseal(s_reps[i].adminPwEnc, s_reps[i].pubKeyPrefix, 4,
                            plain, sizeof(plain)) ||
            !crypto::seal(plain, s_reps[i].pubKeyPrefix, 4, s_reps[i].adminPwEnc)) {
            s_reps[i].hasAdminPw = false;
            memset(s_reps[i].adminPwEnc, 0, sizeof(s_reps[i].adminPwEnc));
            lost++;
            OPS_LOG("Repeaters", "Re-key lost password for %s", s_reps[i].name);
        } else {
            done++;
        }
        memset(plain, 0, sizeof(plain));
    }
    if (done || lost) save();
    if (outLost) *outLost = lost;
    return done;
}

}  // namespace ops
