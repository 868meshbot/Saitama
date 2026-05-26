// Saitama — ScreenLauncher.cpp
// Copyright 2026 Saitama — MIT License

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
#include "ScreenFinder.h"
#include "ScreenMap.h"
#include "Theme.h"
#include "../utils/Config.h"
#include "../utils/Contacts.h"
#include "../utils/Log.h"
#include "../hardware/Board.h"
#include "../mesh/MeshService.h"

#include <time.h>
#include <cstring>
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

// ── 2-D navigation state ─────────────────────────────────────────────
static lv_obj_t* s_tiles[12]          = {};
static lv_obj_t* s_homeBtn            = nullptr;
static lv_obj_t* s_contactsUnreadDot  = nullptr;  // red dot on Contacts tile
static lv_obj_t* s_advertScreen       = nullptr;
static lv_obj_t* s_advertTimeLbl      = nullptr;
static lv_obj_t* s_advertList         = nullptr;  // scrollable repeater-response list
static lv_obj_t* s_advertModeDropdown = nullptr;  // Zero Hop / Flood selector
static uint32_t  s_advertSentAt       = 0;         // unix time of last sendAdvert(); 0 = not yet sent
static int8_t    s_selRow     = 0;       // 0-2
static int8_t    s_selCol     = 0;       // 0-3
static bool      s_homeSel    = false;   // true when Home button focused

static void _updateHighlight() {
    for (int i = 0; i < 12; i++) {
        if (s_tiles[i]) lv_obj_clear_state(s_tiles[i], LV_STATE_FOCUSED);
    }
    if (s_homeBtn) lv_obj_clear_state(s_homeBtn, LV_STATE_FOCUSED);

    if (s_homeSel) {
        if (s_homeBtn) lv_obj_add_state(s_homeBtn, LV_STATE_FOCUSED);
    } else {
        lv_obj_t* t = s_tiles[s_selRow * 4 + s_selCol];
        if (t) lv_obj_add_state(t, LV_STATE_FOCUSED);
    }
}

// ── App grid descriptors ─────────────────────────────────────────────
// Each entry has an LVGL symbol string and a display label.
// Symbols are UTF-8 glyphs from the built-in LVGL symbol font
// (automatically included with any Montserrat font size ≥ 14).
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
    { LV_SYMBOL_LIST,      "Trace"     },  // row 2
    { LV_SYMBOL_KEYBOARD,  "Terminal"  },
    { LV_SYMBOL_GPS,       "GPS"       },
    { LV_SYMBOL_WIFI,      "Signal"    },
};

// ── Grid column / row descriptors ────────────────────────────────────
// Must be static (LVGL holds a pointer to these arrays).
// LV_GRID_FR(1) = "1 fractional unit" — equal share of remaining space.
static const lv_coord_t kColDsc[] = {
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
    LV_GRID_TEMPLATE_LAST
};
static const lv_coord_t kRowDsc[] = {
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
    LV_GRID_TEMPLATE_LAST
};


// ── show() ───────────────────────────────────────────────────────────
void ScreenLauncher::show() {
    // Lazy-create: build the screen once, then just reload on re-entry.
    // LVGL keeps all objects alive; we just switch the active screen.
    if (!_screen) {
        _screen = lv_obj_create(nullptr);   // nullptr = top-level screen
        lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
        lv_obj_set_style_bg_color(_screen, theme::BG, 0);
        lv_obj_set_style_pad_all(_screen, 0, 0);
        lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

        _buildTopBar(_screen);
        _buildGrid(_screen);
        _buildBottomBar(_screen);

        // Populate dynamic content on first build
        refreshClock();
        auto& b = Board::instance();
        refreshBattery(b.batteryPercent(), b.batteryCharging());
        refreshStatus(ops::config::get().gpsMode, b.hasGPSFix(), 0);
        refreshSpeaker(ops::config::get().speakerEnabled);
    }

    // Update Contacts unread dot
    if (s_contactsUnreadDot) {
        if (ops::contacts::anyUnread())
            lv_obj_clear_flag(s_contactsUnreadDot, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_contactsUnreadDot, LV_OBJ_FLAG_HIDDEN);
    }

    lv_scr_load(_screen);
    _updateHighlight();  // restore selection highlight on every visit
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

    // Flex row: items fill left-to-right, centred vertically
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar,
        LV_FLEX_ALIGN_START,   // main axis: left-aligned
        LV_FLEX_ALIGN_CENTER,  // cross axis: vertically centred
        LV_FLEX_ALIGN_CENTER);

    // ── Helper: create a small compact button in the top bar ──────────
    // Returns the button object so the caller can adjust if needed.
    auto mkBtn = [](lv_obj_t* par, const char* text,
                    lv_color_t bg, lv_event_cb_t cb, void* ud) -> lv_obj_t*
    {
        lv_obj_t* btn = lv_btn_create(par);
        lv_group_remove_obj(btn);  // top bar is not encoder-navigable
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

    // Home button — highlighted because we ARE home; selectable by trackball
    s_homeBtn = mkBtn(bar, LV_SYMBOL_HOME, theme::PRIMARY, nullptr, nullptr);
    lv_obj_set_style_border_color(s_homeBtn, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_homeBtn, 2, LV_STATE_FOCUSED);

    // Flex spacer — grows to push the clock all the way to the right
    lv_obj_t* spacer = lv_obj_create(bar);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);  // consume all remaining width

    // Clock label (right-aligned via spacer)
    _timeLbl = lv_label_create(bar);
    lv_label_set_text(_timeLbl, "--:--");
    lv_obj_set_style_text_color(_timeLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(_timeLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_pad_right(_timeLbl, 4, 0);
}

// ── _buildGrid() ─────────────────────────────────────────────────────
void ScreenLauncher::_buildGrid(lv_obj_t* parent) {
    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_set_size(grid, OPS_SCREEN_W, GRID_H);
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(grid, theme::BG, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 2, 0);      // outer padding
    lv_obj_set_style_pad_row(grid, 2, 0);      // gap between rows
    lv_obj_set_style_pad_column(grid, 2, 0);   // gap between columns
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    // Activate LVGL grid layout and apply 4-col / 3-row descriptors
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, kColDsc, kRowDsc);

    // Create one cell per app
    for (int i = 0; i < 12; i++) {
        int col = i % 4;
        int row = i / 4;

        lv_obj_t* cell = lv_btn_create(grid);
        s_tiles[i] = cell;
        lv_group_remove_obj(cell);  // 2D nav handles this; encoder must not tab into tiles
        lv_obj_set_style_bg_color(cell, theme::BG_CARD,  0);
        lv_obj_set_style_bg_color(cell, theme::PRIMARY,  LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(cell, theme::BG_CARD,  LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(cell, theme::ACCENT,  LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(cell, 1,            0);
        lv_obj_set_style_border_color(cell, theme::BORDER, 0);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_shadow_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 4, 0);

        // Place this cell in the correct grid column and row.
        // LV_GRID_ALIGN_STRETCH = fill the entire cell area.
        lv_obj_set_grid_cell(cell,
            LV_GRID_ALIGN_STRETCH, col, 1,   // column position + span
            LV_GRID_ALIGN_STRETCH, row, 1);  // row position + span

        // Flex column inside the cell: icon centred on top, label below
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell,
            LV_FLEX_ALIGN_CENTER,  // main axis (vertical)
            LV_FLEX_ALIGN_CENTER,  // cross axis (horizontal)
            LV_FLEX_ALIGN_CENTER);

        // Icon — uses the LVGL built-in symbol font
        lv_obj_t* icon = lv_label_create(cell);
        lv_label_set_text(icon, kApps[i].symbol);
        lv_obj_set_style_text_color(icon, theme::ACCENT, 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);

        // App name label
        lv_obj_t* lbl = lv_label_create(cell);
        lv_label_set_text(lbl, kApps[i].label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);

        // Unread dot on the Contacts tile (index 1)
        if (i == 1) {
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

        // Pass the app label string as click user-data
        lv_obj_add_event_cb(cell, _onIconClick, LV_EVENT_CLICKED,
            (void*)kApps[i].label);
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

    // Device callsign (left — fixed, no change needed after build)
    lv_obj_t* nameLbl = lv_label_create(bar);
    const char* cs = ops::config::get().callsign;
    lv_label_set_text(nameLbl, cs[0] ? cs : "Saitama");
    lv_obj_set_style_text_color(nameLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);

    // Flex spacer — pushes status icons + battery to the right
    lv_obj_t* spacer = lv_obj_create(bar);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    // Single GPS/satellite indicator — hidden when GPS disabled
    _satLbl = lv_label_create(bar);
    lv_label_set_text(_satLbl, LV_SYMBOL_GPS "--");
    lv_obj_set_style_text_color(_satLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_satLbl, &lv_font_montserrat_10, 0);
    lv_obj_add_flag(_satLbl, LV_OBJ_FLAG_HIDDEN);

    // Speaker mute/volume icon — always shown, colour reflects cfg.speakerEnabled
    _speakerLbl = lv_label_create(bar);
    lv_label_set_text(_speakerLbl, LV_SYMBOL_MUTE);
    lv_obj_set_style_text_color(_speakerLbl, theme::RED, 0);
    lv_obj_set_style_text_font(_speakerLbl, &lv_font_montserrat_10, 0);

    // LoRa radio status — updated by refreshRadio(), hidden until initialized
    _radioLbl = lv_label_create(bar);
    lv_label_set_text(_radioLbl, LV_SYMBOL_WIFI " RX");
    lv_obj_set_style_text_color(_radioLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_radioLbl, &lv_font_montserrat_10, 0);
    lv_obj_add_flag(_radioLbl, LV_OBJ_FLAG_HIDDEN);

    // Battery indicator — updated by refreshBattery()
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
    if (charging)
        snprintf(buf, sizeof(buf), "%s CHG", sym);
    else
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
        // Off — show red GPS icon
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "Off");
        col = theme::RED;
    } else if (gpsMode == 1) {
        // Intermittent — orange; show sat count when available
        if (hasFix && satellites > 0)
            snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "%d", satellites);
        else
            snprintf(buf, sizeof(buf), LV_SYMBOL_GPS "--");
        col = theme::ORANGE;
    } else {
        // On — green with fix, muted without
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

// ── refreshUnreadDot() ───────────────────────────────────────────────
void ScreenLauncher::refreshUnreadDot()
{
    if (!s_contactsUnreadDot) return;
    if (ops::contacts::anyUnread())
        lv_obj_clear_flag(s_contactsUnreadDot, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_contactsUnreadDot, LV_OBJ_FLAG_HIDDEN);
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
        if (p.type != 2)                 continue;  // repeaters only
        if (s_advertSentAt == 0)         continue;  // nothing sent yet
        if (p.lastSeen < s_advertSentAt) continue;  // only post-advert responses

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
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);
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

        // ── Title bar ─────────────────────────────────────────────────
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

        // ── Info row: callsign (left) + sent time (right) ─────────────
        // y = TOP_H (28), h = 24
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

        // Spacer pushes sent-time to the right, matching top-bar pattern
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

        // ── Button row ─────────────────────────────────────────────────
        // y = TOP_H + 24 (52), h = 40
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

        // Mode dropdown: Zero Hop / Flood
        s_advertModeDropdown = lv_dropdown_create(btnRow);
        lv_obj_set_size(s_advertModeDropdown, 158, 30);
        lv_dropdown_set_options(s_advertModeDropdown, "Zero Hop\nFlood");
        lv_dropdown_set_selected(s_advertModeDropdown, 0);
        lv_obj_set_style_text_font(s_advertModeDropdown, &lv_font_montserrat_12, 0);
        lv_obj_set_style_bg_color(s_advertModeDropdown, theme::BG_CARD, 0);
        lv_obj_set_style_border_color(s_advertModeDropdown, theme::BORDER, 0);
        lv_obj_set_style_text_color(s_advertModeDropdown, theme::TEXT, 0);

        // Send button
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


        // ── Repeater response list ─────────────────────────────────────
        // y = TOP_H + 24 + 40 (92), fills remaining height (148 px)
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

    if      (strcmp(name, "Chat")     == 0) { ScreenHome::show();     return; }
    else if (strcmp(name, "Terminal") == 0) { ScreenTerminal::show(); return; }
    else if (strcmp(name, "Settings") == 0) { ScreenSettings::show(); return; }
    else if (strcmp(name, "Heard")    == 0) { ScreenHeard::show();    return; }
    else if (strcmp(name, "Contacts")  == 0) { ScreenContacts::show();  return; }
    else if (strcmp(name, "Repeaters") == 0) { ScreenRepeaters::show();    return; }
    else if (strcmp(name, "Trace")     == 0) { ScreenTrace::show();        return; }
    else if (strcmp(name, "Advertise") == 0) { _showAdvertiseScreen();    return; }
    else if (strcmp(name, "Signal")    == 0) { ScreenSignal::show();      return; }
    else if (strcmp(name, "Finder")    == 0) { ScreenFinder::show();      return; }
    else if (strcmp(name, "Map")       == 0) { ScreenMap::show();         return; }
    // Unimplemented tiles show a placeholder
    ScreenPlaceholder::show(name);
}

// ── 2-D trackball navigation ─────────────────────────────────────────
bool ScreenLauncher::isActive() {
    return _screen && (lv_scr_act() == _screen);
}

void ScreenLauncher::navigate(int dx, int dy) {
    if (!_screen) return;

    if (s_homeSel) {
        // From Home button: down returns to the grid at (0,0)
        if (dy > 0) {
            s_homeSel = false;
            s_selRow = 0;
            s_selCol = 0;
        }
        // left/right ignored — Home is the only navigable top-bar item
    } else {
        if (dy < 0 && s_selRow == 0) {
            // Up from top row → Home button
            s_homeSel = true;
        } else if (dy < 0 && s_selRow > 0) {
            s_selRow--;
        } else if (dy > 0 && s_selRow < 2) {
            s_selRow++;
        }

        if (!s_homeSel) {
            // Wrap left/right within the row
            s_selCol = (int8_t)((s_selCol + dx + 4) % 4);
        }
    }

    _updateHighlight();
}

void ScreenLauncher::confirmSelect() {
    if (!_screen) return;
    lv_obj_t* target = s_homeSel ? s_homeBtn
                                 : s_tiles[s_selRow * 4 + s_selCol];
    if (target) lv_event_send(target, LV_EVENT_CLICKED, nullptr);
}

}}  // namespace ops::ui
