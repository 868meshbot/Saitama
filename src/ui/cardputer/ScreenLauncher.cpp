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
#include "../../utils/Log.h"
#include <stdio.h>
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

static lv_obj_t* s_list = nullptr;
static lv_obj_t* s_rows[APP_COUNT] = {};
static lv_obj_t* s_unreadDot = nullptr;    // on the Chat row
static int       s_sel = 0;

static void _updateHighlight()
{
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
        _buildGrid(_screen);
        _buildBottomBar(_screen);

        refreshClock();
        auto& b = Board::instance();
        refreshBattery(b.batteryPercent(), b.batteryCharging());
        refreshStatus(config::get().gpsMode, b.hasGPSFix(), 0);
        refreshBluetooth(config::get().bluetoothEnabled);
    }
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
    s_list = lv_obj_create(parent);
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
    int step = dy ? dy : dx;
    int n = s_sel + step;
    s_sel = n < 0 ? 0 : (n >= APP_COUNT ? APP_COUNT - 1 : n);
    _updateHighlight();
}

void ScreenLauncher::confirmSelect()
{
    if (_screen && s_rows[s_sel]) lv_event_send(s_rows[s_sel], LV_EVENT_CLICKED, nullptr);
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
