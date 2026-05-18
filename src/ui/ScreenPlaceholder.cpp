// Saitama — ScreenPlaceholder.cpp
// Copyright 2026 Saitama — MIT License

#include "ScreenPlaceholder.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../utils/Log.h"
#include "../utils/Config.h"
#include "../hardware/Board.h"
#include <cstdio>
#include <cstring>
#include <cmath>

namespace ops { namespace ui {

lv_obj_t* ScreenPlaceholder::_screen = nullptr;

// ── GPS screen builder ────────────────────────────────────────────────
// Layout (320 × 212 body below 28 px top bar):
//
//  0                140  141           319
//  ┌────────────────┬─┬────────────────┐ 28
//  │   N            │ │ Fix   Yes      │
//  │  ┌──────────┐  │ │ Sats  8        │
//  │  │  ○  ○   │  │ │ Lat   51.1234  │
//  │ W│    ×    │E │ │ Lon   -1.4567  │
//  │  │ ○     ○ │  │ │ Alt   42m      │
//  │  └──────────┘  │ │ Date  2026-04  │
//  │   S            │ │ Time  12:34:56 │
//  │ Fix  8 sats    │ │                │
//  └────────────────┴─┴────────────────┘ 240

static void _buildGPSBody(lv_obj_t* screen) {
    auto& b              = Board::instance();
    const auto& cfg      = ops::config::get();

    bool gpsOff  = !cfg.gpsEnabled;
    bool hasFix  = !gpsOff && b.hasGPSFix();
    int  satCnt  = gpsOff ? 0 : (int)b.gpsSatellites();
    float lat    = hasFix ? b.gpsLat()  : 0.0f;
    float lng    = hasFix ? b.gpsLng()  : 0.0f;
    float altM   = hasFix ? b.gpsAltM() : 0.0f;
    uint16_t yr  = 0; uint8_t mo = 0, dy = 0, hr = 0, mi = 0, sc = 0;
    bool hasTime = !gpsOff && b.gpsDateTime(yr, mo, dy, hr, mi, sc);

    // ── Vertical separator ──────────────────────────────────────────
    {
        lv_obj_t* sep = lv_obj_create(screen);
        lv_obj_set_pos(sep, 140, 28);
        lv_obj_set_size(sep, 1, OPS_SCREEN_H - 28);
        lv_obj_set_style_bg_color(sep, theme::BORDER, 0);
        lv_obj_set_style_border_width(sep, 0, 0);
        lv_obj_set_style_radius(sep, 0, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    }

    // ── Skyplot (left panel x=0..139) ─────────────────────────────────
    // All positions are in screen coordinates (y=0 is top of screen).
    static constexpr int CX = 70;    // circle centre x
    static constexpr int CY = 118;   // circle centre y  (28 top-bar + 90)
    static constexpr int OR = 55;    // outer radius
    static constexpr int RING_MID  = 37;  // middle ring radius (~2/3 × OR = 60° elev)
    static constexpr int RING_INNER = 18; // inner ring radius  (~1/3 × OR = 30° elev)

    // Dark background fill circle
    {
        lv_obj_t* bg = lv_obj_create(screen);
        lv_obj_set_pos(bg, CX - OR, CY - OR);
        lv_obj_set_size(bg, OR * 2, OR * 2);
        lv_obj_set_style_radius(bg, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bg, lv_color_make(6, 22, 6), 0);  // very dark green
        lv_obj_set_style_border_color(bg, theme::ACCENT, 0);
        lv_obj_set_style_border_width(bg, 1, 0);
        lv_obj_set_style_shadow_width(bg, 0, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    }
    // 60° elevation ring
    {
        lv_obj_t* r = lv_obj_create(screen);
        lv_obj_set_pos(r, CX - RING_MID, CY - RING_MID);
        lv_obj_set_size(r, RING_MID * 2, RING_MID * 2);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(r, theme::BORDER, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_shadow_width(r, 0, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    }
    // 30° elevation ring
    {
        lv_obj_t* r = lv_obj_create(screen);
        lv_obj_set_pos(r, CX - RING_INNER, CY - RING_INNER);
        lv_obj_set_size(r, RING_INNER * 2, RING_INNER * 2);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(r, theme::BORDER, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_shadow_width(r, 0, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    }
    // Cross — horizontal
    {
        lv_obj_t* h = lv_obj_create(screen);
        lv_obj_set_pos(h, CX - OR, CY);
        lv_obj_set_size(h, OR * 2, 1);
        lv_obj_set_style_bg_color(h, theme::BORDER, 0);
        lv_obj_set_style_border_width(h, 0, 0);
        lv_obj_set_style_radius(h, 0, 0);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    }
    // Cross — vertical
    {
        lv_obj_t* v = lv_obj_create(screen);
        lv_obj_set_pos(v, CX, CY - OR);
        lv_obj_set_size(v, 1, OR * 2);
        lv_obj_set_style_bg_color(v, theme::BORDER, 0);
        lv_obj_set_style_border_width(v, 0, 0);
        lv_obj_set_style_radius(v, 0, 0);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Cardinal direction labels
    auto mkCard = [&](const char* txt, lv_coord_t x, lv_coord_t y) {
        lv_obj_t* l = lv_label_create(screen);
        lv_label_set_text(l, txt);
        lv_obj_set_pos(l, x, y);
        lv_obj_set_style_text_color(l, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    };
    mkCard("N", CX - 4,        CY - OR - 14);
    mkCard("S", CX - 4,        CY + OR + 2);
    mkCard("E", CX + OR + 3,   CY - 5);
    mkCard("W", CX - OR - 11,  CY - 5);

    // Satellite dots.
    // TinyGPSPlus does not expose individual azimuth/elevation, so positions
    // are deterministic representative slots spread across the skyplot.
    struct SatSlot { int16_t ang; int16_t rfrac; };  // bearing (°CW from N), radius fraction /100
    static const SatSlot kSlots[12] = {
        {  35, 82 }, {  82, 48 }, { 148, 68 }, { 200, 56 },
        { 263, 78 }, { 317, 42 }, {  67, 61 }, { 127, 30 },
        { 183, 74 }, { 244, 51 }, { 296, 87 }, {  18, 26 },
    };
    int nDots = satCnt < 12 ? satCnt : 12;
    lv_color_t dotCol = hasFix ? theme::GREEN : theme::ORANGE;
    for (int i = 0; i < nDots; i++) {
        float rad = kSlots[i].ang * (3.14159265f / 180.0f);
        int dx    = (int)(sinf(rad) * kSlots[i].rfrac * OR / 100);
        int dy    = -(int)(cosf(rad) * kSlots[i].rfrac * OR / 100);
        lv_obj_t* dot = lv_obj_create(screen);
        lv_obj_set_pos(dot, CX + dx - 4, CY + dy - 4);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, dotCol, 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_border_color(dot, lv_color_make(0, 255, 0), 0);
        lv_obj_set_style_shadow_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Fix status label centred below the circle
    {
        const char* fixTxt;
        lv_color_t  fixCol;
        if      (gpsOff)  { fixTxt = "GPS Off";      fixCol = theme::TEXT_MUTED; }
        else if (hasFix)  { fixTxt = LV_SYMBOL_GPS " Fix"; fixCol = theme::GREEN; }
        else              { fixTxt = "Searching...";  fixCol = theme::ORANGE; }

        lv_obj_t* fl = lv_label_create(screen);
        lv_label_set_text(fl, fixTxt);
        lv_obj_set_pos(fl, 0, CY + OR + 31);
        lv_obj_set_width(fl, 140);
        lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(fl, fixCol, 0);
        lv_obj_set_style_text_font(fl, &lv_font_montserrat_10, 0);
    }
    // Satellite count below fix label
    if (!gpsOff) {
        char sbuf[16];
        snprintf(sbuf, sizeof(sbuf), "%d sats", satCnt);
        lv_obj_t* sl = lv_label_create(screen);
        lv_label_set_text(sl, sbuf);
        lv_obj_set_pos(sl, 0, CY + OR + 19);
        lv_obj_set_width(sl, 140);
        lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(sl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_10, 0);
    }

    // ── Data panel (right side x=141..319) ────────────────────────────
    static constexpr int RX     = 143;                          // left edge of content
    static constexpr int RW     = OPS_SCREEN_W - RX - 2;       // 175 px
    static constexpr int ROW_H  = 27;                           // 7 rows × 27 = 189 ≤ 212

    // Helper: one alternating-striped row with left label and right value
    auto mkRow = [&](int row, const char* name, const char* val, lv_color_t valCol) {
        int ry = 28 + 3 + row * ROW_H;

        lv_obj_t* cell = lv_obj_create(screen);
        lv_obj_set_pos(cell, RX, ry);
        lv_obj_set_size(cell, RW, ROW_H - 1);
        lv_obj_set_style_bg_color(cell, (row & 1) ? theme::BG_CARD : theme::BG, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_radius(cell, 0, 0);
        lv_obj_set_style_pad_hor(cell, 4, 0);
        lv_obj_set_style_pad_ver(cell, 0, 0);
        lv_obj_set_style_shadow_width(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cell,
            LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* nl = lv_label_create(cell);
        lv_label_set_text(nl, name);
        lv_obj_set_style_text_color(nl, theme::ACCENT, 0);
        lv_obj_set_style_text_font(nl, &lv_font_montserrat_10, 0);

        lv_obj_t* vl = lv_label_create(cell);
        lv_label_set_text(vl, val);
        lv_obj_set_style_text_color(vl, valCol, 0);
        lv_obj_set_style_text_font(vl, &lv_font_montserrat_10, 0);
    };

    char buf[24];
    int  row = 0;

    // Fix
    if      (gpsOff)  mkRow(row++, "Fix", "Disabled",    theme::TEXT_MUTED);
    else if (hasFix)  mkRow(row++, "Fix", "Yes",         theme::GREEN);
    else              mkRow(row++, "Fix", "No",           theme::ORANGE);

    // Satellites
    if (!gpsOff) snprintf(buf, sizeof(buf), "%d", satCnt); else strcpy(buf, "--");
    mkRow(row++, "Sats", buf, theme::TEXT);

    // Latitude
    if (hasFix) snprintf(buf, sizeof(buf), "%.5f", (double)lat); else strcpy(buf, "--");
    mkRow(row++, "Lat", buf, theme::TEXT);

    // Longitude
    if (hasFix) snprintf(buf, sizeof(buf), "%.5f", (double)lng); else strcpy(buf, "--");
    mkRow(row++, "Lon", buf, theme::TEXT);

    // Altitude
    if (hasFix) snprintf(buf, sizeof(buf), "%.0fm", (double)altM); else strcpy(buf, "--");
    mkRow(row++, "Alt", buf, theme::TEXT);

    // Date
    if (hasTime) snprintf(buf, sizeof(buf), "%04u-%02u-%02u", yr, mo, dy);
    else         strcpy(buf, "--");
    mkRow(row++, "Date", buf, theme::TEXT);

    // Time (UTC)
    if (hasTime) snprintf(buf, sizeof(buf), "%02u:%02u:%02u", hr, mi, sc);
    else         strcpy(buf, "--");
    mkRow(row++, "Time (UTC)", buf, theme::TEXT);
}

// ── show() ────────────────────────────────────────────────────────────
void ScreenPlaceholder::show(const char* title) {
    // Always delete and rebuild so live data refreshes on every visit.
    if (_screen) {
        lv_obj_del(_screen);
        _screen = nullptr;
    }

    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ────────────────────────────────────────────────────
    lv_obj_t* topbar = lv_obj_create(_screen);
    lv_obj_set_size(topbar, OPS_SCREEN_W, 28);
    lv_obj_align(topbar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(topbar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_radius(topbar, 0, 0);
    lv_obj_set_style_pad_hor(topbar, 4, 0);
    lv_obj_set_style_pad_ver(topbar, 2, 0);
    lv_obj_set_scrollbar_mode(topbar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* homeBtn = lv_btn_create(topbar);
    lv_obj_set_size(homeBtn, 56, 22);
    lv_obj_align(homeBtn, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(homeBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(homeBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(homeBtn, 1, 0);
    lv_obj_set_style_border_color(homeBtn, theme::BORDER, 0);
    lv_obj_set_style_radius(homeBtn, 4, 0);
    lv_obj_set_style_shadow_width(homeBtn, 0, 0);
    lv_obj_add_event_cb(homeBtn, _onHomeClick, LV_EVENT_CLICKED, nullptr);
    lv_group_t* pg = lv_group_get_default();
    if (pg) {
        lv_group_add_obj(pg, homeBtn);
        lv_group_focus_obj(homeBtn);
    }
    lv_obj_add_event_cb(homeBtn, [](lv_event_t* e) {
        if (lv_event_get_key(e) == LV_KEY_ESC) ScreenLauncher::show();
    }, LV_EVENT_KEY, nullptr);

    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME " Home");
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    lv_obj_t* titleLbl = lv_label_create(topbar);
    lv_label_set_text(titleLbl, title);
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_12, 0);
    lv_obj_align(titleLbl, LV_ALIGN_CENTER, 0, 0);

    // ── Body ────────────────────────────────────────────────────────
    if (strcmp(title, "GPS") == 0) {
        _buildGPSBody(_screen);
    } else {
        lv_obj_t* icon = lv_label_create(_screen);
        lv_label_set_text(icon, LV_SYMBOL_WARNING);
        lv_obj_set_style_text_color(icon, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -12);

        lv_obj_t* msg = lv_label_create(_screen);
        lv_label_set_text(msg, "Coming soon");
        lv_obj_set_style_text_color(msg, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_12, 0);
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, 10);
    }

    lv_scr_load(_screen);
    OPS_LOG("UI", "Placeholder: %s", title);
}

void ScreenPlaceholder::_onHomeClick(lv_event_t* /*e*/) {
    ScreenLauncher::show();
}

}}  // namespace ops::ui
