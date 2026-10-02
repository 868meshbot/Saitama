// Saitama — ScreenLauncher.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenLauncher.h"
#include "ScreenPlaceholder.h"
#include "ScreenHome.h"
#include "ScreenTerminal.h"
#include "ScreenSettings.h"
#include "ScreenHeard.h"
#include "ScreenContacts.h"
#include "ScreenRepeaters.h"
#include "ScreenSignal.h"
#include "ScreenTrace.h"
#include "ScreenRegions.h"
#include "ScreenFinder.h"
#include "ScreenMap.h"
#include "ScreenMP3Player.h"
#include "ScreenFileManager.h"
#include "ScreenSpectrum.h"
#include "ScreenChanScan.h"
#include "ScreenSigGen.h"
#include "ScreenPower.h"
#include "ScreenZeroXZero.h"
#include "Screen2048.h"
#include "ScreenPcap.h"
#include "ScreenFoxhunt.h"
#include "ScreenPicViewer.h"
#include "Theme.h"
#include "Emoji.h"
#include "../utils/Config.h"
#include "../utils/Contacts.h"
#include "../utils/Log.h"
#include "../hardware/Board.h"
#include "../mesh/MeshService.h"

#include <time.h>
#include <cstring>
#include <strings.h>   // strcasecmp
#include <cstdio>

namespace ops { namespace ui {

// ── Layout constants ─────────────────────────────────────────────────
static constexpr int TOP_H  = 28;
static constexpr int BOT_H  = 24;
static constexpr int GRID_H = OPS_SCREEN_H - TOP_H - BOT_H;  // 188 px

// ── Static members ───────────────────────────────────────────────────
lv_obj_t* ScreenLauncher::_screen     = nullptr;
lv_obj_t* ScreenLauncher::_timeLbl    = nullptr;
lv_obj_t* ScreenLauncher::_battLbl    = nullptr;
lv_obj_t* ScreenLauncher::_satLbl     = nullptr;
lv_obj_t* ScreenLauncher::_radioLbl   = nullptr;
lv_obj_t* ScreenLauncher::_speakerLbl = nullptr;
lv_obj_t* ScreenLauncher::_btLbl      = nullptr;

// ── Home page (page 0) state ─────────────────────────────────────────
// Four app buttons over a "rain" of stations heard this session and the
// channels they talk on, with short-lived lines linking who talks to what.
static constexpr int HOME_BTNS   = 4;
static constexpr int RAIN_MAX    = 20;   // stations cycled through
static constexpr int CHAN_MAX    = 5;    // channels cycled through
static constexpr int RAIN_DROPS  = 14;   // labels falling at once
static constexpr int RAIN_TICKMS = 50;
static constexpr int HOME_BTN_H  = 44;
static constexpr int RAIN_LINKS    = 6;    // lines on screen at once
static constexpr uint32_t RAIN_LINK_MS = 15000;
// A line waits this long for the home page to be on screen (e.g. after
// posting from Chat); its RAIN_LINK_MS fade starts once it is shown.
static constexpr uint32_t RAIN_LINK_WAIT_MS = 60000;

// Fixed colours (ACCENT changes with the theme).
static const lv_color_t kChanBlue = LV_COLOR_MAKE(88, 166, 255);

enum RainKind : uint8_t {
    RK_STATION  = 0,   // heard: advert, DM or path — coloured by signal
    RK_CHATONLY = 1,   // known only from channel messages — grey
    RK_CHANNEL  = 2,   // a channel — blue
    RK_SELF     = 3,   // this node — only shown as a line end (DMs)
};

// One name in the rain list.
struct RainEntry {
    char     name[32];
    uint32_t at;        // unix time last heard / last active
    float    rssi;      // of that packet
    uint8_t  hops;      // 0 = heard directly, 0xFF = unknown
    uint8_t  kind;      // RainKind
};

struct RainDrop {
    lv_obj_t* lbl;
    int16_t   y;          // top edge, px within the rain area
    uint8_t   speed;      // px per tick
    int8_t    src;        // s_rain index at spawn, -1 = idle
    bool      isChan;     // what it shows — kept here because s_rain is
    char      name[32];   // rebuilt (and reordered) whenever peers change
};

// A line between two names, fading out over RAIN_LINK_MS.
struct RainLink {
    lv_obj_t*  line;
    lv_point_t pts[2];
    char       a[32];
    char       b[32];
    bool       bIsChan;
    lv_color_t col;
    uint32_t   createdMs;
    uint32_t   startMs;  // first shown; 0 = not on screen yet
    bool       active;
    bool       warned;   // logged a missing end already
};

static lv_obj_t*   s_homeBtns[HOME_BTNS] = {};
static lv_obj_t*   s_homeUnreadDot       = nullptr;
static int8_t      s_selHome             = 0;
static lv_obj_t*   s_rainArea            = nullptr;
static lv_obj_t*   s_rainEmpty           = nullptr;
static RainDrop    s_drops[RAIN_DROPS]   = {};
static RainLink    s_links[RAIN_LINKS]     = {};

// Rebuilt from peers + chat senders + channels: stations newest first
// [0, s_rainStations), then channels, then this node last.
static RainEntry   s_rain[RAIN_MAX + CHAN_MAX + 1];
static int         s_rainCount           = 0;
static int         s_rainStations        = 0;
static int         s_rainSpawnable       = 0;   // stations + channels
// Channel-message senders. Channel packets carry no public key, so these
// are names only, matched to heard stations by name.
static RainEntry   s_chat[RAIN_MAX];
static int         s_chatCount           = 0;
// When each channel last carried a message (by name, leading '#' stripped).
struct ChanAct { char name[32]; uint32_t at; };
static ChanAct     s_chanAct[10];
static int         s_chanActCount        = 0;
static uint32_t    s_chatSerial          = 0;
static uint32_t    s_rainChatSerial      = 0xFFFFFFFFu;
static int         s_rainNext            = 0;  // round-robin source for respawns
static uint32_t    s_rainSerial          = 0xFFFFFFFFu;
static lv_timer_t* s_rainTimer           = nullptr;

// ── Page 1 state (classic grid) ──────────────────────────────────────
static lv_obj_t* s_tiles[12]          = {};
static lv_obj_t* s_homeBtn            = nullptr;
static lv_obj_t* s_contactsUnreadDot  = nullptr;
static int8_t    s_selRow     = 0;
static int8_t    s_selCol     = 0;
static bool      s_homeSel    = false;

// ── Page 2 state (tools grid) ────────────────────────────────────────
static lv_obj_t* s_tiles2[12]          = {};  // capacity for future games
static int8_t    s_selRow2    = 0;
static int8_t    s_selCol2    = 0;

// ── Paging state ─────────────────────────────────────────────────────
static lv_obj_t* s_pageContainer  = nullptr;
static constexpr int PAGES = 3;
static lv_obj_t* s_pageDot[PAGES] = {};
static int       s_activePage     = 0;   // 0 = home, 1 = classic grid, 2 = tools

// ── Advertise screen (page 1 action) ─────────────────────────────────
static lv_obj_t* s_advertScreen       = nullptr;
static lv_obj_t* s_advertTimeLbl      = nullptr;
static lv_obj_t* s_advertList         = nullptr;
static lv_obj_t* s_advertModeDropdown = nullptr;
static uint32_t  s_advertSentAt       = 0;

// ── App grid descriptors ─────────────────────────────────────────────
struct AppItem { const char* symbol; const char* label; };

static const AppItem kApps[12] = {
    { LV_SYMBOL_ENVELOPE,  "Chat"      },  // row 0
    { LV_SYMBOL_CALL,      "Contacts"  },
    { LV_SYMBOL_LOOP,      "Repeaters" },
    { LV_SYMBOL_GPS,       "Finder"    },
    { LV_SYMBOL_AUDIO,     "Heard"     },  // row 1
    { LV_SYMBOL_IMAGE,     "Map"       },
    { LV_SYMBOL_UPLOAD,    "Advertise" },
    { LV_SYMBOL_SETTINGS,  "Settings"  },
    { LV_SYMBOL_LOOP,    "ChanScan"  },  // row 2
    { LV_SYMBOL_KEYBOARD,  "Terminal"  },
    { LV_SYMBOL_GPS,       "GPS"       },
    { LV_SYMBOL_WIFI,      "Signal"    },
};

static const AppItem kApps2[] = {
    { LV_SYMBOL_PLAY,      "MP3"      },  // row 0
    { LV_SYMBOL_SD_CARD,   "Files"    },
    { LV_SYMBOL_UP,        "Spectrum" },
    { LV_SYMBOL_LIST,      "Trace"    },
    { LV_SYMBOL_TINT,      "SigGen"   },  // row 1
    { LV_SYMBOL_BATTERY_3, "Power"    },
    { LV_SYMBOL_EDIT,      "0x0"      },
    { LV_SYMBOL_SHUFFLE,   "2048"     },
    { "\xF0\x9F\x94\x8D",   "PCAP"     },  // row 2 - magnifier emoji
    { "\xF0\x9F\xA6\x8A",   "BT Foxhunt" },  // fox emoji
    { LV_SYMBOL_IMAGE,     "Pic Viewer" },
    { LV_SYMBOL_GPS,       "Regions"  },
};
static constexpr int kApps2Count = (int)(sizeof(kApps2) / sizeof(kApps2[0]));

static const AppItem kHomeApps[HOME_BTNS] = {
    { LV_SYMBOL_ENVELOPE,  "Chat"     },
    { LV_SYMBOL_CALL,      "Contacts" },
    { LV_SYMBOL_IMAGE,     "Map"      },
    { LV_SYMBOL_SETTINGS,  "Settings" },
};

// ── Grid descriptors (shared by both pages) ──────────────────────────
static const lv_coord_t kColDsc[] = {
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
    LV_GRID_TEMPLATE_LAST
};
static const lv_coord_t kRowDsc[] = {
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
    LV_GRID_TEMPLATE_LAST
};

// ── Page helpers ─────────────────────────────────────────────────────

static void _updatePageDots()
{
    for (int i = 0; i < PAGES; i++) {
        if (!s_pageDot[i]) continue;
        lv_obj_set_style_bg_color(s_pageDot[i],
            s_activePage == i ? theme::ACCENT : lv_color_make(50, 50, 50), 0);
    }
}

static void _updateHighlight()
{
    for (int i = 0; i < 12; i++) {
        if (s_tiles[i])  lv_obj_clear_state(s_tiles[i],  LV_STATE_FOCUSED);
    }
    for (int i = 0; i < kApps2Count; i++) {
        if (s_tiles2[i]) lv_obj_clear_state(s_tiles2[i], LV_STATE_FOCUSED);
    }
    for (int i = 0; i < HOME_BTNS; i++) {
        if (s_homeBtns[i]) lv_obj_clear_state(s_homeBtns[i], LV_STATE_FOCUSED);
    }
    if (s_homeBtn) lv_obj_clear_state(s_homeBtn, LV_STATE_FOCUSED);

    if (s_activePage == 0) {
        if (s_homeBtns[s_selHome]) lv_obj_add_state(s_homeBtns[s_selHome], LV_STATE_FOCUSED);
    } else if (s_activePage == 1) {
        if (s_homeSel) {
            if (s_homeBtn) lv_obj_add_state(s_homeBtn, LV_STATE_FOCUSED);
        } else {
            lv_obj_t* t = s_tiles[s_selRow * 4 + s_selCol];
            if (t) lv_obj_add_state(t, LV_STATE_FOCUSED);
        }
    } else {
        int idx = s_selRow2 * 4 + s_selCol2;
        if (idx < kApps2Count && s_tiles2[idx]) lv_obj_add_state(s_tiles2[idx], LV_STATE_FOCUSED);
    }
}

static void _onScrollEnd(lv_event_t*)
{
    if (!s_pageContainer) return;
    lv_coord_t x = lv_obj_get_scroll_x(s_pageContainer);
    int page = (x + OPS_SCREEN_W / 2) / OPS_SCREEN_W;
    s_activePage = page < 0 ? 0 : page >= PAGES ? PAGES - 1 : page;
    _updatePageDots();
    _updateHighlight();
}

static void _showPage(int page)
{
    s_activePage = page;
    lv_obj_scroll_to_x(s_pageContainer, page * OPS_SCREEN_W, LV_ANIM_ON);
    _updatePageDots();
    _updateHighlight();
}

// ── Home page rain ───────────────────────────────────────────────────
// Stations heard this session (newest first, up to RAIN_MAX) and up to
// CHAN_MAX channels drift down the home page in staggered columns. A drop
// that reaches the buttons respawns at the top with the next name in turn.
// Stations: coloured by signal, brighter when more recent; channel-only
// senders grey; channels blue. A channel message draws a line from its sender
// to the channel; a DM, from the sender to this node; a reply ("@[Name] ..."),
// between the two users. Lines fade out over RAIN_LINK_MS.

static const char* _skipHash(const char* n) { return (n && n[0] == '#') ? n + 1 : n; }

// Inserts e into the newest-first station part of s_rain, capped at RAIN_MAX.
static void _rainInsert(const RainEntry& e)
{
    int pos = s_rainStations;
    while (pos > 0 && s_rain[pos - 1].at < e.at) pos--;
    if (pos >= RAIN_MAX) return;
    int last = s_rainStations < RAIN_MAX ? s_rainStations : RAIN_MAX - 1;
    for (int k = last; k > pos; k--) s_rain[k] = s_rain[k - 1];
    s_rain[pos] = e;
    if (s_rainStations < RAIN_MAX) s_rainStations++;
}

static uint32_t _chanActivity(const char* name)
{
    for (int i = 0; i < s_chanActCount; i++)
        if (strcasecmp(s_chanAct[i].name, _skipHash(name)) == 0) return s_chanAct[i].at;
    return 0;
}

static void _rainReloadNames()
{
    auto& mesh = ops::MeshService::instance();
    uint32_t serial = mesh.peerSerial();
    if (serial == s_rainSerial && s_chatSerial == s_rainChatSerial) return;
    s_rainSerial     = serial;
    s_rainChatSerial = s_chatSerial;

    // Stations: heard peers, refreshed by any newer channel message from the
    // same name; then channel-only senders.
    bool chatMatched[RAIN_MAX] = {};
    s_rainStations = 0;
    int n = mesh.peerCount();
    for (int i = 0; i < n; i++) {
        ops::PeerInfo p;
        if (!mesh.getPeer(i, p) || !p.name[0]) continue;
        RainEntry e{};
        strncpy(e.name, p.name, sizeof(e.name) - 1);
        e.at   = p.lastSeen;
        e.rssi = p.lastRssi;
        e.hops = p.hops;
        e.kind = RK_STATION;
        for (int c = 0; c < s_chatCount; c++) {
            if (strcmp(s_chat[c].name, e.name) != 0) continue;
            chatMatched[c] = true;
            if (s_chat[c].at > e.at) { e.at = s_chat[c].at; e.rssi = s_chat[c].rssi; e.hops = s_chat[c].hops; }
        }
        _rainInsert(e);
    }
    for (int c = 0; c < s_chatCount; c++)
        if (!chatMatched[c]) _rainInsert(s_chat[c]);
    s_rainCount = s_rainStations;

    // Channels: the configured ones, most recently active first (ties keep
    // slot order), up to CHAN_MAX.
    const auto& cfg = ops::config::get();
    int nChan = 0;
    for (int slot = 0; slot < 10; slot++) {
        // Slot 0 with no name is registered on the mesh as "Public" — match that.
        const char* cn = cfg.channels[slot].name;
        if (!cn[0]) {
            if (slot != 0) continue;
            cn = "Public";
        }
        RainEntry e{};
        snprintf(e.name, sizeof(e.name), "#%s", _skipHash(cn));
        e.at   = _chanActivity(cn);
        e.kind = RK_CHANNEL;
        int pos = nChan;
        while (pos > 0 && s_rain[s_rainStations + pos - 1].at < e.at) pos--;
        if (pos >= CHAN_MAX) continue;
        int last = nChan < CHAN_MAX ? nChan : CHAN_MAX - 1;
        for (int k = last; k > pos; k--) s_rain[s_rainStations + k] = s_rain[s_rainStations + k - 1];
        s_rain[s_rainStations + pos] = e;
        if (nChan < CHAN_MAX) nChan++;
    }
    s_rainCount     = s_rainStations + nChan;
    s_rainSpawnable = s_rainCount;

    // This node, for DM lines only — never picked by the round-robin.
    if (cfg.callsign[0]) {
        RainEntry& self = s_rain[s_rainCount++];
        memset(&self, 0, sizeof(self));
        strncpy(self.name, cfg.callsign, sizeof(self.name) - 1);
        self.kind = RK_SELF;
    }
    if (s_rainNext >= s_rainSpawnable) s_rainNext = 0;
}

// Shows entry src on drop i, starting at y.
static void _rainSpawnEntry(int i, int src, int16_t y)
{
    RainDrop& d = s_drops[i];
    const RainEntry& e = s_rain[src];
    d.src    = (int8_t)src;
    d.isChan = (e.kind == RK_CHANNEL);
    strncpy(d.name, e.name, sizeof(d.name) - 1);
    d.name[sizeof(d.name) - 1] = '\0';

    // Display copy only: strip bytes the fonts can't draw (control bytes,
    // emoji modifiers, unknown flags) — d.name stays raw for line matching.
    char shown[sizeof(e.name)];
    strncpy(shown, e.name, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = '\0';
    theme::sanitizeText(shown);
    lv_label_set_text(d.lbl, shown);

    lv_color_t col;
    lv_opa_t   opa = LV_OPA_COVER;
    switch (e.kind) {
        case RK_CHANNEL:  col = kChanBlue;         break;
        case RK_SELF:     col = theme::TEXT;       break;
        case RK_CHATONLY: col = theme::TEXT_MUTED; break;
        default:          // signal of its last packet (the bands Finder uses)
            col = (e.rssi > -80.f)  ? theme::GREEN
                : (e.rssi > -100.f) ? theme::ORANGE
                                    : theme::RED;
            break;
    }
    if (e.kind == RK_STATION || e.kind == RK_CHATONLY)   // newer = brighter
        opa = (lv_opa_t)(src < 12 ? 255 - src * 12 : 110);
    lv_obj_set_style_text_color(d.lbl, col, 0);
    lv_obj_set_style_text_opa(d.lbl, opa, 0);

    // Lanes across the width, jittered so columns don't look gridded.
    // Measure the text directly: lv_obj_update_layout() would re-lay out the
    // whole launcher (both grids) several times a second.
    lv_point_t sz;
    lv_txt_get_size(&sz, lv_label_get_text(d.lbl), theme::bodyFont12(), 0, 0,
                    LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = sz.x;
    const int lanes = 6;
    const int laneW = OPS_SCREEN_W / lanes;
    int x = (int)lv_rand(0, lanes - 1) * laneW + (int)lv_rand(0, laneW / 2) - laneW / 4;
    if (x > OPS_SCREEN_W - w - 2) x = OPS_SCREEN_W - w - 2;
    if (x < 2) x = 2;

    d.speed = (uint8_t)lv_rand(1, 3);
    d.y = y;
    lv_obj_set_pos(d.lbl, x, d.y);
    lv_obj_clear_flag(d.lbl, LV_OBJ_FLAG_HIDDEN);
}

// Starts drop i at the top (staggered above it on first fill) with the next
// name in turn. Drops beyond the number of names stay hidden.
static void _rainSpawn(int i, bool stagger)
{
    RainDrop& d = s_drops[i];
    if (i >= s_rainSpawnable) {
        d.src = -1;
        lv_obj_add_flag(d.lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int src = s_rainNext;
    s_rainNext = (s_rainNext + 1) % s_rainSpawnable;
    _rainSpawnEntry(i, src, stagger ? (int16_t)-(int)lv_rand(16, GRID_H) : (int16_t)-16);
}

// Channel names compare case-insensitively ("#hike" == "#Hike").
static bool _rainNameEq(const char* a, const char* b, bool isChan)
{
    return isChan ? strcasecmp(a, b) == 0 : strcmp(a, b) == 0;
}

// The drop currently showing this name, or -1.
static int _rainFindDrop(const char* name, bool isChan)
{
    for (int i = 0; i < RAIN_DROPS; i++)
        if (s_drops[i].src >= 0 && s_drops[i].isChan == isChan && _rainNameEq(s_drops[i].name, name, isChan))
            return i;
    return -1;
}

// Makes sure the name is falling so a line can reach it: if no drop shows
// it, the drop nearest the bottom (or an idle one) takes it, high up on screen.
static int _rainEnsureDrop(const char* name, bool isChan)
{
    int i = _rainFindDrop(name, isChan);
    if (i >= 0) return i;
    int src = -1;
    for (int k = 0; k < s_rainCount; k++) {
        bool chan = s_rain[k].kind == RK_CHANNEL;
        if (chan == isChan && _rainNameEq(s_rain[k].name, name, isChan)) { src = k; break; }
    }
    if (src < 0) return -1;   // not a name we know (e.g. a reply to someone unheard)
    int pick = -1;
    for (int k = 0; k < RAIN_DROPS; k++) {
        if (s_drops[k].src < 0) { pick = k; break; }
        if (pick < 0 || s_drops[k].y > s_drops[pick].y) pick = k;
    }
    if (pick < 0) return -1;
    _rainSpawnEntry(pick, src, (int16_t)lv_rand(4, (GRID_H - HOME_BTN_H) / 3));
    return pick;
}

static void _rainAddLink(const char* a, const char* b, bool bIsChan, lv_color_t col)
{
    if (!a[0] || !b[0] || (!bIsChan && strcmp(a, b) == 0)) return;
    // Reuse a link between the same pair, else a free slot, else the oldest.
    int slot = -1;
    for (int i = 0; i < RAIN_LINKS && slot < 0; i++)
        if (s_links[i].active && s_links[i].bIsChan == bIsChan &&
            strcmp(s_links[i].a, a) == 0 && strcmp(s_links[i].b, b) == 0) slot = i;
    for (int i = 0; i < RAIN_LINKS && slot < 0; i++)
        if (!s_links[i].active) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < RAIN_LINKS; i++)
            if (s_links[i].createdMs < s_links[slot].createdMs) slot = i;
    }
    RainLink& l = s_links[slot];
    strncpy(l.a, a, sizeof(l.a) - 1);  l.a[sizeof(l.a) - 1] = '\0';
    strncpy(l.b, b, sizeof(l.b) - 1);  l.b[sizeof(l.b) - 1] = '\0';
    l.bIsChan = bIsChan;
    l.col     = col;
    l.createdMs = millis();
    l.startMs   = 0;
    l.active    = true;
    l.warned  = false;
    OPS_LOG("Rain", "Link %s -> %s", a, b);
    if (l.line) lv_obj_set_style_line_color(l.line, col, 0);
}

static void _rainLinkPoint(int drop, lv_point_t& pt)
{
    lv_obj_t* lbl = s_drops[drop].lbl;
    pt.x = (lv_coord_t)(lv_obj_get_x(lbl) + lv_obj_get_width(lbl) / 2);
    pt.y = (lv_coord_t)(s_drops[drop].y + 7);
}

static void _rainUpdateLinks()
{
    uint32_t now = millis();
    for (int i = 0; i < RAIN_LINKS; i++) {
        RainLink& l = s_links[i];
        if (!l.line) continue;
        if (l.active && l.startMs == 0) {   // first time on screen: start fading
            if (now - l.createdMs > RAIN_LINK_WAIT_MS) l.active = false;
            else l.startMs = now ? now : 1;
        }
        if (!l.active || now - l.startMs >= RAIN_LINK_MS) {
            l.active = false;
            lv_obj_add_flag(l.line, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int da = _rainEnsureDrop(l.a, false);
        int db = _rainEnsureDrop(l.b, l.bIsChan);
        if (da < 0 || db < 0) {   // an end we can't show
            if (!l.warned) {
                OPS_LOG("Rain", "Link %s -> %s: no name to attach to (%s)",
                        l.a, l.b, da < 0 ? l.a : l.b);
                l.warned = true;
            }
            lv_obj_add_flag(l.line, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        _rainLinkPoint(da, l.pts[0]);
        _rainLinkPoint(db, l.pts[1]);
        lv_line_set_points(l.line, l.pts, 2);
        uint32_t left = RAIN_LINK_MS - (now - l.startMs);
        lv_obj_set_style_line_opa(l.line, (lv_opa_t)(left * 220 / RAIN_LINK_MS + 35), 0);
        lv_obj_clear_flag(l.line, LV_OBJ_FLAG_HIDDEN);
    }
}

// True while drop i is an end of a line on screen.
static bool _rainDropLinked(int i)
{
    const RainDrop& d = s_drops[i];
    if (d.src < 0) return false;
    for (int k = 0; k < RAIN_LINKS; k++) {
        const RainLink& l = s_links[k];
        if (!l.active || l.startMs == 0) continue;
        if ((!d.isChan && strcmp(l.a, d.name) == 0) ||
            (d.isChan == l.bIsChan && _rainNameEq(l.b, d.name, d.isChan)))
            return true;
    }
    return false;
}

// Runs every RAIN_TICKMS but only moves anything while the home page is on
// screen, so it costs nothing elsewhere (or under the screensaver).
static void _rainTick(lv_timer_t*)
{
    if (!s_rainArea || !ScreenLauncher::isActive() || s_activePage != 0) return;
    _rainReloadNames();

    if (s_rainStations == 0) lv_obj_clear_flag(s_rainEmpty, LV_OBJ_FLAG_HIDDEN);
    else                     lv_obj_add_flag(s_rainEmpty, LV_OBJ_FLAG_HIDDEN);

    const int floorY = GRID_H - HOME_BTN_H;   // drops end behind the buttons
    for (int i = 0; i < RAIN_DROPS; i++) {
        RainDrop& d = s_drops[i];
        if (d.src < 0) {
            if (i < s_rainSpawnable) _rainSpawn(i, true);   // a new name arrived
            continue;
        }
        // A name with a line attached drifts slowly and waits at the bottom
        // edge rather than falling off, so the line stays readable.
        if (_rainDropLinked(i)) {
            if (d.y < floorY - 14) { d.y += 1; lv_obj_set_y(d.lbl, d.y); }
            continue;
        }
        d.y += d.speed;
        if (d.y > floorY) { _rainSpawn(i, false); continue; }
        lv_obj_set_y(d.lbl, d.y);
    }
    _rainUpdateLinks();
}

static void _buildHomePage(lv_obj_t* page, lv_event_cb_t onClick)
{
    // Rain layer: whole page, behind the buttons, never takes input.
    s_rainArea = lv_obj_create(page);
    lv_obj_set_size(s_rainArea, OPS_SCREEN_W, GRID_H);
    lv_obj_set_pos(s_rainArea, 0, 0);
    lv_obj_set_style_bg_opa(s_rainArea, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_rainArea, 0, 0);
    lv_obj_set_style_pad_all(s_rainArea, 0, 0);
    lv_obj_set_style_radius(s_rainArea, 0, 0);
    lv_obj_clear_flag(s_rainArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_rainArea, LV_OBJ_FLAG_CLICKABLE);

    // Link lines first, so they draw behind the names.
    for (int i = 0; i < RAIN_LINKS; i++) {
        lv_obj_t* ln = lv_line_create(s_rainArea);
        lv_obj_set_pos(ln, 0, 0);
        lv_obj_set_style_line_width(ln, 2, 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(ln, LV_OBJ_FLAG_HIDDEN);
        s_links[i] = RainLink{};
        s_links[i].line = ln;
    }

    for (int i = 0; i < RAIN_DROPS; i++) {
        lv_obj_t* l = lv_label_create(s_rainArea);
        lv_obj_set_style_text_font(l, theme::bodyFont12(), 0);   // names may hold emoji
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        s_drops[i] = RainDrop{};
        s_drops[i].lbl   = l;
        s_drops[i].speed = 1;
        s_drops[i].src   = -1;
    }

    s_rainEmpty = lv_label_create(s_rainArea);
    lv_label_set_text(s_rainEmpty, "Listening for stations...");
    lv_obj_set_style_text_color(s_rainEmpty, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_rainEmpty, &lv_font_montserrat_12, 0);
    lv_obj_align(s_rainEmpty, LV_ALIGN_CENTER, 0, -HOME_BTN_H / 2);

    // Button row along the bottom of the page.
    lv_obj_t* row = lv_obj_create(page);
    lv_obj_set_size(row, OPS_SCREEN_W, HOME_BTN_H);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(row, theme::BG, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_style_pad_column(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    for (int i = 0; i < HOME_BTNS; i++) {
        lv_obj_t* b = lv_btn_create(row);
        s_homeBtns[i] = b;
        lv_group_remove_obj(b);
        lv_obj_set_height(b, lv_pct(100));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_bg_color(b, theme::BG_CARD, 0);
        lv_obj_set_style_bg_color(b, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(b, theme::BG_CARD, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, theme::BORDER, 0);
        lv_obj_set_style_border_color(b, theme::ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 2, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b,
            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* icon = lv_label_create(b);
        lv_label_set_text(icon, kHomeApps[i].symbol);
        lv_obj_set_style_text_color(icon, theme::ACCENT, 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);

        lv_obj_t* lbl = lv_label_create(b);
        lv_label_set_text(lbl, kHomeApps[i].label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);

        if (strcmp(kHomeApps[i].label, "Contacts") == 0) {
            lv_obj_t* dot = lv_obj_create(b);
            lv_obj_set_size(dot, 8, 8);
            lv_obj_set_style_bg_color(dot, theme::RED, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_shadow_width(dot, 0, 0);
            lv_obj_add_flag(dot, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -1, 1);
            if (!ops::contacts::anyUnread()) lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
            s_homeUnreadDot = dot;
        }

        lv_obj_add_event_cb(b, onClick, LV_EVENT_CLICKED, (void*)kHomeApps[i].label);
    }

    if (!s_rainTimer) s_rainTimer = lv_timer_create(_rainTick, RAIN_TICKMS, nullptr);
}

// ── Message hooks (from UIScreen::tick) ─────────────────────────────

// "@[Name] ..." — the MeshCore reply prefix. Copies Name into out.
static bool _replyTarget(const char* text, char* out, size_t outMax)
{
    if (!text || text[0] != '@' || text[1] != '[') return false;
    const char* end = strchr(text + 2, ']');
    if (!end) return false;
    size_t n = (size_t)(end - (text + 2));
    if (n == 0 || n >= outMax) return false;
    memcpy(out, text + 2, n);
    out[n] = '\0';
    return true;
}

static void _noteChanActivity(const char* channel, uint32_t now)
{
    if (channel && channel[0]) {
        const char* cn = _skipHash(channel);
        int slot = -1;
        for (int i = 0; i < s_chanActCount; i++)
            if (strcasecmp(s_chanAct[i].name, cn) == 0) { slot = i; break; }
        if (slot < 0) {
            if (s_chanActCount < 10) slot = s_chanActCount++;
            else {
                slot = 0;
                for (int i = 1; i < s_chanActCount; i++)
                    if (s_chanAct[i].at < s_chanAct[slot].at) slot = i;
            }
            strncpy(s_chanAct[slot].name, cn, sizeof(s_chanAct[slot].name) - 1);
            s_chanAct[slot].name[sizeof(s_chanAct[slot].name) - 1] = '\0';
        }
        s_chanAct[slot].at = now;
        s_chatSerial++;
    }
}

void ScreenLauncher::noteChannelMessage(const char* sender, const char* channel,
                                        const char* text, float rssi, uint8_t hops)
{
    uint32_t now = (uint32_t)time(nullptr);
    _noteChanActivity(channel, now);

    if (!sender || !sender[0] || strcmp(sender, "?") == 0) return;
    if (strcmp(sender, ops::config::get().callsign) == 0) return;   // our own echo

    int slot = -1;
    for (int i = 0; i < s_chatCount; i++)
        if (strcmp(s_chat[i].name, sender) == 0) { slot = i; break; }
    if (slot < 0) {
        if (s_chatCount < RAIN_MAX) slot = s_chatCount++;
        else {   // full: reuse the longest-silent sender
            slot = 0;
            for (int i = 1; i < s_chatCount; i++)
                if (s_chat[i].at < s_chat[slot].at) slot = i;
        }
        memset(&s_chat[slot], 0, sizeof(s_chat[slot]));
        strncpy(s_chat[slot].name, sender, sizeof(s_chat[slot].name) - 1);
        s_chat[slot].kind = RK_CHATONLY;
    }
    s_chat[slot].at   = now;
    s_chat[slot].rssi = rssi;
    s_chat[slot].hops = hops;
    s_chatSerial++;

    if (channel && channel[0]) {
        char chan[33];
        snprintf(chan, sizeof(chan), "#%s", _skipHash(channel));
        _rainAddLink(sender, chan, true, kChanBlue);
    }
    char to[32];
    if (_replyTarget(text, to, sizeof(to))) _rainAddLink(sender, to, false, theme::TEXT);
}

void ScreenLauncher::noteOwnMessage(int chSlot, const char* dmTo, const char* text)
{
    const auto& cfg = ops::config::get();
    const char* me = cfg.callsign;
    if (!me[0]) return;
    if (chSlot >= 0 && chSlot < 10) {
        // Same naming as the mesh: an unnamed slot 0 is "Public".
        const char* cn = cfg.channels[chSlot].name[0] ? cfg.channels[chSlot].name
                       : (chSlot == 0 ? "Public" : nullptr);
        if (!cn) return;
        _noteChanActivity(cn, (uint32_t)time(nullptr));
        char chan[33];
        snprintf(chan, sizeof(chan), "#%s", _skipHash(cn));
        _rainAddLink(me, chan, true, kChanBlue);
        char to[32];
        if (_replyTarget(text, to, sizeof(to))) _rainAddLink(me, to, false, theme::TEXT);
    } else if (dmTo && dmTo[0]) {
        _rainAddLink(me, dmTo, false, theme::TEXT);
    }
}

void ScreenLauncher::noteDirectMessage(const char* sender)
{
    const char* me = ops::config::get().callsign;
    if (sender && sender[0] && me[0]) _rainAddLink(sender, me, false, theme::TEXT);
}

// ── show() ───────────────────────────────────────────────────────────
void ScreenLauncher::show() {
    if (!_screen) {
        _screen = lv_obj_create(nullptr);
        lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
        lv_obj_set_style_bg_color(_screen, theme::BG, 0);
        lv_obj_set_style_pad_all(_screen, 0, 0);
        lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

        _buildTopBar(_screen);
        _buildGrid(_screen);
        _buildBottomBar(_screen);

        refreshClock();
        auto& b = Board::instance();
        refreshBattery(b.batteryPercent(), b.batteryCharging());
        refreshStatus(ops::config::get().gpsMode, b.hasGPSFix(), 0);
        refreshSpeaker(ops::config::get().speakerEnabled);
        refreshBluetooth(ops::config::get().bluetoothEnabled);
    }

    refreshUnreadDot();

    // Always land on the home page — scroll snap can latch to another page
    // on first render.
    s_activePage = 0;
    s_selHome = 0;
    s_selRow = 0; s_selCol = 0; s_homeSel = false;
    if (s_pageContainer) lv_obj_scroll_to_x(s_pageContainer, 0, LV_ANIM_OFF);
    _updatePageDots();

    lv_scr_load(_screen);
    _updateHighlight();
    OPS_LOG("UI", "Launcher shown");
}

// ── _buildTopBar() ───────────────────────────────────────────────────
void ScreenLauncher::_buildTopBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, OPS_SCREEN_W, TOP_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_set_style_pad_ver(bar, 2, 0);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto mkBtn = [](lv_obj_t* par, const char* text,
                    lv_color_t bg, lv_event_cb_t cb, void* ud) -> lv_obj_t*
    {
        lv_obj_t* btn = lv_btn_create(par);
        lv_group_remove_obj(btn);
        lv_obj_set_height(btn, TOP_H - 6);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(btn, 5, 0);
        lv_obj_set_style_pad_ver(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
        return btn;
    };

    // Home: back to the home page from either grid.
    s_homeBtn = mkBtn(bar, LV_SYMBOL_HOME, theme::PRIMARY,
                      [](lv_event_t*) { s_homeSel = false; s_selHome = 0; _showPage(0); },
                      nullptr);
    lv_obj_set_style_border_color(s_homeBtn, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_homeBtn, 2, LV_STATE_FOCUSED);

    lv_obj_t* spacer = lv_obj_create(bar);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    _btLbl = lv_label_create(bar);
    lv_label_set_text(_btLbl, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(_btLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(_btLbl, theme::TEXT_MUTED, 0);  // grey = off
    lv_obj_set_style_pad_right(_btLbl, 2, 0);

    _timeLbl = lv_label_create(bar);
    lv_label_set_text(_timeLbl, "--:--");
    lv_obj_set_style_text_color(_timeLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(_timeLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_pad_right(_timeLbl, 4, 0);
}

// ── _buildGrid() ─────────────────────────────────────────────────────
// Creates a horizontally-scrollable page container and builds the three
// pages: home (rain + 4 buttons), the classic app grid, and the tools grid.
void ScreenLauncher::_buildGrid(lv_obj_t* parent) {

    // ── Page container ────────────────────────────────────────────────
    s_pageContainer = lv_obj_create(parent);
    lv_obj_set_size(s_pageContainer, OPS_SCREEN_W, GRID_H);
    lv_obj_align(s_pageContainer, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(s_pageContainer, theme::BG, 0);
    lv_obj_set_style_border_width(s_pageContainer, 0, 0);
    lv_obj_set_style_radius(s_pageContainer, 0, 0);
    lv_obj_set_style_pad_all(s_pageContainer, 0, 0);
    lv_obj_set_scroll_dir(s_pageContainer, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_pageContainer, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(s_pageContainer, LV_SCROLL_SNAP_START);
    lv_obj_add_event_cb(s_pageContainer, _onScrollEnd, LV_EVENT_SCROLL_END, nullptr);

    // ── Home page ─────────────────────────────────────────────────────
    lv_obj_t* home = lv_obj_create(s_pageContainer);
    lv_obj_set_size(home, OPS_SCREEN_W, GRID_H);
    lv_obj_set_pos(home, 0, 0);
    lv_obj_add_flag(home, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_set_style_bg_color(home, theme::BG, 0);
    lv_obj_set_style_border_width(home, 0, 0);
    lv_obj_set_style_radius(home, 0, 0);
    lv_obj_set_style_pad_all(home, 0, 0);
    lv_obj_set_scrollbar_mode(home, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_SCROLLABLE);
    _buildHomePage(home, _onIconClick);

    // ── Classic app grid ──────────────────────────────────────────────
    lv_obj_t* grid1 = lv_obj_create(s_pageContainer);
    lv_obj_set_size(grid1, OPS_SCREEN_W, GRID_H);
    lv_obj_set_pos(grid1, OPS_SCREEN_W, 0);
    lv_obj_add_flag(grid1, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_set_style_bg_color(grid1, theme::BG, 0);
    lv_obj_set_style_border_width(grid1, 0, 0);
    lv_obj_set_style_radius(grid1, 0, 0);
    lv_obj_set_style_pad_all(grid1, 2, 0);
    lv_obj_set_style_pad_row(grid1, 2, 0);
    lv_obj_set_style_pad_column(grid1, 2, 0);
    lv_obj_set_scrollbar_mode(grid1, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(grid1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(grid1, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid1, kColDsc, kRowDsc);

    for (int i = 0; i < 12; i++) {
        int col = i % 4;
        int row = i / 4;

        lv_obj_t* cell = lv_btn_create(grid1);
        s_tiles[i] = cell;
        lv_group_remove_obj(cell);
        lv_obj_set_style_bg_color(cell, theme::BG_CARD,  0);
        lv_obj_set_style_bg_color(cell, theme::PRIMARY,  LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(cell, theme::BG_CARD,  LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(cell, theme::ACCENT,  LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(cell, 1,            0);
        lv_obj_set_style_border_color(cell, theme::BORDER, 0);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_shadow_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 4, 0);
        lv_obj_set_grid_cell(cell,
            LV_GRID_ALIGN_STRETCH, col, 1,
            LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell,
            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* icon = lv_label_create(cell);
        lv_label_set_text(icon, kApps[i].symbol);
        lv_obj_set_style_text_color(icon, theme::ACCENT, 0);
        // emojiFont keeps montserrat_14 as the base (so LV_SYMBOL_* tiles are
        // unchanged) and adds the emoji imgfont as a fallback for tiles whose
        // icon is an emoji codepoint.
        lv_obj_set_style_text_font(icon,
            ops::emoji::emojiFont(&lv_font_montserrat_14), 0);

        lv_obj_t* lbl = lv_label_create(cell);
        lv_label_set_text(lbl, kApps[i].label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);

        if (i == 1) {  // Contacts unread dot
            lv_obj_t* dot = lv_obj_create(cell);
            lv_obj_set_size(dot, 8, 8);
            lv_obj_set_style_bg_color(dot, theme::RED, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_shadow_width(dot, 0, 0);
            lv_obj_add_flag(dot, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -1, 1);
            if (!ops::contacts::anyUnread())
                lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
            s_contactsUnreadDot = dot;
        }

        lv_obj_add_event_cb(cell, _onIconClick, LV_EVENT_CLICKED,
            (void*)kApps[i].label);
    }

    // ── Tools grid ────────────────────────────────────────────────────
    lv_obj_t* grid2 = lv_obj_create(s_pageContainer);
    lv_obj_set_size(grid2, OPS_SCREEN_W, GRID_H);
    lv_obj_set_pos(grid2, OPS_SCREEN_W * 2, 0);
    lv_obj_add_flag(grid2, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_set_style_bg_color(grid2, theme::BG, 0);
    lv_obj_set_style_border_width(grid2, 0, 0);
    lv_obj_set_style_radius(grid2, 0, 0);
    lv_obj_set_style_pad_all(grid2, 2, 0);
    lv_obj_set_style_pad_row(grid2, 2, 0);
    lv_obj_set_style_pad_column(grid2, 2, 0);
    lv_obj_set_scrollbar_mode(grid2, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(grid2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(grid2, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid2, kColDsc, kRowDsc);

    for (int i = 0; i < kApps2Count; i++) {
        int col = i % 4;
        int row = i / 4;

        lv_obj_t* cell = lv_btn_create(grid2);
        s_tiles2[i] = cell;
        lv_group_remove_obj(cell);
        lv_obj_set_style_bg_color(cell, theme::BG_CARD,  0);
        lv_obj_set_style_bg_color(cell, theme::PRIMARY,  LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(cell, theme::BG_CARD,  LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(cell, theme::ACCENT,  LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(cell, 1,            0);
        lv_obj_set_style_border_color(cell, theme::BORDER, 0);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_shadow_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 4, 0);
        lv_obj_set_grid_cell(cell,
            LV_GRID_ALIGN_STRETCH, col, 1,
            LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell,
            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* icon = lv_label_create(cell);
        lv_label_set_text(icon, kApps2[i].symbol);
        lv_obj_set_style_text_color(icon, theme::ACCENT, 0);
        lv_obj_set_style_text_font(icon,
            ops::emoji::emojiFont(&lv_font_montserrat_14), 0);

        lv_obj_t* lbl = lv_label_create(cell);
        lv_label_set_text(lbl, kApps2[i].label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);

        lv_obj_add_event_cb(cell, _onIconClick, LV_EVENT_CLICKED,
            (void*)kApps2[i].label);
    }
}

// ── _buildBottomBar() ────────────────────────────────────────────────
void ScreenLauncher::_buildBottomBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, OPS_SCREEN_W, BOT_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_set_style_pad_ver(bar, 2, 0);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Callsign (left)
    lv_obj_t* nameLbl = lv_label_create(bar);
    const char* cs = ops::config::get().callsign;
    lv_label_set_text(nameLbl, cs[0] ? cs : "Saitama");
    lv_obj_set_style_text_color(nameLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);

    // Left spacer — pushes dots to center
    lv_obj_t* lSpacer = lv_obj_create(bar);
    lv_obj_set_size(lSpacer, 1, 1);
    lv_obj_set_style_bg_opa(lSpacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lSpacer, 0, 0);
    lv_obj_set_style_pad_all(lSpacer, 0, 0);
    lv_obj_set_flex_grow(lSpacer, 1);

    // Page indicator dots (centered)
    for (int i = 0; i < PAGES; i++) {
        s_pageDot[i] = lv_obj_create(bar);
        lv_obj_set_size(s_pageDot[i], 6, 6);
        lv_obj_set_style_radius(s_pageDot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_pageDot[i], 0, 0);
        lv_obj_set_style_shadow_width(s_pageDot[i], 0, 0);
        lv_obj_set_style_bg_opa(s_pageDot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_pageDot[i],
            i == 0 ? theme::ACCENT : lv_color_make(50, 50, 50), 0);
        lv_obj_set_style_pad_all(s_pageDot[i], 0, 0);
        if (i < PAGES - 1) lv_obj_set_style_pad_right(s_pageDot[i], 3, 0);
    }

    // Right spacer
    lv_obj_t* rSpacer = lv_obj_create(bar);
    lv_obj_set_size(rSpacer, 1, 1);
    lv_obj_set_style_bg_opa(rSpacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rSpacer, 0, 0);
    lv_obj_set_style_pad_all(rSpacer, 0, 0);
    lv_obj_set_flex_grow(rSpacer, 1);

    // GPS/satellite indicator
    _satLbl = lv_label_create(bar);
    lv_label_set_text(_satLbl, LV_SYMBOL_GPS "--");
    lv_obj_set_style_text_color(_satLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_satLbl, &lv_font_montserrat_10, 0);
    lv_obj_add_flag(_satLbl, LV_OBJ_FLAG_HIDDEN);

    // Speaker icon
    _speakerLbl = lv_label_create(bar);
    lv_label_set_text(_speakerLbl, LV_SYMBOL_MUTE);
    lv_obj_set_style_text_color(_speakerLbl, theme::RED, 0);
    lv_obj_set_style_text_font(_speakerLbl, &lv_font_montserrat_10, 0);

    // LoRa radio status
    _radioLbl = lv_label_create(bar);
    lv_label_set_text(_radioLbl, LV_SYMBOL_WIFI " RX");
    lv_obj_set_style_text_color(_radioLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_radioLbl, &lv_font_montserrat_10, 0);
    lv_obj_add_flag(_radioLbl, LV_OBJ_FLAG_HIDDEN);

    // Battery indicator
    _battLbl = lv_label_create(bar);
    lv_label_set_text(_battLbl, LV_SYMBOL_BATTERY_FULL " --%");
    lv_obj_set_style_text_color(_battLbl, theme::GREEN, 0);
    lv_obj_set_style_text_font(_battLbl, &lv_font_montserrat_10, 0);
}

// ── refreshClock() ───────────────────────────────────────────────────
void ScreenLauncher::refreshClock() {
    if (!_timeLbl) return;
    time_t now = ops::config::localEpoch();
    if (now < 1700000000UL) {
        lv_label_set_text(_timeLbl, "--:--");
        return;
    }
    struct tm t;
    gmtime_r(&now, &t);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(_timeLbl, buf);
}

// ── refreshBattery() ─────────────────────────────────────────────────
void ScreenLauncher::refreshBattery(int percent, bool charging) {
    if (!_battLbl) return;
    const char* sym;
    lv_color_t  col;
    if (charging) {
        sym = LV_SYMBOL_CHARGE;
        col = theme::GREEN;
    } else if (percent >= 75) { sym = LV_SYMBOL_BATTERY_FULL;  col = theme::GREEN;  }
    else if   (percent >= 50) { sym = LV_SYMBOL_BATTERY_3;     col = theme::GREEN;  }
    else if   (percent >= 25) { sym = LV_SYMBOL_BATTERY_2;     col = theme::ORANGE; }
    else if   (percent >=  5) { sym = LV_SYMBOL_BATTERY_1;     col = theme::ORANGE; }
    else                      { sym = LV_SYMBOL_BATTERY_EMPTY; col = theme::RED;    }
    char buf[20];
    snprintf(buf, sizeof(buf), "%s %d%%", sym, percent);
    lv_label_set_text(_battLbl, buf);
    lv_obj_set_style_text_color(_battLbl, col, 0);
}

// ── refreshStatus() ──────────────────────────────────────────────────
void ScreenLauncher::refreshStatus(uint8_t gpsMode, bool hasFix, int satellites) {
    if (!_satLbl) return;
    lv_obj_clear_flag(_satLbl, LV_OBJ_FLAG_HIDDEN);
    char buf[12];
    lv_color_t col;
    if (gpsMode == 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "Off");
        col = theme::RED;
    } else if (gpsMode == 1) {
        if (hasFix && satellites > 0)
            snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "%d", satellites);
        else
            snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "--");
        col = theme::ORANGE;
    } else {
        if (hasFix && satellites > 0)
            snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "%d", satellites);
        else
            snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "--");
        col = hasFix ? theme::GREEN : theme::TEXT_MUTED;
    }
    lv_label_set_text(_satLbl, buf);
    lv_obj_set_style_text_color(_satLbl, col, 0);
}

// ── refreshRadio() ───────────────────────────────────────────────────
void ScreenLauncher::refreshRadio(bool initialized, bool active)
{
    if (!_radioLbl) return;
    if (!initialized) {
        lv_obj_add_flag(_radioLbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(_radioLbl, LV_OBJ_FLAG_HIDDEN);
    if (active) {
        lv_label_set_text(_radioLbl, LV_SYMBOL_WIFI " RX");
        lv_obj_set_style_text_color(_radioLbl, theme::GREEN, 0);
    } else {
        lv_label_set_text(_radioLbl, LV_SYMBOL_WIFI " Off");
        lv_obj_set_style_text_color(_radioLbl, theme::TEXT_MUTED, 0);
    }
}

// ── refreshSpeaker() ─────────────────────────────────────────────────
void ScreenLauncher::refreshSpeaker(bool enabled)
{
    if (!_speakerLbl) return;
    if (enabled) {
        lv_label_set_text(_speakerLbl, LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_style_text_color(_speakerLbl, theme::GREEN, 0);
    } else {
        lv_label_set_text(_speakerLbl, LV_SYMBOL_MUTE);
        lv_obj_set_style_text_color(_speakerLbl, theme::RED, 0);
    }
}

// ── refreshBluetooth() ───────────────────────────────────────────────
void ScreenLauncher::refreshBluetooth(bool enabled)
{
    if (!_btLbl) return;
    lv_obj_set_style_text_color(_btLbl,
        enabled ? lv_color_make(0, 122, 255) : theme::TEXT_MUTED, 0);
}

// ── refreshUnreadDot() ───────────────────────────────────────────────
void ScreenLauncher::refreshUnreadDot()
{
    bool unread = ops::contacts::anyUnread();
    lv_obj_t* dots[] = { s_contactsUnreadDot, s_homeUnreadDot };
    for (lv_obj_t* d : dots) {
        if (!d) continue;
        if (unread) lv_obj_clear_flag(d, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }
}

// ── Advertise screen ─────────────────────────────────────────────────

static void _advertUpdateTime()
{
    if (!s_advertTimeLbl) return;
    time_t now = ops::config::localEpoch();
    char buf[24];
    if (now < 1700000000UL) {
        lv_label_set_text(s_advertTimeLbl, LV_SYMBOL_UPLOAD " --:--:--");
        return;
    }
    struct tm t;
    gmtime_r(&now, &t);
    snprintf(buf, sizeof(buf), LV_SYMBOL_UPLOAD " %02d:%02d:%02d",
             t.tm_hour, t.tm_min, t.tm_sec);
    lv_label_set_text(s_advertTimeLbl, buf);
}

static void _advertRebuildList()
{
    if (!s_advertList) return;
    lv_obj_clean(s_advertList);

    auto& svc = ops::MeshService::instance();
    int shown = 0;
    for (int i = 0; i < svc.peerCount(); i++) {
        ops::PeerInfo p;
        if (!svc.getPeer(i, p))          continue;
        if (p.type != 2)                 continue;
        if (s_advertSentAt == 0)         continue;
        if (p.lastSeen < s_advertSentAt) continue;

        lv_obj_t* row = lv_obj_create(s_advertList);
        lv_obj_set_size(row, lv_pct(100), 22);
        lv_obj_set_style_bg_color(row, (shown & 1) ? theme::BG_CARD : theme::BG, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, p.name[0] ? p.name : "?");
        lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, theme::bodyFont10(), 0);
        lv_obj_set_width(nameLbl, 118);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);

        char addr[12];
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X",
                 p.pubKeyPrefix[0], p.pubKeyPrefix[1],
                 p.pubKeyPrefix[2], p.pubKeyPrefix[3]);
        lv_obj_t* addrLbl = lv_label_create(row);
        lv_label_set_text(addrLbl, addr);
        lv_obj_set_style_text_color(addrLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(addrLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_width(addrLbl, 88);

        char rssiBuf[12];
        snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm", (int)p.lastRssi);
        lv_obj_t* rssiLbl = lv_label_create(row);
        lv_label_set_text(rssiLbl, rssiBuf);
        lv_color_t rc = (p.lastRssi > -80.f)  ? theme::GREEN
                      : (p.lastRssi > -100.f) ? theme::ORANGE
                                              : theme::RED;
        lv_obj_set_style_text_color(rssiLbl, rc, 0);
        lv_obj_set_style_text_font(rssiLbl, &lv_font_montserrat_10, 0);

        shown++;
    }

    if (shown == 0) {
        lv_obj_t* hint = lv_label_create(s_advertList);
        lv_label_set_text(hint, s_advertSentAt == 0
                                ? "Press Send to advertise"
                                : "No response yet");
        lv_obj_set_style_text_color(hint, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 10);
    }
}

static void _onAdvertSend(lv_event_t* /*e*/)
{
    bool flood = s_advertModeDropdown &&
                 lv_dropdown_get_selected(s_advertModeDropdown) == 1;
    s_advertSentAt = (uint32_t)time(nullptr);
    ops::MeshService::instance().sendAdvert(0, flood);
    _advertUpdateTime();
    _advertRebuildList();
}

static void _onAdvertBack(lv_event_t* /*e*/)
{
    ScreenLauncher::show();
}

static void _showAdvertiseScreen()
{
    if (!s_advertScreen) {
        s_advertScreen = lv_obj_create(nullptr);
        lv_obj_set_size(s_advertScreen, OPS_SCREEN_W, OPS_SCREEN_H);
        lv_obj_set_style_bg_color(s_advertScreen, theme::BG, 0);
        lv_obj_set_style_pad_all(s_advertScreen, 0, 0);
        lv_obj_clear_flag(s_advertScreen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* titleBar = lv_obj_create(s_advertScreen);
        lv_obj_set_size(titleBar, OPS_SCREEN_W, TOP_H);
        lv_obj_align(titleBar, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(titleBar, theme::BG_CARD, 0);
        lv_obj_set_style_border_width(titleBar, 0, 0);
        lv_obj_set_style_radius(titleBar, 0, 0);
        lv_obj_set_style_pad_hor(titleBar, 4, 0);
        lv_obj_set_style_pad_ver(titleBar, 2, 0);
        lv_obj_set_style_pad_column(titleBar, 6, 0);
        lv_obj_clear_flag(titleBar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(titleBar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(titleBar,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* homeBtn = lv_btn_create(titleBar);
        lv_group_remove_obj(homeBtn);
        lv_obj_set_height(homeBtn, TOP_H - 6);
        lv_obj_set_style_bg_color(homeBtn, theme::BG, 0);
        lv_obj_set_style_bg_color(homeBtn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(homeBtn, theme::BORDER, 0);
        lv_obj_set_style_border_width(homeBtn, 1, 0);
        lv_obj_set_style_radius(homeBtn, 4, 0);
        lv_obj_set_style_shadow_width(homeBtn, 0, 0);
        lv_obj_set_style_pad_hor(homeBtn, 5, 0);
        lv_obj_add_event_cb(homeBtn, _onAdvertBack, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* homeLbl = lv_label_create(homeBtn);
        lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
        lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
        lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
        lv_obj_center(homeLbl);

        lv_obj_t* titleLbl = lv_label_create(titleBar);
        lv_label_set_text(titleLbl, LV_SYMBOL_UPLOAD " Advertise");
        lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_14, 0);

        lv_obj_t* infoRow = lv_obj_create(s_advertScreen);
        lv_obj_set_size(infoRow, OPS_SCREEN_W, 24);
        lv_obj_align(infoRow, LV_ALIGN_TOP_LEFT, 0, TOP_H);
        lv_obj_set_style_bg_color(infoRow, theme::BG_CARD, 0);
        lv_obj_set_style_border_width(infoRow, 0, 0);
        lv_obj_set_style_radius(infoRow, 0, 0);
        lv_obj_set_style_pad_hor(infoRow, 8, 0);
        lv_obj_set_style_pad_ver(infoRow, 2, 0);
        lv_obj_clear_flag(infoRow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(infoRow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(infoRow,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        const char* cs = ops::config::get().callsign;
        lv_obj_t* csLbl = lv_label_create(infoRow);
        lv_label_set_text(csLbl, cs[0] ? cs : "Saitama");
        lv_obj_set_style_text_color(csLbl, theme::ACCENT, 0);
        lv_obj_set_style_text_font(csLbl, &lv_font_montserrat_14, 0);

        lv_obj_t* iSpacer = lv_obj_create(infoRow);
        lv_obj_set_size(iSpacer, 1, 1);
        lv_obj_set_style_bg_opa(iSpacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(iSpacer, 0, 0);
        lv_obj_set_style_pad_all(iSpacer, 0, 0);
        lv_obj_set_flex_grow(iSpacer, 1);

        s_advertTimeLbl = lv_label_create(infoRow);
        lv_label_set_text(s_advertTimeLbl, LV_SYMBOL_UPLOAD " --:--:--");
        lv_obj_set_style_text_color(s_advertTimeLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(s_advertTimeLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_pad_right(s_advertTimeLbl, 2, 0);

        lv_obj_t* btnRow = lv_obj_create(s_advertScreen);
        lv_obj_set_size(btnRow, OPS_SCREEN_W, 40);
        lv_obj_align(btnRow, LV_ALIGN_TOP_LEFT, 0, TOP_H + 24);
        lv_obj_set_style_bg_color(btnRow, theme::BG, 0);
        lv_obj_set_style_border_width(btnRow, 0, 0);
        lv_obj_set_style_radius(btnRow, 0, 0);
        lv_obj_set_style_pad_hor(btnRow, 10, 0);
        lv_obj_set_style_pad_ver(btnRow, 4, 0);
        lv_obj_set_style_pad_column(btnRow, 8, 0);
        lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btnRow,
            LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        s_advertModeDropdown = lv_dropdown_create(btnRow);
        lv_obj_set_size(s_advertModeDropdown, 158, 30);
        lv_dropdown_set_options(s_advertModeDropdown, "Zero Hop\nFlood");
        lv_dropdown_set_selected(s_advertModeDropdown, 0);
        lv_obj_set_style_text_font(s_advertModeDropdown, &lv_font_montserrat_12, 0);
        lv_obj_set_style_bg_color(s_advertModeDropdown, theme::BG_CARD, 0);
        lv_obj_set_style_border_color(s_advertModeDropdown, theme::BORDER, 0);
        lv_obj_set_style_text_color(s_advertModeDropdown, theme::TEXT, 0);

        lv_obj_t* sendBtn = lv_btn_create(btnRow);
        lv_obj_set_size(sendBtn, 112, 30);
        lv_obj_set_style_bg_color(sendBtn, theme::PRIMARY, 0);
        lv_obj_set_style_bg_color(sendBtn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(sendBtn, 6, 0);
        lv_obj_set_style_border_width(sendBtn, 0, 0);
        lv_obj_set_style_shadow_width(sendBtn, 0, 0);
        lv_obj_add_event_cb(sendBtn, _onAdvertSend, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* sendLbl = lv_label_create(sendBtn);
        lv_label_set_text(sendLbl, LV_SYMBOL_UPLOAD " Send");
        lv_obj_set_style_text_color(sendLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(sendLbl, &lv_font_montserrat_12, 0);
        lv_obj_center(sendLbl);

        static constexpr lv_coord_t LIST_Y = TOP_H + 24 + 40;
        s_advertList = lv_obj_create(s_advertScreen);
        lv_obj_set_size(s_advertList, OPS_SCREEN_W, OPS_SCREEN_H - LIST_Y);
        lv_obj_align(s_advertList, LV_ALIGN_TOP_LEFT, 0, LIST_Y);
        lv_obj_set_style_bg_color(s_advertList, theme::BG, 0);
        lv_obj_set_style_border_width(s_advertList, 0, 0);
        lv_obj_set_style_radius(s_advertList, 0, 0);
        lv_obj_set_style_pad_all(s_advertList, 0, 0);
        lv_obj_set_style_pad_row(s_advertList, 0, 0);
        lv_obj_set_flex_flow(s_advertList, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_advertList,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_scrollbar_mode(s_advertList, LV_SCROLLBAR_MODE_AUTO);
    }

    _advertRebuildList();
    lv_scr_load(s_advertScreen);
    OPS_LOG("UI", "Advertise screen shown");
}

// ── onAdvertPeersUpdated() ───────────────────────────────────────────
void ScreenLauncher::onAdvertPeersUpdated()
{
    if (!s_advertScreen || lv_scr_act() != s_advertScreen) return;
    _advertRebuildList();
}

// ── _onIconClick() ───────────────────────────────────────────────────
void ScreenLauncher::_onIconClick(lv_event_t* e) {
    const char* name = static_cast<const char*>(lv_event_get_user_data(e));
    OPS_LOG("UI", "Launch: %s", name);

    if      (strcmp(name, "Chat")      == 0) { ScreenHome::show();         return; }
    else if (strcmp(name, "Terminal")  == 0) { ScreenTerminal::show();     return; }
    else if (strcmp(name, "Settings")  == 0) { ScreenSettings::show();     return; }
    else if (strcmp(name, "Heard")     == 0) { ScreenHeard::show();        return; }
    else if (strcmp(name, "Contacts")  == 0) { ScreenContacts::show();     return; }
    else if (strcmp(name, "Repeaters") == 0) { ScreenRepeaters::show();    return; }
    else if (strcmp(name, "Trace")     == 0) { ScreenTrace::show();        return; }
    else if (strcmp(name, "Advertise") == 0) { _showAdvertiseScreen();     return; }
    else if (strcmp(name, "Signal")    == 0) { ScreenSignal::show();       return; }
    else if (strcmp(name, "Finder")    == 0) { ScreenFinder::show();       return; }
    else if (strcmp(name, "Map")       == 0) { ScreenMap::show();          return; }
    // Page 2 tools
    else if (strcmp(name, "MP3")       == 0) { ScreenMP3Player::show();    return; }
    else if (strcmp(name, "Files")     == 0) { ScreenFileManager::show();  return; }
    else if (strcmp(name, "Spectrum")  == 0) { ScreenSpectrum::show();     return; }
    else if (strcmp(name, "ChanScan")  == 0) { ScreenChanScan::show();     return; }
    else if (strcmp(name, "SigGen")    == 0) { ScreenSigGen::show();       return; }
    else if (strcmp(name, "Power")     == 0) { ScreenPower::show();          return; }
    else if (strcmp(name, "0x0")       == 0) { ScreenZeroXZero::show();  return; }
    else if (strcmp(name, "2048")      == 0) { Screen2048::show();       return; }
    else if (strcmp(name, "PCAP")      == 0) { ScreenPcap::show();       return; }
    else if (strcmp(name, "BT Foxhunt") == 0) { ScreenFoxhunt::show();   return; }
    else if (strcmp(name, "Pic Viewer") == 0) { ScreenPicViewer::show(); return; }
    else if (strcmp(name, "Regions")   == 0) { ScreenRegions::show();   return; }
    ScreenPlaceholder::show(name);
}

// ── 2-D trackball navigation ─────────────────────────────────────────
bool ScreenLauncher::isActive() {
    return _screen && (lv_scr_act() == _screen);
}

void ScreenLauncher::navigate(int dx, int dy) {
    if (!_screen) return;

    // Left/right past a page's edge moves to the neighbouring page, as a swipe does.
    if (s_activePage == 0) {
        if (dx > 0 && s_selHome == HOME_BTNS - 1) {
            s_selRow = 0; s_selCol = 0; s_homeSel = false;
            _showPage(1);
            return;
        }
        int c = s_selHome + dx;
        s_selHome = (int8_t)(c < 0 ? 0 : c >= HOME_BTNS ? HOME_BTNS - 1 : c);
        _updateHighlight();
        return;
    }

    if (s_activePage == 2) {
        if (dx < 0 && s_selCol2 == 0) {
            s_selRow = s_selRow2; s_selCol = 3; s_homeSel = false;
            _showPage(1);
            return;
        }
        // Page 2: row 0 = 4 tiles (cols 0-3), row 1 = 4 tiles (cols 0-3),
        // row 2 = PCAP, BT Foxhunt, Pic Viewer, Regions
        if (dy < 0 && s_selRow2 > 0) {
            s_selRow2--;
        } else if (dy > 0 && s_selRow2 < 2) {
            s_selRow2++;
        }
        const int rowTiles = (s_selRow2 == 2) ? kApps2Count - 8 : 4;
        int c = s_selCol2 + dx;
        s_selCol2 = (int8_t)(c < 0 ? 0 : c >= rowTiles ? rowTiles - 1 : c);
        _updateHighlight();
        return;
    }

    // Classic grid
    if (!s_homeSel && dx < 0 && s_selCol == 0) {
        s_selHome = HOME_BTNS - 1;
        _showPage(0);
        return;
    }
    if (!s_homeSel && dx > 0 && s_selCol == 3) {
        s_selRow2 = s_selRow; s_selCol2 = 0;
        _showPage(2);
        return;
    }
    if (s_homeSel) {
        if (dy > 0) {
            s_homeSel = false;
            s_selRow = 0;
            s_selCol = 0;
        }
    } else {
        if (dy < 0 && s_selRow == 0) {
            s_homeSel = true;
        } else if (dy < 0 && s_selRow > 0) {
            s_selRow--;
        } else if (dy > 0 && s_selRow < 2) {
            s_selRow++;
        }
        if (!s_homeSel) s_selCol = (int8_t)(s_selCol + dx);
    }

    _updateHighlight();
}

void ScreenLauncher::confirmSelect() {
    if (!_screen) return;

    if (s_activePage == 0) {
        if (s_homeBtns[s_selHome]) lv_event_send(s_homeBtns[s_selHome], LV_EVENT_CLICKED, nullptr);
        return;
    }
    if (s_activePage == 2) {
        int idx = s_selRow2 * 4 + s_selCol2;
        if (idx < kApps2Count && s_tiles2[idx]) lv_event_send(s_tiles2[idx], LV_EVENT_CLICKED, nullptr);
        return;
    }

    lv_obj_t* target = s_homeSel ? s_homeBtn
                                 : s_tiles[s_selRow * 4 + s_selCol];
    if (target) lv_event_send(target, LV_EVENT_CLICKED, nullptr);
}

}}  // namespace ops::ui
