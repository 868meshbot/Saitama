// Saitama — Config: persistent settings via NVS (Preferences)
// Copyright 2026 Saitama — MIT License
//
// Uses Arduino Preferences (NVS) so config survives reboots without
// touching the LittleFS/spiffs partition that MeshCore owns.

#include "Config.h"
#include "SDCard.h"
#include "Log.h"
#include <Preferences.h>
#include <SD.h>
#include <ArduinoJson.h>

namespace ops {

static Config      s_cfg;
static Preferences prefs;

static void setDefaults(Config& c) {
    strncpy(c.callsign,    "OPS-0001", sizeof(c.callsign));
    strncpy(c.radioRegion, "EU868",    sizeof(c.radioRegion));
    // All channel slots start empty — user configures name, shortname, PSK explicitly
    strncpy(c.channels[0].name, "Public", sizeof(c.channels[0].name));
    for (int i = 0; i < 10; i++) {
        if (i > 0) c.channels[i].name[0] = '\0';
        c.channels[i].shortname[0] = '\0';
        c.channels[i].psk[0]       = '\0';
        c.channels[i].notify       = false;
        c.channels[i].scope[0]     = '\0';
    }
    c.activeChannel    = 0;
    c.bluetoothEnabled  = false;
    c.speakerEnabled    = true;
    c.gpsMode           = 2;  // on by default
    c.kbBrightness    = 128;
    c.kbLayout        = 0;
    c.autoAddClient     = true;
    c.autoAddRepeater   = false;
    c.saveMsgs          = true;
    c.showHops          = true;
    c.showRssi          = false;
    c.locationSharing   = false;
    c.notifyPopup       = true;
    c.brightness       = 200;
    c.screenTimeoutSec = 30;
    c.screenOffMin     = 0;
    c.notifySound      = true;
    c.notifySoundChoice = 0;
    strncpy(c.mapTileDir, "/map", sizeof(c.mapTileDir));
    c.theme            = 0;
    c.radioProfile     = 0;
    c.showAdverts      = false;
    c.radioCustom      = false;
    c.freqMHz          = 0.0f;
    c.radioSF          = 0;
    c.radioBW          = 0;
    c.radioCR          = 0;
    c.radioTX          = 0;
    c.manualLat        = 0.0f;
    c.manualLon        = 0.0f;
    c.autoForward      = true;
    c.pathHashSz       = 0;
    c.timezoneOffsetHours = 0;
    c.scopeTag[0]      = '\0';
    c.loraDutyCycle    = false;
    c.rxBoost          = false;
    c.cpuGovernor      = 2;  // Normal — scales down during screensaver/screen-off
    c.touchCalXScale   = 1.0f;
    c.touchCalXOff     = 0.0f;
    c.touchCalYScale   = 1.0f;
    c.touchCalYOff     = 0.0f;
}

// ── SD JSON helpers ────────────────────────────────────────────────

static void _saveToSD() {
    if (!sdcard::isMounted()) return;
    JsonDocument doc;
    doc["callsign"]     = s_cfg.callsign;
    doc["region"]       = s_cfg.radioRegion;
    doc["mapTileDir"]   = s_cfg.mapTileDir;
    doc["channel"]      = s_cfg.activeChannel;
    doc["bluetooth"]    = s_cfg.bluetoothEnabled;
    doc["speaker"]      = s_cfg.speakerEnabled;
    doc["gpsMode"]      = s_cfg.gpsMode;
    doc["kbBright"]   = s_cfg.kbBrightness;
    doc["kbLayout"]   = s_cfg.kbLayout;
    doc["aaclient"]     = s_cfg.autoAddClient;
    doc["aarepeater"]   = s_cfg.autoAddRepeater;
    doc["saveMsgs"]     = s_cfg.saveMsgs;
    doc["showHops"]     = s_cfg.showHops;
    doc["showRssi"]     = s_cfg.showRssi;
    doc["locShare"]     = s_cfg.locationSharing;
    doc["notifyPopup"]  = s_cfg.notifyPopup;
    doc["brightness"]   = s_cfg.brightness;
    doc["screenTimeout"]= s_cfg.screenTimeoutSec;
    doc["screenOff"]    = s_cfg.screenOffMin;
    doc["notifySound"]  = s_cfg.notifySound;
    doc["notifySndCh"]  = s_cfg.notifySoundChoice;
    doc["theme"]        = s_cfg.theme;
    doc["radioProf"]    = s_cfg.radioProfile;
    doc["showAdverts"]  = s_cfg.showAdverts;
    doc["radioCustom"]  = s_cfg.radioCustom;
    doc["freqMHz"]      = s_cfg.freqMHz;
    doc["radioSF"]      = s_cfg.radioSF;
    doc["radioBW"]      = s_cfg.radioBW;
    doc["radioCR"]      = s_cfg.radioCR;
    doc["radioTX"]      = s_cfg.radioTX;
    doc["manualLat"]    = s_cfg.manualLat;
    doc["manualLon"]    = s_cfg.manualLon;
    doc["autoFwd"]      = s_cfg.autoForward;
    doc["pathHashSz"]   = s_cfg.pathHashSz;
    doc["tzOff"]        = s_cfg.timezoneOffsetHours;
    doc["scopeTag"]     = s_cfg.scopeTag;
    doc["loraDC"]       = s_cfg.loraDutyCycle;
    doc["rxBoost"]      = s_cfg.rxBoost;
    doc["cpuGov"]       = s_cfg.cpuGovernor;
    doc["tcXScale"]     = s_cfg.touchCalXScale;
    doc["tcXOff"]       = s_cfg.touchCalXOff;
    doc["tcYScale"]     = s_cfg.touchCalYScale;
    doc["tcYOff"]       = s_cfg.touchCalYOff;
    JsonArray chArr = doc["channels"].to<JsonArray>();
    for (int i = 0; i < 10; i++) {
        JsonObject ch = chArr.add<JsonObject>();
        ch["name"]   = s_cfg.channels[i].name;
        ch["sn"]     = s_cfg.channels[i].shortname;
        ch["psk"]    = s_cfg.channels[i].psk;
        ch["notify"] = s_cfg.channels[i].notify;
        ch["scope"]  = s_cfg.channels[i].scope;
    }
    File f = SD.open("/ops/settings.json", FILE_WRITE);
    if (!f) return;
    serializeJson(doc, f);
    f.close();
}

static bool _loadFromSD() {
    if (!sdcard::isMounted()) return false;
    File f = SD.open("/ops/settings.json", FILE_READ);
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { OPS_LOG("Config", "settings.json parse: %s", err.c_str()); return false; }

    strncpy(s_cfg.callsign,    doc["callsign"]      | s_cfg.callsign,    sizeof(s_cfg.callsign) - 1);
    strncpy(s_cfg.radioRegion, doc["region"]        | s_cfg.radioRegion, sizeof(s_cfg.radioRegion) - 1);
    strncpy(s_cfg.mapTileDir,  doc["mapTileDir"]    | s_cfg.mapTileDir,  sizeof(s_cfg.mapTileDir) - 1);
    s_cfg.activeChannel    = doc["channel"]      | s_cfg.activeChannel;
    s_cfg.bluetoothEnabled  = doc["bluetooth"]    | s_cfg.bluetoothEnabled;
    s_cfg.speakerEnabled    = doc["speaker"]      | s_cfg.speakerEnabled;
    // "gpsMode" is the current key; fall back to legacy "gps" bool (true→2, false→0).
    if (doc["gpsMode"].is<int>())
        s_cfg.gpsMode = (uint8_t)(int)doc["gpsMode"];
    else
        s_cfg.gpsMode = doc["gps"].is<bool>() ? (doc["gps"] ? 2 : 0) : s_cfg.gpsMode;
    s_cfg.kbBrightness    = (uint8_t)(doc["kbBright"] | 128);
    s_cfg.kbLayout        = (uint8_t)(doc["kbLayout"] | 0);
    s_cfg.autoAddClient     = doc["aaclient"]     | s_cfg.autoAddClient;
    s_cfg.autoAddRepeater   = doc["aarepeater"]   | s_cfg.autoAddRepeater;
    s_cfg.saveMsgs          = doc["saveMsgs"]     | s_cfg.saveMsgs;
    s_cfg.showHops          = doc["showHops"]     | s_cfg.showHops;
    s_cfg.showRssi          = doc["showRssi"]     | s_cfg.showRssi;
    s_cfg.locationSharing   = doc["locShare"]     | s_cfg.locationSharing;
    s_cfg.notifyPopup       = doc["notifyPopup"]  | s_cfg.notifyPopup;
    s_cfg.brightness        = doc["brightness"]   | s_cfg.brightness;
    s_cfg.screenTimeoutSec  = doc["screenTimeout"]| s_cfg.screenTimeoutSec;
    s_cfg.screenOffMin      = (uint8_t)(doc["screenOff"] | 0);
    s_cfg.notifySound        = doc["notifySound"]   | s_cfg.notifySound;
    s_cfg.notifySoundChoice  = (uint8_t)(doc["notifySndCh"] | 0);
    s_cfg.theme             = doc["theme"]        | s_cfg.theme;
    s_cfg.radioProfile      = doc["radioProf"]    | s_cfg.radioProfile;
    s_cfg.showAdverts       = doc["showAdverts"]  | s_cfg.showAdverts;
    s_cfg.radioCustom       = doc["radioCustom"]  | false;
    s_cfg.freqMHz           = doc["freqMHz"]      | 0.0f;
    s_cfg.radioSF           = doc["radioSF"]      | (uint8_t)0;
    s_cfg.radioBW           = doc["radioBW"]      | (uint8_t)0;
    s_cfg.radioCR           = doc["radioCR"]      | (uint8_t)0;
    s_cfg.radioTX           = doc["radioTX"]      | (int8_t)0;
    s_cfg.manualLat         = doc["manualLat"]    | 0.0f;
    s_cfg.manualLon         = doc["manualLon"]    | 0.0f;
    s_cfg.autoForward       = doc["autoFwd"]      | true;
    s_cfg.pathHashSz        = doc["pathHashSz"]   | (uint8_t)0;
    s_cfg.timezoneOffsetHours = (int8_t)(doc["tzOff"] | 0);
    strncpy(s_cfg.scopeTag, doc["scopeTag"] | "", sizeof(s_cfg.scopeTag) - 1);
    s_cfg.scopeTag[sizeof(s_cfg.scopeTag) - 1] = '\0';
    s_cfg.loraDutyCycle = doc["loraDC"]   | false;
    s_cfg.rxBoost       = doc["rxBoost"]  | false;
    s_cfg.cpuGovernor   = (uint8_t)(doc["cpuGov"] | 2);
    s_cfg.touchCalXScale = doc["tcXScale"] | 1.0f;
    s_cfg.touchCalXOff   = doc["tcXOff"]   | 0.0f;
    s_cfg.touchCalYScale = doc["tcYScale"] | 1.0f;
    s_cfg.touchCalYOff   = doc["tcYOff"]   | 0.0f;
    if (doc["channels"].is<JsonArray>()) {
        JsonArray chArr = doc["channels"].as<JsonArray>();
        int i = 0;
        for (JsonObject ch : chArr) {
            if (i >= 10) break;
            const char* n  = ch["name"] | s_cfg.channels[i].name;
            const char* sn = ch["sn"]   | s_cfg.channels[i].shortname;
            const char* pk = ch["psk"]  | s_cfg.channels[i].psk;
            const char* sc = ch["scope"] | "";
            strncpy(s_cfg.channels[i].name,      n,  sizeof(s_cfg.channels[i].name)      - 1);
            strncpy(s_cfg.channels[i].shortname, sn, sizeof(s_cfg.channels[i].shortname) - 1);
            strncpy(s_cfg.channels[i].psk,       pk, sizeof(s_cfg.channels[i].psk)       - 1);
            strncpy(s_cfg.channels[i].scope,     sc, sizeof(s_cfg.channels[i].scope)     - 1);
            s_cfg.channels[i].name[sizeof(s_cfg.channels[i].name)           - 1] = '\0';
            s_cfg.channels[i].shortname[sizeof(s_cfg.channels[i].shortname) - 1] = '\0';
            s_cfg.channels[i].psk[sizeof(s_cfg.channels[i].psk)             - 1] = '\0';
            s_cfg.channels[i].scope[sizeof(s_cfg.channels[i].scope)         - 1] = '\0';
            s_cfg.channels[i].notify = ch["notify"] | false;
            i++;
        }
    }
    OPS_LOG("Config", "Loaded from SD: callsign=%s", s_cfg.callsign);
    return true;
}

// ── Public API ─────────────────────────────────────────────────────

const Config& config::get() { return s_cfg; }

void config::init() {
    setDefaults(s_cfg);

    // ── NVS primary (blob format) ───────────────────────────────────────
    // Config is stored as a single putBytes("cfg") entry so every save()
    // touches exactly 1 NVS entry instead of 80+, preventing NOT_ENOUGH_SPACE
    // on the 16 KB NVS partition.
    if (prefs.begin("ops", /*readOnly=*/true)) {
        size_t loaded = prefs.getBytes("cfg", &s_cfg, sizeof(s_cfg));
        prefs.end();
        // Accept partial blobs from older firmware (struct grew after a firmware update).
        // getBytes() copies only min(stored, requested) bytes; fields beyond the stored
        // size are untouched and keep their setDefaults() values, so new fields default
        // gracefully. 64-byte floor rejects clearly corrupt / empty entries.
        if (loaded >= 64) {
            bool migrated = false;
            for (int i = 1; i < 10; i++) {
                char* n  = s_cfg.channels[i].name;
                char* sn = s_cfg.channels[i].shortname;
                if (n[0]=='C' && n[1]=='H' && n[2]>='2' && n[2]<='5' && n[3]=='\0') {
                    n[0] = '\0'; sn[0] = '\0'; migrated = true;
                }
            }
            if (loaded < sizeof(s_cfg)) {
                // Partial blob — firmware struct grew since this blob was written.
                // Resave immediately so next boot loads a full-size blob.
                OPS_LOG("Config", "NVS partial blob (%u/%u bytes) — new fields defaulted; upgrading",
                        (unsigned)loaded, (unsigned)sizeof(s_cfg));
                config::save();
            } else {
                if (migrated) { OPS_LOG("Config", "Migrated legacy CH<n>"); config::save(); }
                OPS_LOG("Config", "Loaded from NVS: callsign=%s region=%s",
                        s_cfg.callsign, s_cfg.radioRegion);
            }
            return;
        }
        // Blob absent or too small to be a valid Config — fall through to SD / legacy keys.
        OPS_LOG("Config", "NVS blob absent or invalid (%u bytes) — migrating",
                (unsigned)loaded);
    } else {
        prefs.end();
    }

    // ── SD backup (migration / post-reflash) ────────────────────────────
    // SD JSON survives reflash and is the most likely source of current data.
    if (sdcard::hasCompleteBackup() && _loadFromSD()) {
        OPS_LOG("Config", "Restored from SD backup: callsign=%s", s_cfg.callsign);
        // Clear old per-key entries and write compact blob.
        if (prefs.begin("ops", false)) { prefs.clear(); prefs.end(); }
        save();
        return;
    }

    // ── Legacy individual NVS keys (migration from pre-blob firmware) ───
    // Only reached if SD is absent and the blob is missing/wrong-size.
    if (prefs.begin("ops", /*readOnly=*/true)) {
        String region = prefs.getString("radioRegion", s_cfg.radioRegion);
        strncpy(s_cfg.radioRegion, region.c_str(), sizeof(s_cfg.radioRegion) - 1);
        String cs = prefs.getString("callsign", s_cfg.callsign);
        strncpy(s_cfg.callsign, cs.c_str(), sizeof(s_cfg.callsign) - 1);
        String mapDir = prefs.getString("mapTileDir", s_cfg.mapTileDir);
        strncpy(s_cfg.mapTileDir, mapDir.c_str(), sizeof(s_cfg.mapTileDir) - 1);

        s_cfg.activeChannel     = prefs.getInt("channel",       s_cfg.activeChannel);
        s_cfg.bluetoothEnabled  = prefs.getBool("bluetooth",    s_cfg.bluetoothEnabled);
        s_cfg.speakerEnabled    = prefs.getBool("speaker",      s_cfg.speakerEnabled);
        s_cfg.gpsMode           = prefs.getBool("gps", true) ? 2 : 0;  // legacy bool key
        s_cfg.kbBrightness      = prefs.getUChar("kbBright",    128);
        s_cfg.kbLayout          = prefs.getUChar("kbLayout",    0);
        s_cfg.autoAddClient     = prefs.getBool("aaclient",     s_cfg.autoAddClient);
        s_cfg.autoAddRepeater   = prefs.getBool("aarepeater",   s_cfg.autoAddRepeater);
        s_cfg.saveMsgs          = prefs.getBool("saveMsgs",     s_cfg.saveMsgs);
        s_cfg.showHops          = prefs.getBool("showHops",     s_cfg.showHops);
        s_cfg.showRssi          = prefs.getBool("showRssi",     s_cfg.showRssi);
        s_cfg.locationSharing   = prefs.getBool("locShare",     s_cfg.locationSharing);
        s_cfg.notifyPopup       = prefs.getBool("notifyPopup",  s_cfg.notifyPopup);
        s_cfg.brightness        = prefs.getInt("brightness",    s_cfg.brightness);
        s_cfg.screenTimeoutSec  = prefs.getInt("screenTimeout", s_cfg.screenTimeoutSec);
        s_cfg.screenOffMin      = prefs.getUChar("screenOff",    0);
        s_cfg.notifySound       = prefs.getBool("notifySound",  s_cfg.notifySound);
        s_cfg.notifySoundChoice = prefs.getUChar("notifySndCh", 0);
        s_cfg.theme             = prefs.getInt("theme",         s_cfg.theme);
        s_cfg.radioProfile      = prefs.getUChar("radioProf",   s_cfg.radioProfile);
        s_cfg.showAdverts       = prefs.getBool("showAdverts",  s_cfg.showAdverts);
        s_cfg.radioCustom       = prefs.getBool("radioCustom",  false);
        s_cfg.freqMHz           = prefs.getFloat("freqMHz",     0.0f);
        s_cfg.radioSF           = prefs.getUChar("radioSF",     0);
        s_cfg.radioBW           = prefs.getUChar("radioBW",     0);
        s_cfg.radioCR           = prefs.getUChar("radioCR",     0);
        s_cfg.radioTX           = (int8_t)prefs.getChar("radioTX", 0);
        s_cfg.manualLat         = prefs.getFloat("manualLat",   0.0f);
        s_cfg.manualLon         = prefs.getFloat("manualLon",   0.0f);
        s_cfg.autoForward       = prefs.getBool("autoFwd",      true);
        s_cfg.pathHashSz        = prefs.getUChar("pathHashSz",  0);
        s_cfg.timezoneOffsetHours = (int8_t)prefs.getChar("tzOff", 0);
        {
            String scope = prefs.getString("scopeTag", "");
            strncpy(s_cfg.scopeTag, scope.c_str(), sizeof(s_cfg.scopeTag) - 1);
            s_cfg.scopeTag[sizeof(s_cfg.scopeTag) - 1] = '\0';
        }
        for (int i = 0; i < 10; i++) {
            char key[12];
            snprintf(key, sizeof(key), "ch%dname", i);
            String n = prefs.getString(key, s_cfg.channels[i].name);
            strncpy(s_cfg.channels[i].name, n.c_str(), sizeof(s_cfg.channels[i].name) - 1);
            snprintf(key, sizeof(key), "ch%dsn", i);
            String sn = prefs.getString(key, s_cfg.channels[i].shortname);
            strncpy(s_cfg.channels[i].shortname, sn.c_str(), sizeof(s_cfg.channels[i].shortname) - 1);
            snprintf(key, sizeof(key), "ch%dpsk", i);
            String pk = prefs.getString(key, s_cfg.channels[i].psk);
            strncpy(s_cfg.channels[i].psk, pk.c_str(), sizeof(s_cfg.channels[i].psk) - 1);
            snprintf(key, sizeof(key), "ch%dnotify", i);
            s_cfg.channels[i].notify = prefs.getBool(key, false);
            snprintf(key, sizeof(key), "ch%dscope", i);
            String sc = prefs.getString(key, "");
            strncpy(s_cfg.channels[i].scope, sc.c_str(), sizeof(s_cfg.channels[i].scope) - 1);
            s_cfg.channels[i].scope[sizeof(s_cfg.channels[i].scope) - 1] = '\0';
        }
        prefs.end();
        // Clear old per-key entries and write compact blob.
        if (prefs.begin("ops", false)) { prefs.clear(); prefs.end(); }
        OPS_LOG("Config", "Migrated from legacy NVS keys: callsign=%s", s_cfg.callsign);
        save();
        return;
    }
    prefs.end();

    // ── Factory defaults ────────────────────────────────────────────────
    if (!_loadFromSD())
        OPS_LOG("Config", "No saved config, using defaults");
    save();
}

void config::save() {
    if (!prefs.begin("ops", /*readOnly=*/false)) {
        OPS_LOG("Config", "Failed to open NVS for write");
        return;
    }
    // Single blob — 1 NVS entry regardless of how many fields Config has.
    prefs.putBytes("cfg", &s_cfg, sizeof(s_cfg));
    prefs.end();
    _saveToSD();
    OPS_LOG("Config", "Saved");
}

void config::setCallsign(const char* cs) {
    strncpy(s_cfg.callsign, cs, sizeof(s_cfg.callsign) - 1);
    s_cfg.callsign[sizeof(s_cfg.callsign) - 1] = '\0';
    save();
}

void config::setRegion(const char* reg) {
    strncpy(s_cfg.radioRegion, reg, sizeof(s_cfg.radioRegion) - 1);
    s_cfg.radioRegion[sizeof(s_cfg.radioRegion) - 1] = '\0';
    save();
}

time_t config::localEpoch() {
    return time(nullptr) + (time_t)s_cfg.timezoneOffsetHours * 3600;
}

void config::setTouchCal(float xScale, float xOff, float yScale, float yOff) {
    s_cfg.touchCalXScale = xScale;
    s_cfg.touchCalXOff   = xOff;
    s_cfg.touchCalYScale = yScale;
    s_cfg.touchCalYOff   = yOff;
    save();
}

void config::setChannelNotify(int idx, bool notify) {
    if (idx < 0 || idx > 9) return;
    s_cfg.channels[idx].notify = notify;
    save();
}

void config::setChannel(int idx, const char* name, const char* psk, const char* shortname, const char* scope) {
    if (idx < 0 || idx > 9) return;
    strncpy(s_cfg.channels[idx].name,      name,      sizeof(s_cfg.channels[idx].name)      - 1);
    strncpy(s_cfg.channels[idx].psk,       psk,       sizeof(s_cfg.channels[idx].psk)       - 1);
    strncpy(s_cfg.channels[idx].shortname, shortname, sizeof(s_cfg.channels[idx].shortname) - 1);
    strncpy(s_cfg.channels[idx].scope,     scope ? scope : "",
                                           sizeof(s_cfg.channels[idx].scope)     - 1);
    s_cfg.channels[idx].name[sizeof(s_cfg.channels[idx].name)           - 1] = '\0';
    s_cfg.channels[idx].psk[sizeof(s_cfg.channels[idx].psk)             - 1] = '\0';
    s_cfg.channels[idx].shortname[sizeof(s_cfg.channels[idx].shortname) - 1] = '\0';
    s_cfg.channels[idx].scope[sizeof(s_cfg.channels[idx].scope)         - 1] = '\0';
    save();
}

}  // namespace ops
