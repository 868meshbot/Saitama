// Saitama — ScreenFoxhunt.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// ─────────────────────────────────────────────────────────────────────────────
// ATTRIBUTION
// ─────────────────────────────────────────────────────────────────────────────
// The proximity-beeper concept and the shape of the RSSI -> beep-interval
// mapping are taken from OUI-SPY Foxhunter:
//
//     https://github.com/colonelpanichacks/ouispy-foxhunter
//
// What is the same: the idea of banding RSSI and interpolating a beep interval
// inside each band, a solid tone at very close range, and a lost-target
// timeout that silences the beeper.
//
// What is different, and why:
//
//   * Audio. The reference drives a PWM piezo on a GPIO (ledcWriteTone), which
//     can start and stop a tone instantly, so it can beep as fast as every
//     10 ms. We have a MAX98357A on I2S at 8 kHz with a 2048-sample DMA queue;
//     a burst has to be rendered and queued, so intervals below ~60 ms simply
//     blur into a continuous tone. The bands below are therefore rescaled to
//     60-1500 ms rather than 10-3000 ms.
//   * Pitch. Because we synthesise the waveform anyway, pitch rises with
//     signal strength as a second cue on top of rate. The reference uses a
//     fixed 1 kHz tone. Two independent cues make it much easier to tell
//     "getting warmer" from "same distance" while sweeping an antenna.
//   * Target selection. The reference configures a target MAC through a WiFi
//     AP and a web portal. We scan and present a live list on the screen, so
//     there is no second device or network involved.
//   * BLE stack. The reference uses NimBLE; this firmware already links
//     Bluedroid (BLEDevice.h) for the companion service, so we use that.
//
// ─────────────────────────────────────────────────────────────────────────────
//
// Layout (320 x 240):
//
//   LIST                                  HUNT
//   +--------------------------+          +--------------------------+
//   | [home] Foxhunt   scanning|          | [home] Foxhunt      back |
//   +--------------------------+          +--------------------------+
//   | name            -54 dBm  |          |        -54 dBm           |
//   | AA:BB:CC:DD:EE:FF        |          |   [======        ]       |
//   | name            -71 dBm  |          |   TargetName             |
//   | ...                      |          |   AA:BB:CC:DD:EE:FF      |
//   +--------------------------+          |   WARM - 320 ms          |
//   | 12 seen - pick a target  |          +--------------------------+

#include "ScreenFoxhunt.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "Emoji.h"
#include "../utils/Config.h"
#include "../utils/Sound.h"
#include "../utils/Log.h"
#include "../bt/BTCompanionService.h"
#include "../utils/SDCard.h"
#include <ArduinoJson.h>
#include <SD.h>

#include <lvgl.h>
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLEUUID.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace ops { namespace ui {

static constexpr int TOP_H  = 28;
static constexpr int FOOT_H = 16;
static constexpr int ROW_H  = 34;

// ── Device table ─────────────────────────────────────────────────────────────
// Written from the BLE scan task, read from the LVGL task. Entries are only
// ever appended or updated in place (never reordered or removed) so a reader
// racing a writer sees a stale RSSI at worst, never a torn pointer. That is
// the right trade here: taking a lock in a BLE callback risks stalling the
// controller, and a one-frame-old RSSI is invisible to the user.
static constexpr int MAX_DEVICES = 40;

struct FoxDevice {
    char     name[24];    // broadcast local name — empty if the device sends none
    char     vendor[14];  // inferred from manufacturer data; NOT a broadcast name
    char     mac[18];
    uint8_t  addrType;    // esp_ble_addr_type_t — connect() needs the right one
    bool     isAlias;     // name[] came from the user, not from the airwaves
    volatile int8_t   rssi;
    volatile uint32_t lastSeenMs;
};

// ── Advertisement parsing helpers ────────────────────────────────────────────
//
// The Arduino BLE library's parseAdvertisement() only handles AD type 0x09
// (Complete Local Name) — 0x08 (Shortened Local Name) appears nowhere in it,
// so devices that broadcast a short name are reported as nameless. We re-walk
// the raw payload ourselves to recover those, and to pull the manufacturer
// company ID so that genuinely nameless devices are still distinguishable.
//
// AD structure layout: [len][type][data...] repeated, len counts type+data.
static bool _findAdField(const uint8_t* p, size_t len, uint8_t wantType,
                         const uint8_t** out, uint8_t* outLen)
{
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t fieldLen = p[i];
        if (fieldLen == 0) break;                 // end of data
        if (i + fieldLen >= len + 1) break;       // malformed / truncated
        uint8_t type = p[i + 1];
        if (type == wantType) {
            *out    = &p[i + 2];
            *outLen = (uint8_t)(fieldLen - 1);
            return *outLen > 0;
        }
        i += fieldLen + 1;
    }
    return false;
}

// Bluetooth SIG company identifiers (little-endian in the payload). Only IDs
// we are confident of are named; anything else is shown as its raw hex ID,
// which is still enough to tell two nameless devices apart.
static const char* _companyName(uint16_t id)
{
    switch (id) {
        case 0x004C: return "Apple";
        case 0x0006: return "Microsoft";
        case 0x00E0: return "Google";
        case 0x0075: return "Samsung";
        case 0x0059: return "Nordic";
        case 0x02E5: return "Espressif";
        default:     return nullptr;
    }
}


// ── BLE address classification ───────────────────────────────────────────────
//
// A random BLE address encodes its own kind in the top two bits of the most
// significant octet — which is the FIRST octet of the printed string, because
// BLEAddress::toString() emits m_address[0] first.
//
//   0b11  Static Random          stable until the device reboots
//   0b01  Resolvable Private     ROTATES, typically every ~15 minutes
//   0b00  Non-Resolvable Private ROTATES
//
// This matters because everything we key on a MAC — the saved-name store, the
// hunt target itself — silently goes stale when the address rotates. A device
// renamed today is "(no name)" again after the next rotation, and the hunt
// loses its target mid-walk. So we detect it and say so rather than letting
// the user discover it the hard way.
enum AddrKind : uint8_t {
    ADDR_PUBLIC = 0,      // globally unique, assigned to the vendor
    ADDR_STATIC_RANDOM,   // random but stable for this power cycle
    ADDR_RESOLVABLE,      // RPA — rotates
    ADDR_NON_RESOLVABLE,  // NRPA — rotates
};

static uint8_t _hexOctet(const char* h)
{
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int hi = nib(h[0]), lo = nib(h[1]);
    if (hi < 0 || lo < 0) return 0;
    return (uint8_t)((hi << 4) | lo);
}

static AddrKind _addrKind(const FoxDevice& d)
{
    if (d.addrType == BLE_ADDR_TYPE_PUBLIC) return ADDR_PUBLIC;
    if (!d.mac[0] || !d.mac[1]) return ADDR_STATIC_RANDOM;
    switch ((_hexOctet(d.mac) >> 6) & 0x03) {
        case 0x03: return ADDR_STATIC_RANDOM;
        case 0x01: return ADDR_RESOLVABLE;
        case 0x00: return ADDR_NON_RESOLVABLE;
        default:   return ADDR_STATIC_RANDOM;   // 0b10 is reserved
    }
}

// True when the address will change under us, invalidating anything keyed on it.
static bool _addrRotates(const FoxDevice& d)
{
    AddrKind k = _addrKind(d);
    return k == ADDR_RESOLVABLE || k == ADDR_NON_RESOLVABLE;
}

static const char* _addrKindText(AddrKind k)
{
    switch (k) {
        case ADDR_PUBLIC:          return "public address - stable";
        case ADDR_STATIC_RANDOM:   return "static random - stable";
        case ADDR_RESOLVABLE:      return "PRIVATE ROTATING (RPA)";
        default:                   return "PRIVATE ROTATING (NRPA)";
    }
}

static FoxDevice     s_dev[MAX_DEVICES];
static volatile int  s_devCount = 0;
static volatile bool s_dirty    = false;   // list needs a rebuild

enum FoxMode : uint8_t { MODE_LIST = 0, MODE_HUNT };
static FoxMode s_mode = MODE_LIST;
static int  s_sel        = 0;      // highlighted row in MODE_LIST
static int  s_targetIdx  = -1;     // index into s_dev while hunting
static bool s_scanning   = false;
static bool s_bleFailed  = false;

// Beeper state
static uint32_t s_lastBeepMs   = 0;
static bool     s_acquired     = false;   // target currently in range
static uint32_t s_lastRebuildMs = 0;

// Per-row RSSI labels, so live values refresh without rebuilding the list
// (a full rebuild twice a second would flicker and fight the user's scrolling).
static lv_obj_t* s_rowRssiLbl[MAX_DEVICES] = {};
static int       s_rowCount = 0;

static BLEScan* s_scan = nullptr;

// Target considered lost after this long with no advertisement.
static constexpr uint32_t LOST_MS = 5000;

// ── RSSI -> beep interval / pitch ────────────────────────────────────────────
//
// Banded interpolation, following the reference's structure but rescaled for
// I2S latency (see the attribution note at the top). Returns milliseconds
// between beep onsets; 0 means "solid tone, you are on top of it".
static int _beepIntervalMs(int rssi)
{
    if (rssi >= -30) return 0;                       // solid — right here
    if (rssi >= -40) return map(rssi, -40, -30,  120,   60);
    if (rssi >= -50) return map(rssi, -50, -40,  220,  120);
    if (rssi >= -60) return map(rssi, -60, -50,  380,  220);
    if (rssi >= -70) return map(rssi, -70, -60,  600,  380);
    if (rssi >= -80) return map(rssi, -80, -70,  900,  600);
    if (rssi >= -90) return map(rssi, -90, -80, 1500,  900);
    return 1500;
}

// Pitch rises with signal strength — the second cue the reference does not have.
static int _beepFreqHz(int rssi)
{
    if (rssi > -30) rssi = -30;
    if (rssi < -95) rssi = -95;
    return map(rssi, -95, -30, 500, 2200);
}

static const char* _proximityWord(int rssi)
{
    if (rssi >= -30) return "ON TOP";
    if (rssi >= -45) return "BURNING";
    if (rssi >= -60) return "HOT";
    if (rssi >= -70) return "WARM";
    if (rssi >= -80) return "COOL";
    return "COLD";
}


// ── User-assigned names (aliases) ────────────────────────────────────────────
//
// Most BLE devices broadcast no name at all, so the scan list is mostly
// "(no name)". A local alias store fixes that: the user labels a device once
// and it is recognised on every later scan.
//
// Kept as a flat RAM table because the BLE scan callback consults it on every
// new device and MUST NOT touch the SD card. SD I/O happens only on load
// (screen open) and on an explicit rename.
static constexpr int MAX_ALIASES = 64;
struct FoxAlias { char mac[18]; char name[24]; };
static FoxAlias s_alias[MAX_ALIASES];
static int      s_aliasCount  = 0;
static bool     s_aliasLoaded = false;

static const char* kAliasPath = "/ops/foxnames.json";

static int _aliasFind(const char* mac)
{
    for (int i = 0; i < s_aliasCount; i++)
        if (strncmp(s_alias[i].mac, mac, sizeof(s_alias[i].mac) - 1) == 0) return i;
    return -1;
}

void ScreenFoxhunt::_aliasLoad()
{
    s_aliasCount  = 0;
    s_aliasLoaded = true;
    if (!ops::sdcard::isMounted()) return;
    if (!SD.exists(kAliasPath)) return;

    File f = SD.open(kAliasPath, FILE_READ);
    if (!f) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { OPS_LOG("Fox", "foxnames.json parse failed: %s", err.c_str()); return; }

    for (JsonObject o : doc["devices"].as<JsonArray>()) {
        if (s_aliasCount >= MAX_ALIASES) break;
        const char* m = o["mac"]  | "";
        const char* n = o["name"] | "";
        if (!m[0] || !n[0]) continue;
        strncpy(s_alias[s_aliasCount].mac,  m, sizeof(s_alias[0].mac)  - 1);
        strncpy(s_alias[s_aliasCount].name, n, sizeof(s_alias[0].name) - 1);
        s_alias[s_aliasCount].mac[sizeof(s_alias[0].mac) - 1]   = '\0';
        s_alias[s_aliasCount].name[sizeof(s_alias[0].name) - 1] = '\0';
        s_aliasCount++;
    }
    OPS_LOG("Fox", "Loaded %d saved name(s)", s_aliasCount);
}

void ScreenFoxhunt::_aliasSave()
{
    if (!ops::sdcard::isMounted()) {
        OPS_LOG("Fox", "No SD card - name not saved");
        return;
    }
    if (!SD.exists("/ops")) SD.mkdir("/ops");
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (int i = 0; i < s_aliasCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["mac"]  = s_alias[i].mac;
        o["name"] = s_alias[i].name;
    }
    SD.remove(kAliasPath);          // FILE_WRITE appends; clear for a clean write
    File f = SD.open(kAliasPath, FILE_WRITE);
    if (!f) { OPS_LOG("Fox", "foxnames.json open failed"); return; }
    serializeJson(doc, f);
    f.close();
    OPS_LOG("Fox", "Saved %d name(s) to %s", s_aliasCount, kAliasPath);
}

// Upsert an alias and persist. Empty name removes the entry.
void ScreenFoxhunt::_aliasSet(const char* mac, const char* name)
{
    int i = _aliasFind(mac);
    if (!name || !name[0]) {
        if (i < 0) return;
        for (int k = i; k < s_aliasCount - 1; k++) s_alias[k] = s_alias[k + 1];
        s_aliasCount--;
    } else {
        if (i < 0) {
            if (s_aliasCount >= MAX_ALIASES) { OPS_LOG("Fox", "Alias table full"); return; }
            i = s_aliasCount++;
            strncpy(s_alias[i].mac, mac, sizeof(s_alias[0].mac) - 1);
            s_alias[i].mac[sizeof(s_alias[0].mac) - 1] = '\0';
        }
        strncpy(s_alias[i].name, name, sizeof(s_alias[0].name) - 1);
        s_alias[i].name[sizeof(s_alias[0].name) - 1] = '\0';
    }
    ScreenFoxhunt::_aliasSave();
}

// ── Static members ───────────────────────────────────────────────────────────
lv_obj_t* ScreenFoxhunt::_screen    = nullptr;
lv_obj_t* ScreenFoxhunt::_title     = nullptr;
lv_obj_t* ScreenFoxhunt::_list      = nullptr;
lv_obj_t* ScreenFoxhunt::_hunt      = nullptr;
lv_obj_t* ScreenFoxhunt::_rssiLbl   = nullptr;
lv_obj_t* ScreenFoxhunt::_bar       = nullptr;
lv_obj_t* ScreenFoxhunt::_targetLbl = nullptr;
lv_obj_t* ScreenFoxhunt::_stateLbl  = nullptr;
lv_obj_t* ScreenFoxhunt::_addrLbl   = nullptr;
lv_obj_t* ScreenFoxhunt::_rateLbl   = nullptr;
lv_obj_t* ScreenFoxhunt::_hintLbl   = nullptr;

// ── BLE scan callback ────────────────────────────────────────────────────────
// Runs in the BLE task. Must not touch LVGL, allocate, or block.
class FoxScanCallbacks : public BLEAdvertisedDeviceCallbacks
{
    void onResult(BLEAdvertisedDevice adv) override
    {
        // getAddress().toString() returns a std::string BY VALUE. Holding its
        // c_str() past the end of this statement would dangle, so copy the
        // text out into our own storage before using it.
        char mac[18];
        {
            std::string s = adv.getAddress().toString();
            strncpy(mac, s.c_str(), sizeof(mac) - 1);
            mac[sizeof(mac) - 1] = '\0';
        }
        int8_t rssi = (int8_t)adv.getRSSI();
        uint32_t now = millis();

        // Resolve a name: complete local name (library) first, then the
        // shortened local name the library skips.
        // NOTE: a BLE local name is arbitrary vendor-controlled UTF-8. Anything
        // outside the compiled font range renders as tofu, so run it through
        // the same sanitiser the mesh peer names use before storing it.
        char name[24] = {};
        if (adv.haveName()) {
            strncpy(name, adv.getName().c_str(), sizeof(name) - 1);
        } else {
            const uint8_t* p   = adv.getPayload();
            size_t         plen = adv.getPayloadLength();
            const uint8_t* f; uint8_t flen;
            if (p && plen && _findAdField(p, plen, 0x08 /*short name*/, &f, &flen)) {
                if (flen > sizeof(name) - 1) flen = sizeof(name) - 1;
                memcpy(name, f, flen);
                name[flen] = '\0';
            }
        }

        ops::theme::sanitizeText(name);

        // A saved user alias always wins over whatever the device broadcasts:
        // it is an explicit choice, and it is usually the only label a device
        // has at all. Lookup is against the in-RAM table only — never SD, this
        // runs in the BLE task.
        bool isAlias = false;
        {
            int ai = _aliasFind(mac);
            if (ai >= 0) {
                strncpy(name, s_alias[ai].name, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
                isAlias = true;
            }
        }

        // Manufacturer company ID - lets nameless devices still be told apart.
        char vendor[14] = {};
        {
            const uint8_t* p    = adv.getPayload();
            size_t         plen = adv.getPayloadLength();
            const uint8_t* f; uint8_t flen;
            if (p && plen && _findAdField(p, plen, 0xFF /*mfr data*/, &f, &flen) && flen >= 2) {
                uint16_t cid = (uint16_t)f[0] | ((uint16_t)f[1] << 8);
                const char* cn = _companyName(cid);
                if (cn) snprintf(vendor, sizeof(vendor), "%s", cn);
                else    snprintf(vendor, sizeof(vendor), "ID:%04X", cid);
            }
        }

        int count = s_devCount;
        for (int i = 0; i < count; i++) {
            if (strncmp(s_dev[i].mac, mac, sizeof(s_dev[i].mac) - 1) == 0) {
                s_dev[i].rssi       = rssi;
                s_dev[i].lastSeenMs = now;
                // Later adverts (or the scan response) may carry detail the
                // first one lacked — fill gaps in, never overwrite.
                if (name[0] && (s_dev[i].name[0] == '\0' ||
                                (isAlias && !s_dev[i].isAlias))) {
                    strncpy(s_dev[i].name, name, sizeof(s_dev[i].name) - 1);
                    s_dev[i].isAlias = isAlias;
                    s_dirty = true;
                }
                if (s_dev[i].vendor[0] == '\0' && vendor[0]) {
                    strncpy(s_dev[i].vendor, vendor, sizeof(s_dev[i].vendor) - 1);
                    s_dirty = true;
                }
                return;
            }
        }
        if (count >= MAX_DEVICES) return;   // table full — ignore new devices

        FoxDevice& d = s_dev[count];
        memset(d.name,   0, sizeof(d.name));
        memset(d.vendor, 0, sizeof(d.vendor));
        strncpy(d.name,   name,   sizeof(d.name) - 1);
        strncpy(d.vendor, vendor, sizeof(d.vendor) - 1);
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
        d.mac[sizeof(d.mac) - 1] = '\0';
        d.addrType   = (uint8_t)adv.getAddressType();
        d.isAlias    = isAlias;
        d.rssi       = rssi;
        d.lastSeenMs = now;
        s_devCount   = count + 1;   // publish only after the entry is complete
        s_dirty      = true;
    }
};

static FoxScanCallbacks s_scanCb;

// ── GATT name-probe state (declared early: stop() abandons it) ─────────────
enum ProbeState : uint8_t { PROBE_IDLE = 0, PROBE_REQ, PROBE_RUN, PROBE_OK, PROBE_FAIL };

static volatile uint8_t  s_probeState = PROBE_IDLE;
static volatile uint32_t s_probeGen   = 0;      // bumped whenever a probe is abandoned
static int      s_probeIdx      = -1;
static char     s_probeMac[18]  = {};
static uint8_t  s_probeAddrType = 0;
static char     s_probeName[24] = {};
static uint32_t s_probeStartMs  = 0;

static constexpr uint32_t PROBE_TIMEOUT_MS = 20000;

// ── Rename-dialog state (declared early: stop() closes it) ─────────────────
static lv_obj_t* s_renameOv = nullptr;   // overlay for the rename dialog
static lv_obj_t* s_renameTa = nullptr;
static int       s_popupIdx = -1;        // device the dialog is editing
static lv_obj_t* s_focusObj = nullptr;   // invisible key sink for this screen

static bool s_refocusPending = false;

// Save/Cancel are children of the overlay, and they call this from inside
// their OWN event handler. Deleting an ancestor of the object currently
// dispatching an event is undefined behaviour in LVGL (lv_obj_del_async
// exists exactly for this case) — doing it synchronously corrupted the group
// and event state after a few uses, which is why rename/forget silently
// stopped responding. Defer the delete to the end of the LVGL cycle.
static void _closeRename()
{
    if (!s_renameOv) return;
    lv_obj_del_async(s_renameOv);
    s_renameOv = nullptr;
    s_renameTa = nullptr;
    // The delete (and its group removal) lands later, so re-assert focus from
    // tick() rather than here, or LVGL's own refocus would overwrite it.
    s_refocusPending = true;
}

// ── Scan control ─────────────────────────────────────────────────────────────
static bool _startScan()
{
    if (s_scanning) return true;

    // The BLE controller claims ~60 KB of contiguous internal DMA SRAM, which
    // main.cpp deliberately reserves BEFORE LVGL takes its draw buffers. If
    // Bluetooth was off at boot that memory is long gone to LVGL, so an init
    // here would fail with "Start HCI Host Layer Failure". Detect that case
    // and tell the user to enable Bluetooth and reboot, rather than crashing.
    if (!ops::BTCompanionService::instance().isRunning()) {
        OPS_LOG("Fox", "BLE stack not up (Bluetooth off at boot) - cannot scan");
        s_bleFailed = true;
        return false;
    }

    s_scan = BLEDevice::getScan();
    if (!s_scan) { s_bleFailed = true; return false; }

    s_scan->setAdvertisedDeviceCallbacks(&s_scanCb, /*wantDuplicates=*/true);
    s_scan->setActiveScan(true);   // ask for scan responses — gets us names
    s_scan->setInterval(100);
    s_scan->setWindow(99);         // ~99 % duty cycle
    s_scan->start(0, nullptr, false);   // 0 = run until stopped

    s_scanning  = true;
    s_bleFailed = false;
    OPS_LOG("Fox", "BLE scan started");
    return true;
}

static void _stopScan()
{
    if (!s_scanning) return;
    if (s_scan) {
        s_scan->stop();
        s_scan->clearResults();
    }
    s_scanning = false;
    OPS_LOG("Fox", "BLE scan stopped");
}

// ── stop() ───────────────────────────────────────────────────────────────────
void ScreenFoxhunt::stop()
{
    _closeRename();
    // Abandon an in-flight name query: bump the generation so the worker's
    // late result is ignored, and clear the state so _probeTick() will not
    // quietly restart scanning after the user has left the screen.
    if (s_probeState != PROBE_IDLE) {
        s_probeGen++;
        s_probeState = PROBE_IDLE;
    }
    _stopScan();
    s_acquired = false;
}

bool ScreenFoxhunt::isActive() { return _screen && lv_scr_act() == _screen; }

// ── List rendering ───────────────────────────────────────────────────────────
void ScreenFoxhunt::_rebuildList()
{
    if (!_list) return;
    lv_obj_clean(_list);          // invalidates every cached row label
    memset(s_rowRssiLbl, 0, sizeof(s_rowRssiLbl));
    s_rowCount = 0;

    int count = s_devCount;
    if (count == 0) {
        lv_obj_t* hint = lv_label_create(_list);
        lv_label_set_text(hint, s_bleFailed
            ? "Bluetooth is off.\nEnable it in Settings, then reboot."
            : "Scanning for BLE devices...");
        lv_obj_set_style_text_color(hint, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(hint, theme::bodyFont12(), 0);
        return;
    }
    if (s_sel >= count) s_sel = count - 1;
    if (s_sel < 0)      s_sel = 0;

    for (int i = 0; i < count; i++) {
        lv_obj_t* row = lv_btn_create(_list);
        lv_group_remove_obj(row);
        lv_obj_set_size(row, lv_pct(100), ROW_H);
        lv_obj_set_style_bg_color(row, (i == s_sel) ? theme::PRIMARY : theme::BG_CARD, 0);
        lv_obj_set_style_bg_color(row, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, _onRowClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, s_dev[i].name[0] ? s_dev[i].name : "(no name)");
        lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, theme::bodyFont12(), 0);
        lv_obj_set_width(nameLbl, 190);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);
        lv_obj_align(nameLbl, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t* macLbl = lv_label_create(row);
        {
            char mbuf[40];
            if (s_dev[i].vendor[0])
                snprintf(mbuf, sizeof(mbuf), "%s  %s", s_dev[i].mac, s_dev[i].vendor);
            else
                snprintf(mbuf, sizeof(mbuf), "%s", s_dev[i].mac);
            lv_label_set_text(macLbl, mbuf);
        }
        lv_obj_set_style_text_color(macLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(macLbl, &lv_font_montserrat_10, 0);
        lv_obj_align(macLbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        char rbuf[12];
        int r = s_dev[i].rssi;
        snprintf(rbuf, sizeof(rbuf), "%d dBm", r);
        lv_obj_t* rssiLbl = lv_label_create(row);
        lv_label_set_text(rssiLbl, rbuf);
        lv_obj_set_style_text_color(rssiLbl,
            r > -60 ? theme::GREEN : (r > -80 ? theme::ORANGE : theme::RED), 0);
        lv_obj_set_style_text_font(rssiLbl, &lv_font_montserrat_10, 0);
        lv_obj_align(rssiLbl, LV_ALIGN_RIGHT_MID, 0, 0);
        s_rowRssiLbl[i] = rssiLbl;
    }
    s_rowCount = count;

    if (_hintLbl) {
        char b[40];
        snprintf(b, sizeof(b), "%d seen - pick a target", count);
        lv_label_set_text(_hintLbl, b);
    }
}

// Refresh just the RSSI text/colour on existing rows — no object churn, so
// the user's scroll position and selection survive.
void ScreenFoxhunt::_refreshListValues()
{
    int n = s_devCount;
    if (n > s_rowCount) n = s_rowCount;
    for (int i = 0; i < n; i++) {
        if (!s_rowRssiLbl[i]) continue;
        int  r    = s_dev[i].rssi;
        bool live = (millis() - s_dev[i].lastSeenMs) < LOST_MS;
        char rbuf[12];
        if (live) snprintf(rbuf, sizeof(rbuf), "%d dBm", r);
        else      snprintf(rbuf, sizeof(rbuf), "--");
        lv_label_set_text(s_rowRssiLbl[i], rbuf);
        lv_obj_set_style_text_color(s_rowRssiLbl[i],
            !live  ? theme::TEXT_MUTED :
            r > -60 ? theme::GREEN : (r > -80 ? theme::ORANGE : theme::RED), 0);
    }
}

void ScreenFoxhunt::_updateHighlight()
{
    if (!_list) return;
    uint32_t n = lv_obj_get_child_cnt(_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* row = lv_obj_get_child(_list, i);
        if (!row) continue;
        lv_obj_set_style_bg_color(row,
            ((int)i == s_sel) ? theme::PRIMARY : theme::BG_CARD, 0);
    }
    if (s_sel >= 0 && s_sel < (int)n) {
        lv_obj_t* row = lv_obj_get_child(_list, (uint32_t)s_sel);
        if (row) lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    }
}



// ── 'g' — query the device's name over GATT ──────────────────────────────────
//
// Most BLE devices broadcast no name in their advertisement, but a connectable
// one usually still exposes the GAP "Device Name" characteristic
// (service 0x1800, characteristic 0x2A00). Connecting and reading it is the
// only way to get a real name out of those.
//
// Caveats, all unavoidable:
//   * It is an ACTIVE connection, not passive listening. The target sees it.
//   * Only works on connectable devices — beacons and non-connectable
//     advertisers will simply time out.
//   * BLEClient::connect() BLOCKS for up to several seconds, so scanning is
//     paused around it and the UI freezes briefly. The caller paints a
//     "querying" state first.
//   * Some devices require bonding before they will serve even the GAP name.
//
// BLEDevice::createClient() does `new BLEClient()` on every call and this
// library version exposes no deleteClient(), so one client is created lazily
// and reused rather than leaking one per query.
static BLEClient* s_gattClient = nullptr;

static bool _gattReadName(const char* mac, uint8_t addrType, char* out, size_t outSize)
{
    if (!s_gattClient) {
        s_gattClient = BLEDevice::createClient();
        if (!s_gattClient) return false;
    }
    bool ok = false;
    if (s_gattClient->connect(BLEAddress(std::string(mac)),
                              (esp_ble_addr_type_t)addrType)) {
        BLERemoteService* svc = s_gattClient->getService(BLEUUID((uint16_t)0x1800));
        if (svc) {
            BLERemoteCharacteristic* ch = svc->getCharacteristic(BLEUUID((uint16_t)0x2A00));
            if (ch) {
                std::string v = ch->readValue();
                if (!v.empty()) {
                    strncpy(out, v.c_str(), outSize - 1);
                    out[outSize - 1] = '\0';
                    ok = true;
                }
            }
        }
        s_gattClient->disconnect();
    }
    return ok;
}

// ── Rename dialog ───────────────────────────────────────────────────────────

void ScreenFoxhunt::_onRenameSave(lv_event_t*)
{
    if (!s_renameTa || s_popupIdx < 0 || s_popupIdx >= s_devCount) { _closeRename(); return; }
    char name[24] = {};
    strncpy(name, lv_textarea_get_text(s_renameTa), sizeof(name) - 1);
    ops::theme::sanitizeText(name);

    // Trim trailing spaces so an all-blank entry counts as "clear the alias".
    for (int i = (int)strlen(name) - 1; i >= 0 && name[i] == ' '; i--) name[i] = '\0';

    char mac[18];
    strncpy(mac, s_dev[s_popupIdx].mac, sizeof(mac) - 1);
    mac[sizeof(mac) - 1] = '\0';

    _aliasSet(mac, name);

    // Reflect it immediately; the scan callback would also do this on the next
    // advert, but the user should see the change without waiting for one.
    if (name[0]) {
        strncpy(s_dev[s_popupIdx].name, name, sizeof(s_dev[0].name) - 1);
        s_dev[s_popupIdx].name[sizeof(s_dev[0].name) - 1] = '\0';
        s_dev[s_popupIdx].isAlias = true;
    } else {
        s_dev[s_popupIdx].name[0] = '\0';
        s_dev[s_popupIdx].isAlias = false;
    }
    _closeRename();
    _rebuildList();
}

void ScreenFoxhunt::_onRenameCancel(lv_event_t*) { _closeRename(); }

void ScreenFoxhunt::_onRenameKey(lv_event_t* e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC) _closeRename();
}


// ── _showRenameDialog() ──────────────────────────────────────────────────────
void ScreenFoxhunt::_showRenameDialog(int devIdx)
{
    if (devIdx < 0 || devIdx >= s_devCount) return;
    _closeRename();
    s_popupIdx = devIdx;

    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    s_renameOv = overlay;
    lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, _onRenameKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(overlay);
    lv_obj_set_size(panel, 260, 132);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::ACCENT, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Rename device");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t* macLbl = lv_label_create(panel);
    lv_label_set_text(macLbl, s_dev[devIdx].mac);
    lv_obj_set_style_text_color(macLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(macLbl, &lv_font_montserrat_10, 0);

    lv_obj_t* ta = lv_textarea_create(panel);
    s_renameTa = ta;
    lv_obj_set_size(ta, 236, 30);
    lv_obj_set_style_bg_color(ta, theme::BG, 0);
    lv_obj_set_style_text_color(ta, theme::TEXT, 0);
    lv_obj_set_style_border_color(ta, theme::ACCENT, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_text_font(ta, theme::bodyFont12(), 0);
    lv_textarea_set_max_length(ta, 23);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "blank = clear saved name");
    if (s_dev[devIdx].isAlias) lv_textarea_set_text(ta, s_dev[devIdx].name);
    else                        lv_textarea_set_text(ta, "");

    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, 236, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(row);
    lv_obj_set_size(saveBtn, 100, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT, 0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onRenameSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(saveBtn, _onRenameKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* sl = lv_label_create(saveBtn);
    lv_label_set_text(sl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(sl, theme::BG, 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_12, 0);
    lv_obj_center(sl);

    lv_obj_t* cancelBtn = lv_btn_create(row);
    lv_obj_set_size(cancelBtn, 100, 26);
    lv_obj_set_style_bg_color(cancelBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(cancelBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(cancelBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(cancelBtn, 1, 0);
    lv_obj_set_style_radius(cancelBtn, 4, 0);
    lv_obj_set_style_shadow_width(cancelBtn, 0, 0);
    lv_obj_add_event_cb(cancelBtn, _onRenameCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(cancelBtn, _onRenameKey,    LV_EVENT_KEY,     nullptr);
    lv_obj_t* cl = lv_label_create(cancelBtn);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE " Cancel");
    lv_obj_set_style_text_color(cl, theme::TEXT, 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
    lv_obj_center(cl);

    // Only the textarea joins the group — the buttons stay touch-only. Fewer
    // group members means less to unwind when the modal is torn down, and
    // Enter on a one-line textarea fires LV_EVENT_READY, which commits.
    lv_obj_add_event_cb(ta, _onRenameSave, LV_EVENT_READY, nullptr);
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, ta);
        lv_group_focus_obj(ta);
    }
}

// ── _forgetDevice() ──────────────────────────────────────────────────────────
// Drops one entry from the live scan table. The BLE callback appends to and
// indexes into that same table, so pause the scan while compacting it rather
// than racing the controller. The device reappears on its next advertisement.
void ScreenFoxhunt::_forgetDevice(int devIdx)
{
    if (devIdx < 0 || devIdx >= s_devCount) return;

    bool wasScanning = s_scanning;
    if (wasScanning && s_scan) s_scan->stop();

    OPS_LOG("Fox", "Forget %s", s_dev[devIdx].mac);
    int n = s_devCount;
    for (int i = devIdx; i < n - 1; i++) s_dev[i] = s_dev[i + 1];
    s_devCount = n - 1;

    // Indices shift, so anything holding one must be re-anchored.
    if (s_targetIdx == devIdx)      s_targetIdx = -1;
    else if (s_targetIdx > devIdx)  s_targetIdx--;
    if (s_sel >= s_devCount) s_sel = s_devCount - 1;
    if (s_sel < 0)           s_sel = 0;

    if (wasScanning && s_scan) s_scan->start(0, nullptr, false);

    _rebuildList();
}


// ── 'g' name probe — runs OFF the loop task ─────────────────────────────────
//
// WHY A TASK, not a straight call:
//   BLEClient::connect() waits on a semaphore with NO timeout, and the GAP
//   "open" event never arrives for a device that is not connectable — which is
//   most of what a foxhunt scan turns up. Calling it inline blocked the Arduino
//   loop task, and with CONFIG_ESP_TASK_WDT_TIMEOUT_S = 5 the task watchdog
//   panicked a few seconds after "QUERYING NAME..." appeared. That was the
//   crash.
//
//   So the blocking work runs on its own task while the loop keeps feeding the
//   watchdog and rendering. The task touches NO LVGL state — it only fills a
//   result buffer and flips a flag that tick() picks up.
//
//   A probe that never returns would still strand its task, so tick() also
//   enforces a wall-clock deadline and abandons the attempt. A generation
//   counter makes sure an abandoned probe finishing late cannot overwrite
//   whatever the user did in the meantime.
static void _probeTaskFn(void* arg)
{
    uint32_t myGen = (uint32_t)(uintptr_t)arg;
    char nm[24] = {};
    bool ok = _gattReadName(s_probeMac, s_probeAddrType, nm, sizeof(nm));

    // Only publish if this probe is still the current one.
    if (myGen == s_probeGen) {
        if (ok) { strncpy(s_probeName, nm, sizeof(s_probeName) - 1);
                  s_probeName[sizeof(s_probeName) - 1] = '\0'; }
        s_probeState = ok ? PROBE_OK : PROBE_FAIL;
    }
    vTaskDelete(nullptr);
}

// Called from the key handler: only records the request. tick() starts it.
void ScreenFoxhunt::_probeName(int devIdx)
{
    if (devIdx < 0 || devIdx >= s_devCount) return;
    if (s_probeState != PROBE_IDLE) return;      // one at a time

    s_probeIdx      = devIdx;
    s_probeAddrType = s_dev[devIdx].addrType;
    strncpy(s_probeMac, s_dev[devIdx].mac, sizeof(s_probeMac) - 1);
    s_probeMac[sizeof(s_probeMac) - 1] = '\0';
    s_probeName[0]  = '\0';
    s_probeState    = PROBE_REQ;

    if (_rateLbl) {
        lv_label_set_text(_rateLbl, "QUERYING NAME...");
        lv_obj_set_style_text_color(_rateLbl, theme::ACCENT, 0);
    }
    if (_hintLbl) lv_label_set_text(_hintLbl, "connecting - beeper paused");
}

// Drives the probe state machine. Called from tick(), on the loop task.
void ScreenFoxhunt::_probeTick()
{
    switch (s_probeState) {

    case PROBE_REQ: {
        _stopScan();                 // the controller cannot scan and connect at once
        s_probeStartMs = millis();
        s_probeState   = PROBE_RUN;
        uint32_t gen   = s_probeGen;
        // 8 KB: GATT service/characteristic discovery allocates a fair amount.
        if (xTaskCreate(_probeTaskFn, "foxgatt", 8192,
                        (void*)(uintptr_t)gen, 1, nullptr) != pdPASS) {
            OPS_LOG("Fox", "probe task spawn failed");
            s_probeState = PROBE_FAIL;
        }
        break;
    }

    case PROBE_RUN:
        if (millis() - s_probeStartMs > PROBE_TIMEOUT_MS) {
            OPS_LOG("Fox", "GATT name query timed out for %s", s_probeMac);
            s_probeGen++;            // orphan the task's result
            s_probeState = PROBE_FAIL;
        }
        break;

    case PROBE_OK: {
        ops::theme::sanitizeText(s_probeName);
        OPS_LOG("Fox", "GATT name for %s: '%s'", s_probeMac, s_probeName);
        if (s_probeName[0]) {
            // Persist only for stable addresses. Saving against a rotating
            // address would fill foxnames.json with entries that can never
            // match again.
            bool stable = (s_probeIdx >= 0 && s_probeIdx < s_devCount)
                          ? !_addrRotates(s_dev[s_probeIdx]) : false;
            if (stable) _aliasSet(s_probeMac, s_probeName);
            else OPS_LOG("Fox", "rotating address - name shown but not saved");
            if (s_probeIdx >= 0 && s_probeIdx < s_devCount &&
                strncmp(s_dev[s_probeIdx].mac, s_probeMac, sizeof(s_probeMac) - 1) == 0) {
                strncpy(s_dev[s_probeIdx].name, s_probeName, sizeof(s_dev[0].name) - 1);
                s_dev[s_probeIdx].name[sizeof(s_dev[0].name) - 1] = '\0';
                s_dev[s_probeIdx].isAlias = true;
            }
        }
        ops::sound::playBeep(1800, 60);
        if (_hintLbl) lv_label_set_text(_hintLbl, "name retrieved");
        s_probeState = PROBE_IDLE;
        if (isActive()) { _startScan(); _refreshHunt(); _rebuildList(); }
        break;
    }

    case PROBE_FAIL:
        ops::sound::playBeep(400, 120);
        if (_hintLbl) lv_label_set_text(_hintLbl, "no name (not connectable?)");
        s_probeState = PROBE_IDLE;
        if (isActive()) { _startScan(); _refreshHunt(); }
        break;

    default: break;
    }
}

// ── Mode switching ───────────────────────────────────────────────────────────
void ScreenFoxhunt::_enterHunt(int devIdx)
{
    if (devIdx < 0 || devIdx >= s_devCount) return;
    s_targetIdx  = devIdx;
    s_mode       = MODE_HUNT;
    s_acquired   = false;
    s_lastBeepMs = 0;

    lv_obj_add_flag(_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_hunt, LV_OBJ_FLAG_HIDDEN);

    char b[48];
    snprintf(b, sizeof(b), "%s", s_dev[devIdx].name[0] ? s_dev[devIdx].name : "(no name)");
    lv_label_set_text(_targetLbl, b);
    if (s_dev[devIdx].vendor[0])
        snprintf(b, sizeof(b), "%s  %s", s_dev[devIdx].mac, s_dev[devIdx].vendor);
    else
        snprintf(b, sizeof(b), "%s", s_dev[devIdx].mac);
    lv_label_set_text(_stateLbl, b);

    // Address kind — flagged loudly when it rotates, because rename/forget are
    // refused for those and the hunt itself will lose the target.
    {
        AddrKind k = _addrKind(s_dev[devIdx]);
        bool rot   = _addrRotates(s_dev[devIdx]);
        lv_label_set_text(_addrLbl, _addrKindText(k));
        lv_obj_set_style_text_color(_addrLbl, rot ? theme::ORANGE : theme::TEXT_MUTED, 0);
        if (_hintLbl)
            lv_label_set_text(_hintLbl, rot
                ? "g get name   bksp back   (r/f need a stable address)"
                : "r rename  g get name  f forget  bksp back");
    }

    OPS_LOG("Fox", "Hunting %s (%s)", s_dev[devIdx].mac, s_dev[devIdx].name);
    _refreshHunt();
}

void ScreenFoxhunt::_enterList()
{
    s_mode      = MODE_LIST;
    s_targetIdx = -1;
    s_acquired  = false;
    lv_obj_add_flag(_hunt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_list, LV_OBJ_FLAG_HIDDEN);
    _rebuildList();
}

// ── Hunt view refresh ────────────────────────────────────────────────────────
void ScreenFoxhunt::_refreshHunt()
{
    if (s_targetIdx < 0 || s_targetIdx >= s_devCount) return;
    const FoxDevice& d = s_dev[s_targetIdx];
    int  rssi = d.rssi;
    bool live = (millis() - d.lastSeenMs) < LOST_MS;

    char b[32];
    if (live) {
        snprintf(b, sizeof(b), "%d dBm", rssi);
        lv_label_set_text(_rssiLbl, b);
        lv_obj_set_style_text_color(_rssiLbl,
            rssi > -60 ? theme::GREEN : (rssi > -80 ? theme::ORANGE : theme::RED), 0);
        // Map -95..-30 dBm onto the bar.
        int pct = map(rssi < -95 ? -95 : (rssi > -30 ? -30 : rssi), -95, -30, 0, 100);
        lv_bar_set_value(_bar, pct, LV_ANIM_OFF);

        int iv = _beepIntervalMs(rssi);
        if (iv == 0) snprintf(b, sizeof(b), "%s - solid", _proximityWord(rssi));
        else         snprintf(b, sizeof(b), "%s - %d ms", _proximityWord(rssi), iv);
        lv_label_set_text(_rateLbl, b);
        lv_obj_set_style_text_color(_rateLbl, theme::ACCENT, 0);
    } else {
        lv_label_set_text(_rssiLbl, "--");
        lv_obj_set_style_text_color(_rssiLbl, theme::TEXT_MUTED, 0);
        lv_bar_set_value(_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(_rateLbl, "LOST - searching");
        lv_obj_set_style_text_color(_rateLbl, theme::TEXT_MUTED, 0);
    }
}

// ── tick() ───────────────────────────────────────────────────────────────────
void ScreenFoxhunt::tick()
{
    // Only the LVGL work is gated on this screen being frontmost. The beeper
    // below is NOT: the whole point of the tool is to walk around listening
    // without looking, and the screensaver would otherwise cut the audio
    // mid-hunt. stop() (home / back) is what actually ends a hunt.
    const bool visible = isActive();

    _probeTick();          // advance any in-flight GATT name query

    // Re-assert key focus after a modal was torn down asynchronously. Retried
    // each frame until it sticks, because the async delete's own refocus may
    // land after ours.
    if (s_refocusPending && visible && s_focusObj) {
        lv_group_t* g = lv_group_get_default();
        if (g && lv_group_get_focused(g) == s_focusObj) s_refocusPending = false;
        else lv_group_focus_obj(s_focusObj);
    }

    if (s_mode == MODE_LIST) {
        if (!visible) return;
        uint32_t now = millis();
        // A full rebuild is expensive and resets scroll position, so only do it
        // when the device *set* changed. Live RSSI values refresh in place.
        if (s_dirty && now - s_lastRebuildMs > 500) {
            s_dirty = false;
            s_lastRebuildMs = now;
            _rebuildList();
        } else if (now - s_lastRebuildMs > 250) {
            _refreshListValues();
            s_lastRebuildMs = now;
        }
        return;
    }

    // ── MODE_HUNT ────────────────────────────────────────────────────────────
    if (s_probeState != PROBE_IDLE) return;        // querying: scan is paused
    if (!s_scanning) return;                       // hunt ended
    if (s_targetIdx < 0 || s_targetIdx >= s_devCount) return;
    const FoxDevice& d = s_dev[s_targetIdx];
    uint32_t now  = millis();
    bool     live = (now - d.lastSeenMs) < LOST_MS;
    int      rssi = d.rssi;

    if (visible) _refreshHunt();

    if (!live) {
        if (s_acquired) {
            s_acquired = false;
            OPS_LOG("Fox", "Target lost");
        }
        return;   // silence while lost
    }

    if (!s_acquired) {
        s_acquired = true;
        // Acquisition chirp — two rising notes, so you know it is back without
        // looking. Distinct from the steady proximity beep.
        ops::sound::playBeep(1200, 60);
        OPS_LOG("Fox", "Target acquired at %d dBm", rssi);
        s_lastBeepMs = now;
        return;
    }

    int interval = _beepIntervalMs(rssi);
    int freq     = _beepFreqHz(rssi);

    if (interval == 0) {
        // Solid-tone range: re-queue back-to-back bursts so it sounds continuous.
        if (now - s_lastBeepMs >= 90) {
            ops::sound::playBeep(freq, 100);
            s_lastBeepMs = now;
        }
        return;
    }

    if (now - s_lastBeepMs >= (uint32_t)interval) {
        ops::sound::playBeep(freq, 35);
        s_lastBeepMs = now;
    }
}

// ── Input ────────────────────────────────────────────────────────────────────
void ScreenFoxhunt::navigate(int /*dx*/, int dy)
{
    if (s_mode != MODE_LIST || dy == 0) return;
    int count = s_devCount;
    if (count == 0) return;
    s_sel += (dy > 0) ? 1 : -1;
    if (s_sel < 0)      s_sel = 0;
    if (s_sel >= count) s_sel = count - 1;
    _updateHighlight();
}

void ScreenFoxhunt::confirmSelect()
{
    if (s_mode == MODE_LIST) _enterHunt(s_sel);
}

void ScreenFoxhunt::_onRowClick(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_sel = idx;
    _enterHunt(idx);
}

void ScreenFoxhunt::_onBack(lv_event_t*) { _enterList(); }

void ScreenFoxhunt::_onHome(lv_event_t*)
{
    ScreenFoxhunt::stop();
    ScreenLauncher::show();
}

void ScreenFoxhunt::_onKey(lv_event_t* e)
{
    uint32_t* key = (uint32_t*)lv_event_get_param(e);
    if (!key) return;
    uint32_t k = *key;

    if (k == LV_KEY_ESC) {
        // Backspace steps back one level: hunt -> list -> launcher.
        if (s_mode == MODE_HUNT) { _enterList(); return; }
        ScreenFoxhunt::stop();
        ScreenLauncher::show();
        return;
    }
    if (k == LV_KEY_ENTER) { confirmSelect(); return; }

    // Hunt-mode shortcuts. Rename lives on a key rather than a tap because a
    // tap is already how you start hunting, and while hunting you usually have
    // one hand on the device and the target in front of you.
    if (s_mode == MODE_HUNT && s_targetIdx >= 0) {
        // Rename and Forget both key on the MAC, so they are meaningless on an
        // address that rotates: the saved name would be orphaned within
        // minutes and the forgotten device reappears under a new address.
        // Refuse them rather than pretend they worked.
        if (k == 'r' || k == 'R' || k == 'f' || k == 'F') {
            if (_addrRotates(s_dev[s_targetIdx])) {
                if (_hintLbl)
                    lv_label_set_text(_hintLbl, "rotating address - r/f unavailable");
                ops::sound::playBeep(300, 120);
                return;
            }
        }
        if (k == 'r' || k == 'R') { _showRenameDialog(s_targetIdx); return; }
        if (k == 'g' || k == 'G') { _probeName(s_targetIdx);        return; }
        if (k == 'f' || k == 'F') {
            int idx = s_targetIdx;
            _enterList();           // leave hunt before the indices shift
            _forgetDevice(idx);
            return;
        }
    }
}

// ── _build() ─────────────────────────────────────────────────────────────────
void ScreenFoxhunt::_build()
{
    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ──────────────────────────────────────────────────────────────
    lv_obj_t* bar = lv_obj_create(_screen);
    lv_obj_set_size(bar, OPS_SCREEN_W, TOP_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_set_style_pad_ver(bar, 2, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* homeBtn = lv_btn_create(bar);
    lv_group_remove_obj(homeBtn);
    lv_obj_set_height(homeBtn, TOP_H - 6);
    lv_obj_set_style_bg_color(homeBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(homeBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(homeBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(homeBtn, 1, 0);
    lv_obj_set_style_radius(homeBtn, 4, 0);
    lv_obj_set_style_shadow_width(homeBtn, 0, 0);
    lv_obj_set_style_pad_hor(homeBtn, 5, 0);
    lv_obj_add_event_cb(homeBtn, _onHome, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    _title = lv_label_create(bar);
    // Fox emoji (U+1F98A) via the imgfont — falls back to plain text if the
    // emoji font is unavailable.
    lv_label_set_text(_title, "\xF0\x9F\xA6\x8A BT Foxhunt");
    lv_obj_set_style_text_color(_title, theme::TEXT, 0);
    lv_obj_set_style_text_font(_title,
        ops::emoji::emojiFont(&lv_font_montserrat_12), 0);
    lv_obj_set_flex_grow(_title, 1);

    // ── Device list ──────────────────────────────────────────────────────────
    _list = lv_obj_create(_screen);
    lv_obj_set_size(_list, OPS_SCREEN_W, OPS_SCREEN_H - TOP_H - FOOT_H);
    lv_obj_align(_list, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(_list, theme::BG, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_radius(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 4, 0);
    lv_obj_set_style_pad_row(_list, 3, 0);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_ACTIVE);

    // ── Hunt view ────────────────────────────────────────────────────────────
    _hunt = lv_obj_create(_screen);
    lv_obj_set_size(_hunt, OPS_SCREEN_W, OPS_SCREEN_H - TOP_H - FOOT_H);
    lv_obj_align(_hunt, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(_hunt, theme::BG, 0);
    lv_obj_set_style_border_width(_hunt, 0, 0);
    lv_obj_set_style_radius(_hunt, 0, 0);
    lv_obj_set_style_pad_all(_hunt, 8, 0);
    lv_obj_clear_flag(_hunt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_hunt, LV_OBJ_FLAG_HIDDEN);

    _rssiLbl = lv_label_create(_hunt);
    lv_label_set_text(_rssiLbl, "--");
    lv_obj_set_style_text_color(_rssiLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(_rssiLbl, &lv_font_montserrat_20, 0);
    lv_obj_align(_rssiLbl, LV_ALIGN_TOP_MID, 0, 2);

    _bar = lv_bar_create(_hunt);
    lv_obj_set_size(_bar, 250, 18);
    lv_obj_align(_bar, LV_ALIGN_TOP_MID, 0, 42);
    lv_bar_set_range(_bar, 0, 100);
    lv_bar_set_value(_bar, 0, LV_ANIM_OFF);
    lv_group_remove_obj(_bar);
    lv_obj_set_style_bg_color(_bar, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(_bar, theme::ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(_bar, 4, 0);
    lv_obj_set_style_radius(_bar, 4, LV_PART_INDICATOR);

    _rateLbl = lv_label_create(_hunt);
    lv_label_set_text(_rateLbl, "");
    lv_obj_set_style_text_color(_rateLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(_rateLbl, &lv_font_montserrat_14, 0);
    lv_obj_align(_rateLbl, LV_ALIGN_TOP_MID, 0, 68);

    _targetLbl = lv_label_create(_hunt);
    lv_label_set_text(_targetLbl, "");
    lv_obj_set_style_text_color(_targetLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(_targetLbl, theme::bodyFont12(), 0);
    lv_obj_set_width(_targetLbl, OPS_SCREEN_W - 24);
    lv_label_set_long_mode(_targetLbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(_targetLbl, LV_ALIGN_TOP_MID, 0, 94);

    _stateLbl = lv_label_create(_hunt);
    lv_label_set_text(_stateLbl, "");
    lv_obj_set_style_text_color(_stateLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_stateLbl, &lv_font_montserrat_10, 0);
    lv_obj_align(_stateLbl, LV_ALIGN_TOP_MID, 0, 112);

    // Address-kind line: the warning that a rotating address makes rename /
    // forget meaningless has to be visible while hunting, not buried.
    _addrLbl = lv_label_create(_hunt);
    lv_label_set_text(_addrLbl, "");
    lv_obj_set_style_text_color(_addrLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_addrLbl, &lv_font_montserrat_10, 0);
    lv_obj_align(_addrLbl, LV_ALIGN_TOP_MID, 0, 128);

    lv_obj_t* backBtn = lv_btn_create(_hunt);
    lv_obj_set_size(backBtn, 120, 30);
    lv_obj_align(backBtn, LV_ALIGN_TOP_MID, 0, 146);
    lv_obj_set_style_bg_color(backBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(backBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(backBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(backBtn, 1, 0);
    lv_obj_set_style_radius(backBtn, 4, 0);
    lv_obj_set_style_shadow_width(backBtn, 0, 0);
    lv_group_remove_obj(backBtn);
    lv_obj_add_event_cb(backBtn, _onBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Devices");
    lv_obj_set_style_text_color(backLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_12, 0);
    lv_obj_center(backLbl);

    // ── Footer ───────────────────────────────────────────────────────────────
    _hintLbl = lv_label_create(_screen);
    lv_obj_set_pos(_hintLbl, 4, OPS_SCREEN_H - FOOT_H + 1);
    lv_obj_set_style_text_color(_hintLbl, lv_color_make(90, 90, 90), 0);
    lv_obj_set_style_text_font(_hintLbl, &lv_font_montserrat_10, 0);
    lv_label_set_text(_hintLbl, "scanning...");

    // Focus target for trackball/keyboard routing.
    lv_obj_t* foc = lv_obj_create(_screen);
    lv_obj_set_size(foc, 1, 1);
    lv_obj_set_pos(foc, 0, 0);
    lv_obj_set_style_bg_opa(foc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(foc, 0, 0);
    lv_obj_add_event_cb(foc, _onKey, LV_EVENT_KEY, nullptr);
    lv_group_add_obj(lv_group_get_default(), foc);
    lv_group_focus_obj(foc);
    s_focusObj = foc;
}

// ── show() ───────────────────────────────────────────────────────────────────
void ScreenFoxhunt::show()
{
    if (!_screen) _build();
    if (!_screen) return;

    s_mode      = MODE_LIST;
    s_targetIdx = -1;
    s_acquired  = false;
    lv_obj_add_flag(_hunt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_list, LV_OBJ_FLAG_HIDDEN);

    // Load saved names BEFORE scanning starts: the BLE callback consults the
    // in-RAM alias table for every device it sees, so it must be populated
    // first or the first batch of devices would come up unnamed.
    if (!s_aliasLoaded) _aliasLoad();

    _startScan();
    _rebuildList();
    lv_scr_load(_screen);
}

}}  // namespace ops::ui
