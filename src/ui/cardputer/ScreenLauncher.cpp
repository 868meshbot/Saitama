// Saitama — cardputer/ScreenLauncher.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// ScreenLauncher.h for the Cardputer ADV (240x135, keyboard only): a status
// bar and one list of apps, moved through with the arrow keys (Fn + ; .)
// and opened with Enter. Built instead of ../ScreenLauncher.cpp by
// [env:cardputer-adv]. No home-page rain: the mesh rain is the screensaver.
//
//  ┌──────────────────────────────────────┐  y = 0
//  │ 14:32 ☾             ⌖7  RX  ᛒ  ▮ 82% │  status bar 16 px
//  ├──────────────────────────────────────┤
//  │▌✉ Chat                            ●  │
//  │ ☰ Contacts                           │  rows 17 px
//  │ ...                                  │
//  └──────────────────────────────────────┘  y = 135
//
// The carousel (config launcherStyle 1, the default; Settings > Launcher
// = Menu brings back the list) shows one big icon with its neighbours;
// Left/Right move, wrapping round:
//
//  ├──────────────────────────────────────┤
//  │            ╭────────╮                │
//  │    ⚙       │   ✉    │       ☰        │  neighbours 20 pt, centre 36 pt
//  │ Settings   ╰────────╯   Contacts     │
//  │                Chat                  │  name 16 pt
//  │           New messages               │  subtitle
//  │        ○ ● ○ ○ ○ ○ ○ ○ ○ ○           │  position dots
//  └──────────────────────────────────────┘

#include "../ScreenLauncher.h"
#include "../ScreenHome.h"
#include "../ScreenContacts.h"
#include "../ScreenRepeaters.h"
#include "../ScreenHeard.h"
#include "../ScreenFinder.h"
#include "../ScreenRegions.h"
#include "../ScreenSignal.h"
#include "../ScreenTerminal.h"
#include "../ScreenSettings.h"
#include "../Theme.h"
#include "../Emoji.h"
#include "ScreenGpsText.h"
#include "MeshRain.h"
#include "../../hardware/Board.h"
#include "../../utils/Config.h"
#include "../../utils/Contacts.h"
#include "../../utils/Repeaters.h"
#include "../../mesh/MeshService.h"
#include "../../utils/Log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace ops { namespace ui {

lv_obj_t* ScreenLauncher::_screen     = nullptr;
lv_obj_t* ScreenLauncher::_timeLbl    = nullptr;
lv_obj_t* ScreenLauncher::_battLbl    = nullptr;
lv_obj_t* ScreenLauncher::_satLbl     = nullptr;
lv_obj_t* ScreenLauncher::_radioLbl   = nullptr;
lv_obj_t* ScreenLauncher::_speakerLbl = nullptr;   // no speaker: unused
lv_obj_t* ScreenLauncher::_btLbl      = nullptr;

static constexpr int BAR_H = 16;
static constexpr int ROW_H = 17;

struct App { const char* icon; const char* name; void (*open)(); };

static const App kApps[] = {
    { LV_SYMBOL_ENVELOPE,  "Chat",      [] { ScreenHome::show(); } },
    { LV_SYMBOL_LIST,      "Contacts",  [] { ScreenContacts::show(); } },
    { LV_SYMBOL_WIFI,      "Repeaters", [] { ScreenRepeaters::show(); } },
    { LV_SYMBOL_EYE_OPEN,  "Heard",     [] { ScreenHeard::show(); } },
    { LV_SYMBOL_SHUFFLE,   "Finder",    [] { ScreenFinder::show(); } },
    { LV_SYMBOL_DIRECTORY, "Regions",   [] { ScreenRegions::show(); } },
    { LV_SYMBOL_CHARGE,    "Signal",    [] { ScreenSignal::show(); } },
    { LV_SYMBOL_GPS,       "GPS",       [] { ScreenGpsText::show(); } },
    { LV_SYMBOL_KEYBOARD,  "Terminal",  [] { ScreenTerminal::show(); } },
    { LV_SYMBOL_SETTINGS,  "Settings",  [] { ScreenSettings::show(); } },
};
static constexpr int APP_COUNT = sizeof(kApps) / sizeof(kApps[0]);

static lv_obj_t* s_body = nullptr;         // the list or the carousel
static int       s_builtStyle = -1;        // launcherStyle s_body was built for
static lv_obj_t* s_list = nullptr;
static lv_obj_t* s_rows[APP_COUNT] = {};
static lv_obj_t* s_unreadDot = nullptr;    // on the Chat row
static int       s_sel = 0;

// ── Carousel ────────────────────────────────────────────────────────
// Five slots: the centre, its two neighbours, and one more off each edge
// that slides in. The slots are reused and re-labelled as the selection
// moves; only their x changes during the slide. No transform zoom (it
// needs a layer buffer there is no RAM for): the centre is a bigger font.
static constexpr int CAR_SLOTS = 5;
static constexpr int CAR_MID   = CAR_SLOTS / 2;
static constexpr int CAR_STEP  = 80;       // px between slot centres
static constexpr int SLOT_W    = 76;
static constexpr int SLOT_H    = 60;
static constexpr int TILE_W    = 64;
static constexpr int TILE_H    = 58;
static constexpr int ANIM_MS   = 150;

static lv_obj_t* s_slot[CAR_SLOTS]     = {};
static lv_obj_t* s_slotIcon[CAR_SLOTS] = {};
static lv_obj_t* s_slotName[CAR_SLOTS] = {};
static lv_obj_t* s_carName = nullptr;
static lv_obj_t* s_carSub  = nullptr;
static lv_obj_t* s_dots[APP_COUNT] = {};
static int32_t   s_carOffset = 0;

static bool _carousel() { return s_builtStyle == 1; }
static int  _wrap(int i) { return ((i % APP_COUNT) + APP_COUNT) % APP_COUNT; }

static void _placeSlots()
{
    for (int k = 0; k < CAR_SLOTS; k++) {
        if (!s_slot[k]) continue;
        int cx = OPS_SCREEN_W / 2 + (k - CAR_MID) * CAR_STEP + (int)s_carOffset;
        lv_obj_set_x(s_slot[k], (lv_coord_t)(cx - SLOT_W / 2));
    }
}

// One line under the app name: something live where it is cheap to get.
static void _subtitle(int i, char* buf, size_t n)
{
    const char* name = kApps[i].name;
    if (!strcmp(name, "Chat")) {
        if (contacts::anyUnread()) snprintf(buf, n, "#FF453A " LV_SYMBOL_BULLET "# New messages");
        else                       snprintf(buf, n, "Channels and DMs");
    } else if (!strcmp(name, "Contacts")) {
        snprintf(buf, n, "%d saved", contacts::count());
    } else if (!strcmp(name, "Repeaters")) {
        snprintf(buf, n, "%d known", repeaters::count());
    } else if (!strcmp(name, "Heard")) {
        snprintf(buf, n, "%d heard this session", MeshService::instance().peerCount());
    } else if (!strcmp(name, "Finder")) {
        snprintf(buf, n, "Find a node");
    } else if (!strcmp(name, "Regions")) {
        const char* sc = config::get().scopeTag;
        if (sc[0]) snprintf(buf, n, "Scope: %s", sc);
        else       snprintf(buf, n, "No scope");
    } else if (!strcmp(name, "Signal")) {
        snprintf(buf, n, "Radio stats");
    } else if (!strcmp(name, "GPS")) {
        if (config::get().gpsMode == 0)          snprintf(buf, n, "Off");
        else if (Board::instance().hasGPSFix())  snprintf(buf, n, "Fix");
        else                                     snprintf(buf, n, "No fix");
    } else if (!strcmp(name, "Terminal")) {
        snprintf(buf, n, "Commands and log");
    } else {
        snprintf(buf, n, "Device setup");
    }
}

static void _fillCarousel()
{
    for (int k = 0; k < CAR_SLOTS; k++) {
        if (!s_slot[k]) continue;
        int i = _wrap(s_sel + k - CAR_MID);
        lv_label_set_text(s_slotIcon[k], kApps[i].icon);
        lv_label_set_text(s_slotName[k], kApps[i].name);
    }
    if (s_carName) lv_label_set_text(s_carName, kApps[s_sel].name);
    if (s_carSub) {
        char buf[40];
        _subtitle(s_sel, buf, sizeof(buf));
        lv_label_set_text(s_carSub, buf);
    }
    for (int i = 0; i < APP_COUNT; i++) {
        if (!s_dots[i]) continue;
        lv_obj_set_style_bg_color(s_dots[i], i == s_sel ? theme::ACCENT : theme::BORDER, 0);
    }
}

static void _carAnimCb(void* /*var*/, int32_t v)
{
    s_carOffset = v;
    _placeSlots();
}

static void _carouselMove(int step)
{
    s_sel = _wrap(s_sel + step);
    _fillCarousel();
    // The new centre was one step to the side: slide everything back in.
    lv_anim_del(&s_carOffset, _carAnimCb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_carOffset);
    lv_anim_set_exec_cb(&a, _carAnimCb);
    lv_anim_set_values(&a, step * CAR_STEP, 0);
    lv_anim_set_time(&a, ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void _buildCarousel(lv_obj_t* body)
{
    static constexpr int BODY_H = OPS_SCREEN_H - 16;

    // Frame behind the centre icon.
    lv_obj_t* tile = lv_obj_create(body);
    lv_obj_set_size(tile, TILE_W, TILE_H);
    lv_obj_set_pos(tile, (OPS_SCREEN_W - TILE_W) / 2, 3);
    lv_obj_set_style_bg_color(tile, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(tile, theme::ACCENT, 0);
    lv_obj_set_style_border_width(tile, 2, 0);
    lv_obj_set_style_radius(tile, 10, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int k = 0; k < CAR_SLOTS; k++) {
        bool mid = (k == CAR_MID);
        lv_obj_t* slot = lv_obj_create(body);
        s_slot[k] = slot;
        lv_obj_set_size(slot, SLOT_W, SLOT_H);
        lv_obj_set_y(slot, 2);
        lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(slot, 0, 0);
        lv_obj_set_style_pad_all(slot, 0, 0);
        lv_obj_set_style_pad_row(slot, 2, 0);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(slot, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(slot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        s_slotIcon[k] = lv_label_create(slot);
        lv_obj_set_style_text_font(s_slotIcon[k],
            mid ? &lv_font_montserrat_36 : &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_slotIcon[k], mid ? theme::ACCENT : theme::TEXT_MUTED, 0);

        s_slotName[k] = lv_label_create(slot);
        lv_obj_set_style_text_font(s_slotName[k], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(s_slotName[k], theme::TEXT_MUTED, 0);
        if (mid) lv_obj_add_flag(s_slotName[k], LV_OBJ_FLAG_HIDDEN);   // big name below
    }

    s_carName = lv_label_create(body);
    lv_obj_set_width(s_carName, OPS_SCREEN_W);
    lv_obj_set_pos(s_carName, 0, 64);
    lv_obj_set_style_text_align(s_carName, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_carName, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_carName, theme::TEXT, 0);

    s_carSub = lv_label_create(body);
    lv_obj_set_width(s_carSub, OPS_SCREEN_W);
    lv_obj_set_pos(s_carSub, 0, 84);
    lv_label_set_recolor(s_carSub, true);
    lv_obj_set_style_text_align(s_carSub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_carSub, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_carSub, theme::TEXT_MUTED, 0);

    lv_obj_t* dots = lv_obj_create(body);
    lv_obj_set_size(dots, OPS_SCREEN_W, 8);
    lv_obj_set_pos(dots, 0, BODY_H - 13);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_style_pad_all(dots, 0, 0);
    lv_obj_set_style_pad_column(dots, 6, 0);
    lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_t* d = lv_obj_create(dots);
        s_dots[i] = d;
        lv_obj_set_size(d, 6, 6);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_pad_all(d, 0, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    s_carOffset = 0;
    _placeSlots();
    _fillCarousel();
}

static void _updateHighlight()
{
    if (_carousel()) { _fillCarousel(); return; }
    for (int i = 0; i < APP_COUNT; i++) {
        if (!s_rows[i]) continue;
        if (i == s_sel) lv_obj_add_state(s_rows[i], LV_STATE_CHECKED);
        else            lv_obj_clear_state(s_rows[i], LV_STATE_CHECKED);
    }
    if (s_rows[s_sel]) lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_OFF);
}

// ── show() ──────────────────────────────────────────────────────────
void ScreenLauncher::show()
{
    if (!_screen) {
        _screen = lv_obj_create(nullptr);
        lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
        lv_obj_set_style_bg_color(_screen, theme::BG, 0);
        lv_obj_set_style_pad_all(_screen, 0, 0);
        lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

        _buildTopBar(_screen);
        _buildBottomBar(_screen);

        refreshClock();
        auto& b = Board::instance();
        refreshBattery(b.batteryPercent(), b.batteryCharging());
        refreshStatus(config::get().gpsMode, b.hasGPSFix(), 0);
        refreshBluetooth(config::get().bluetoothEnabled);
    }
    if (s_builtStyle != (int)config::get().launcherStyle)
        _buildGrid(_screen);   // first show, or Settings > Launcher changed
    refreshUnreadDot();
    lv_scr_load(_screen);
    _updateHighlight();
#if !OPS_HAS_PSRAM
    // No PSRAM: LVGL objects live in internal RAM, which BLE and the radio
    // also need. Free the big screens that are rebuilt on every visit.
    ScreenSettings::release();
    ScreenSignal::release();
#endif
    OPS_LOG("UI", "Launcher shown");
}

// ── Status bar ──────────────────────────────────────────────────────
static lv_obj_t* _barLabel(lv_obj_t* bar, const lv_font_t* font)
{
    lv_obj_t* l = lv_label_create(bar);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, theme::TEXT_MUTED, 0);
    return l;
}

void ScreenLauncher::_buildTopBar(lv_obj_t* parent)
{
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, OPS_SCREEN_W, BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 3, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 5, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    _timeLbl = _barLabel(bar, emoji::emojiFont(&lv_font_montserrat_12));
    lv_obj_set_style_text_color(_timeLbl, theme::TEXT, 0);
    lv_obj_set_flex_grow(_timeLbl, 1);

    _satLbl   = _barLabel(bar, &lv_font_montserrat_10);
    _radioLbl = _barLabel(bar, &lv_font_montserrat_10);
    lv_obj_add_flag(_radioLbl, LV_OBJ_FLAG_HIDDEN);
    _btLbl    = _barLabel(bar, &lv_font_montserrat_10);
    lv_label_set_text(_btLbl, LV_SYMBOL_BLUETOOTH);
    _battLbl  = _barLabel(bar, &lv_font_montserrat_10);
    lv_obj_set_style_text_color(_battLbl, theme::GREEN, 0);
}

// ── App list ────────────────────────────────────────────────────────
void ScreenLauncher::_buildGrid(lv_obj_t* parent)
{
    // Rebuilt when the style changes: drop the old body and its pointers.
    lv_anim_del(&s_carOffset, _carAnimCb);
    if (s_body) lv_obj_del(s_body);
    s_body = nullptr;
    s_list = nullptr;
    s_unreadDot = nullptr;
    s_carName = s_carSub = nullptr;
    for (int i = 0; i < APP_COUNT; i++) { s_rows[i] = nullptr; s_dots[i] = nullptr; }
    for (int k = 0; k < CAR_SLOTS; k++) s_slot[k] = s_slotIcon[k] = s_slotName[k] = nullptr;
    s_builtStyle = config::get().launcherStyle;

    if (_carousel()) {
        s_body = lv_obj_create(parent);
        lv_obj_set_size(s_body, OPS_SCREEN_W, OPS_SCREEN_H - BAR_H);
        lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, BAR_H);
        lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_body, 0, 0);
        lv_obj_set_style_radius(s_body, 0, 0);
        lv_obj_set_style_pad_all(s_body, 0, 0);
        lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        _buildCarousel(s_body);
        return;
    }

    s_list = lv_obj_create(parent);
    s_body = s_list;
    lv_obj_set_size(s_list, OPS_SCREEN_W, OPS_SCREEN_H - BAR_H);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, BAR_H);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_t* row = lv_btn_create(s_list);
        lv_group_remove_obj(row);   // the launcher moves its own highlight
        s_rows[i] = row;
        lv_obj_set_size(row, OPS_SCREEN_W, ROW_H);
        lv_obj_set_style_bg_color(row, theme::BG, 0);
        lv_obj_set_style_bg_color(row, theme::BG_CARD, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_width(row, 3, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(row, theme::ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, _onIconClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t* ic = lv_label_create(row);
        lv_label_set_text(ic, kApps[i].icon);
        lv_obj_set_width(ic, 14);
        lv_obj_set_style_text_color(ic, theme::ACCENT, 0);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_12, 0);

        lv_obj_t* nm = lv_label_create(row);
        lv_label_set_text(nm, kApps[i].name);
        lv_obj_set_flex_grow(nm, 1);
        lv_obj_set_style_text_color(nm, theme::TEXT, 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_12, 0);

        if (i == 0) {
            s_unreadDot = lv_label_create(row);
            lv_label_set_text(s_unreadDot, LV_SYMBOL_BULLET);
            lv_obj_set_style_text_color(s_unreadDot, theme::RED, 0);
            lv_obj_set_style_text_font(s_unreadDot, &lv_font_montserrat_12, 0);
            lv_obj_add_flag(s_unreadDot, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ScreenLauncher::_buildBottomBar(lv_obj_t* /*parent*/) {}

void ScreenLauncher::_onIconClick(lv_event_t* e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= APP_COUNT) return;
    s_sel = i;
    kApps[i].open();
}

// ── Navigation ──────────────────────────────────────────────────────
bool ScreenLauncher::isActive()
{
    return _screen && lv_scr_act() == _screen;
}

void ScreenLauncher::navigate(int dx, int dy)
{
    if (!_screen) return;
    if (_carousel()) {
        // Left/Right only; up/down do nothing here.
        if (dx) _carouselMove(dx > 0 ? 1 : -1);
        return;
    }
    int step = dy ? dy : dx;
    int n = s_sel + step;
    s_sel = n < 0 ? 0 : (n >= APP_COUNT ? APP_COUNT - 1 : n);
    _updateHighlight();
}

void ScreenLauncher::confirmSelect()
{
    if (!_screen) return;
    if (_carousel()) { kApps[s_sel].open(); return; }
    if (s_rows[s_sel]) lv_event_send(s_rows[s_sel], LV_EVENT_CLICKED, nullptr);
}

// ── Status refreshes ────────────────────────────────────────────────
void ScreenLauncher::refreshClock()
{
    if (!_timeLbl) return;
    time_t now = config::localEpoch();
    if (now < 1700000000) { lv_label_set_text(_timeLbl, "--:--"); return; }
    struct tm t;
    gmtime_r(&now, &t);
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d%s", t.tm_hour, t.tm_min,
             config::nightModeActive() ? " \xF0\x9F\x8C\x99" : "");   // moon
    lv_label_set_text(_timeLbl, buf);
}

void ScreenLauncher::refreshBattery(int percent, bool charging)
{
    if (!_battLbl) return;
    const char* sym;
    lv_color_t  col;
    if (charging) {
        sym = LV_SYMBOL_CHARGE;
        col = theme::GREEN;
    } else {
        // Green above 60 %, orange 30-60 %, red below 30 % (as the T-Deck).
        if      (percent >= 75) sym = LV_SYMBOL_BATTERY_FULL;
        else if (percent >= 50) sym = LV_SYMBOL_BATTERY_3;
        else if (percent >= 25) sym = LV_SYMBOL_BATTERY_2;
        else if (percent >=  5) sym = LV_SYMBOL_BATTERY_1;
        else                    sym = LV_SYMBOL_BATTERY_EMPTY;
        col = (percent > 60) ? theme::GREEN
            : (percent >= 30) ? theme::ORANGE : theme::RED;
    }
    char buf[32];
    if (charging) snprintf(buf, sizeof(buf), "#FFD60A %s# %d%%", sym, percent);
    else          snprintf(buf, sizeof(buf), "%s %d%%", sym, percent);
    lv_label_set_recolor(_battLbl, true);
    lv_label_set_text(_battLbl, buf);
    lv_obj_set_style_text_color(_battLbl, col, 0);
}

void ScreenLauncher::refreshStatus(uint8_t gpsMode, bool hasFix, int satellites)
{
    if (!_satLbl) return;
    char buf[12];
    lv_color_t col;
    if (gpsMode == 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "Off");
        col = theme::RED;
    } else {
        if (hasFix && satellites > 0) snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "%d", satellites);
        else                          snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "--");
        col = gpsMode == 1 ? theme::ORANGE : (hasFix ? theme::GREEN : theme::ACCENT);
    }
    lv_label_set_text(_satLbl, buf);
    lv_obj_set_style_text_color(_satLbl, col, 0);
}

void ScreenLauncher::refreshRadio(bool initialized, bool active)
{
    if (!_radioLbl) return;
    if (!initialized) { lv_obj_add_flag(_radioLbl, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(_radioLbl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(_radioLbl, active ? "RX" : "Off");
    lv_obj_set_style_text_color(_radioLbl, active ? theme::ACCENT : theme::TEXT_MUTED, 0);
}

void ScreenLauncher::refreshSpeaker(bool /*enabled*/) {}

void ScreenLauncher::refreshBluetooth(bool enabled)
{
    if (!_btLbl) return;
    lv_obj_set_style_text_color(_btLbl, enabled ? lv_color_make(0, 122, 255) : theme::TEXT_MUTED, 0);
}

void ScreenLauncher::refreshUnreadDot()
{
    if (_carousel()) { if (s_carSub) _fillCarousel(); return; }
    if (!s_unreadDot) return;
    if (contacts::anyUnread()) lv_obj_clear_flag(s_unreadDot, LV_OBJ_FLAG_HIDDEN);
    else                       lv_obj_add_flag(s_unreadDot, LV_OBJ_FLAG_HIDDEN);
}

// ── Mesh activity → the mesh-rain screensaver ───────────────────────
void ScreenLauncher::noteChannelMessage(const char* sender, const char* channel,
                                        const char* /*text*/, float /*rssi*/, uint8_t /*hops*/)
{
    meshrain::noteMessage(sender, channel);
}

void ScreenLauncher::noteDirectMessage(const char* sender)
{
    meshrain::noteMessage(sender, nullptr);
}

void ScreenLauncher::noteOwnMessage(int /*chSlot*/, const char* /*dmTo*/, const char* /*text*/) {}
void ScreenLauncher::refreshRain() {}
void ScreenLauncher::onAdvertPeersUpdated() {}

}}  // namespace ops::ui
