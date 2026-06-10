// Saitama — ScreenRepeaters.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenRepeaters.h"
#include "ScreenLauncher.h"
#include "ScreenTerminal.h"
#include "Theme.h"
#include "../mesh/MeshService.h"
#include "../utils/Repeaters.h"
#include "../utils/Log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <time.h>
#include <Arduino.h>

namespace ops { namespace ui {

lv_obj_t* ScreenRepeaters::_screen = nullptr;

// ── Admin login flow statics ──────────────────────────────────────────
uint8_t  ScreenRepeaters::s_adminPrefix[4]    = {};
char     ScreenRepeaters::s_adminName[32]     = {};
char     ScreenRepeaters::s_adminPass[64]     = {};
bool     ScreenRepeaters::s_awaitingLoginResult = false;
uint32_t ScreenRepeaters::s_loginSentMs       = 0;
uint32_t ScreenRepeaters::s_lastStatusMs      = 0;
lv_obj_t* ScreenRepeaters::s_waitOverlay      = nullptr;

// ── Admin panel statics ───────────────────────────────────────────────
lv_obj_t* ScreenRepeaters::s_adminScreen      = nullptr;
lv_obj_t* ScreenRepeaters::s_adminRespLbl     = nullptr;
char      ScreenRepeaters::s_adminRespBuf[640] = {};

static constexpr int REP_TOP_H = 28;
static int s_pendingRepeater = -1;

// Dialog input widget pointers (valid only while a dialog is open)
static lv_obj_t* s_adminPassInput = nullptr;
static lv_obj_t* s_pathInput      = nullptr;
static lv_obj_t* s_hashSz1Btn    = nullptr;
static lv_obj_t* s_hashSz2Btn    = nullptr;
static uint8_t   s_pathHashSz    = 1;

// ── Time formatter ────────────────────────────────────────────────────
static void repFmtDateTime(uint32_t ts, char* buf, size_t len)
{
    if (!ts) { snprintf(buf, len, "--"); return; }
    time_t t   = (time_t)ts;
    time_t now = time(nullptr);
    struct tm lt, nt;
    localtime_r(&t,   &lt);
    localtime_r(&now, &nt);
    static const char* kMon[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    if (lt.tm_year == nt.tm_year && lt.tm_yday == nt.tm_yday)
        snprintf(buf, len, "%02d:%02d", lt.tm_hour, lt.tm_min);
    else
        snprintf(buf, len, "%d %s %02d:%02d",
                 lt.tm_mday, kMon[lt.tm_mon], lt.tm_hour, lt.tm_min);
}

// ── show() ───────────────────────────────────────────────────────────
void ScreenRepeaters::show()
{
    lv_obj_t* old = _screen;
    _screen = nullptr;
    _build();
    if (old) lv_obj_del(old);
}

// ── _build() ─────────────────────────────────────────────────────────
void ScreenRepeaters::_build()
{
    repeaters::reloadFromSD();
    int cnt = repeaters::count();

    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ───────────────────────────────────────────────────────
    lv_obj_t* bar = lv_obj_create(_screen);
    lv_obj_set_size(bar, OPS_SCREEN_W, REP_TOP_H);
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
    lv_obj_set_height(homeBtn, REP_TOP_H - 6);
    lv_obj_set_style_bg_color(homeBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(homeBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(homeBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(homeBtn, 1, 0);
    lv_obj_set_style_radius(homeBtn, 4, 0);
    lv_obj_set_style_shadow_width(homeBtn, 0, 0);
    lv_obj_set_style_pad_hor(homeBtn, 5, 0);
    lv_obj_add_event_cb(homeBtn, _onHomeClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    char title[30];
    snprintf(title, sizeof(title), "Repeaters (%d)", cnt);
    lv_obj_t* titleLbl = lv_label_create(bar);
    lv_label_set_text(titleLbl, title);
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

    // ── Repeater list ─────────────────────────────────────────────────
    lv_obj_t* list = lv_obj_create(_screen);
    lv_obj_set_size(list, OPS_SCREEN_W, OPS_SCREEN_H - REP_TOP_H);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, REP_TOP_H);
    lv_obj_set_style_bg_color(list, theme::BG, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (cnt == 0) {
        lv_obj_t* empty = lv_label_create(list);
        lv_label_set_text(empty, "No repeaters saved.\nStations heard as repeaters\nwill appear here.");
        lv_obj_set_style_text_color(empty, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_10, 0);
        lv_obj_set_style_pad_all(empty, 8, 0);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, OPS_SCREEN_W - 16);
    }

    // Build display order: favourites first, then the rest
    static int s_order[repeaters::CAPACITY];
    int j = 0;
    for (int i = 0; i < cnt; i++) { Repeater r; if (repeaters::get(i, r) && r.favourite)  s_order[j++] = i; }
    for (int i = 0; i < cnt; i++) { Repeater r; if (repeaters::get(i, r) && !r.favourite) s_order[j++] = i; }

    static const lv_color_t kAmber = LV_COLOR_MAKE(0xFF, 0xB3, 0x00);

    for (int vi = 0; vi < cnt; vi++) {
        int si = s_order[vi];   // storage index — passed as user_data
        Repeater r;
        if (!repeaters::get(si, r)) continue;

        char addrBuf[8];
        snprintf(addrBuf, sizeof(addrBuf), "%02X%02X%02X",
                 r.pubKeyPrefix[0], r.pubKeyPrefix[1], r.pubKeyPrefix[2]);

        char timeBuf[20];
        repFmtDateTime(r.lastSeen, timeBuf, sizeof(timeBuf));

        char rssiBuf[8];
        if (r.lastRssi != 0.0f)
            snprintf(rssiBuf, sizeof(rssiBuf), "%ddBm", (int)r.lastRssi);
        else
            snprintf(rssiBuf, sizeof(rssiBuf), "--");

        lv_color_t rssiCol;
        if      (r.lastRssi >= -80.0f)  rssiCol = theme::GREEN;
        else if (r.lastRssi >= -100.0f) rssiCol = theme::ORANGE;
        else                            rssiCol = theme::RED;

        lv_obj_t* row = lv_btn_create(list);
        lv_group_remove_obj(row);
        lv_obj_set_size(row, OPS_SCREEN_W, 28);
        lv_obj_set_style_bg_color(row, (vi & 1) ? theme::BG_CARD : theme::BG, 0);
        lv_obj_set_style_bg_color(row, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 4, 0);
        lv_obj_set_style_pad_right(row, 6, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, _onRowClick, LV_EVENT_CLICKED, (void*)(intptr_t)si);

        // Favourite dot
        lv_obj_t* favDot = lv_obj_create(row);
        lv_obj_set_size(favDot, 8, 8);
        lv_obj_set_style_radius(favDot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(favDot, 0, 0);
        lv_obj_set_style_shadow_width(favDot, 0, 0);
        lv_obj_clear_flag(favDot, LV_OBJ_FLAG_SCROLLABLE);
        if (r.favourite) {
            lv_obj_set_style_bg_color(favDot, kAmber, 0);
            lv_obj_set_style_bg_opa(favDot, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(favDot, LV_OPA_TRANSP, 0);
        }

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, r.name);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nameLbl, 108);  // reduced by 8px for fav dot
        lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, theme::bodyFont10(), 0);

        lv_obj_t* addrLbl = lv_label_create(row);
        lv_label_set_text(addrLbl, addrBuf);
        lv_obj_set_width(addrLbl, 52);
        lv_obj_set_style_text_color(addrLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(addrLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* rssiLbl = lv_label_create(row);
        lv_label_set_text(rssiLbl, rssiBuf);
        lv_obj_set_width(rssiLbl, 52);
        lv_obj_set_style_text_color(rssiLbl, rssiCol, 0);
        lv_obj_set_style_text_font(rssiLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* timeLbl = lv_label_create(row);
        lv_label_set_text(timeLbl, timeBuf);
        lv_obj_set_width(timeLbl, 74);
        lv_obj_set_style_text_color(timeLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(timeLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_align(timeLbl, LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_scr_load(_screen);
    OPS_LOG("UI", "Repeaters shown (%d)", cnt);
}

// ── _onHomeClick() ────────────────────────────────────────────────────
void ScreenRepeaters::_onHomeClick(lv_event_t* /*e*/)
{
    ScreenLauncher::show();
}

// ── _onRowClick() — show action popup ────────────────────────────────
void ScreenRepeaters::_onRowClick(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_pendingRepeater = idx;

    Repeater r;
    if (!repeaters::get(idx, r)) return;

    // ── Dim overlay ───────────────────────────────────────────────────
    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, _onPopupClose, LV_EVENT_CLICKED, overlay);

    // ── Action box (auto-height via LV_SIZE_CONTENT) ──────────────────
    lv_obj_t* box = lv_obj_create(overlay);
    lv_obj_set_width(box, 200);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(box, OPS_SCREEN_H - 16, 0);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 5, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title (repeater name + key prefix)
    char titleBuf[48];
    snprintf(titleBuf, sizeof(titleBuf), "%s  %02X%02X%02X%02X",
             r.name, r.pubKeyPrefix[0], r.pubKeyPrefix[1],
             r.pubKeyPrefix[2], r.pubKeyPrefix[3]);
    lv_obj_t* title = lv_label_create(box);
    lv_label_set_text(title, titleBuf);
    lv_obj_set_width(title, 180);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, theme::TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, 0);

    auto makeBtn = [&](const char* label, lv_color_t fg,
                       lv_color_t bg, lv_color_t border,
                       lv_event_cb_t cb)
    {
        lv_obj_t* btn = lv_btn_create(box);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 184, 26);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, border, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, overlay);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, fg, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
    };

    static const lv_color_t kAmber = LV_COLOR_MAKE(0xFF, 0xB3, 0x00);
    const char* favLabel = r.favourite ? "Remove Favourite" : "Add Favourite";
    lv_color_t  favFg    = r.favourite ? theme::TEXT_MUTED  : kAmber;
    lv_color_t  favBd    = r.favourite ? theme::BORDER      : kAmber;

    makeBtn("Admin Login",     theme::ACCENT,    theme::BG, theme::ACCENT, _onPopupAdmin);
    makeBtn(favLabel,          favFg,            theme::BG, favBd,         _onPopupFavourite);
    makeBtn("Set Path",        theme::TEXT,      theme::BG, theme::BORDER, _onPopupSetPath);
    makeBtn("Reset Path",      theme::ORANGE,    theme::BG, theme::ORANGE, _onPopupResetPath);
    makeBtn("Delete Repeater", theme::RED,       theme::BG, theme::RED,    _onPopupDelete);
    makeBtn("Close",           theme::TEXT_MUTED,theme::BG, theme::BORDER, _onPopupClose);
}

// ── _onPopupAdmin() — close popup and open admin login dialog ─────────
void ScreenRepeaters::_onPopupAdmin(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    // s_pendingRepeater is preserved — dialog will read it
    _showAdminDialog();
}

// ── _onPopupFavourite() — toggle favourite and rebuild list ───────────
void ScreenRepeaters::_onPopupFavourite(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (s_pendingRepeater >= 0) {
        Repeater r;
        if (repeaters::get(s_pendingRepeater, r))
            repeaters::setFavourite(s_pendingRepeater, !r.favourite);
    }
    lv_obj_del(overlay);
    s_pendingRepeater = -1;
    lv_async_call([](void*){ ScreenRepeaters::show(); }, nullptr);
}

// ── _onPopupSetPath() — close popup and open set-path dialog ──────────
void ScreenRepeaters::_onPopupSetPath(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    // s_pendingRepeater is preserved — dialog will read it
    _showSetPathDialog();
}

// ── _onPopupResetPath() — reset path and close popup ─────────────────
void ScreenRepeaters::_onPopupResetPath(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (s_pendingRepeater >= 0) {
        Repeater r;
        if (repeaters::get(s_pendingRepeater, r)) {
            ops::MeshService::instance().resetContactPath(r.pubKeyPrefix);
            char buf[64];
            snprintf(buf, sizeof(buf), "[repeaters] Path reset: %s", r.name);
            ScreenTerminal::appendLine(buf);
            OPS_LOG("Repeaters", "Path reset for %s", r.name);
        }
    }
    lv_obj_del(overlay);
    s_pendingRepeater = -1;
}

// ── _onPopupDelete() ─────────────────────────────────────────────────
void ScreenRepeaters::_onPopupDelete(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (s_pendingRepeater >= 0) {
        OPS_LOG("Repeaters", "Deleted repeater %d", s_pendingRepeater);
        repeaters::remove(s_pendingRepeater);
        s_pendingRepeater = -1;
    }
    lv_obj_del(overlay);
    lv_async_call([](void*){ ScreenRepeaters::show(); }, nullptr);
}

// ── _onPopupClose() ───────────────────────────────────────────────────
void ScreenRepeaters::_onPopupClose(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    s_pendingRepeater = -1;
}

// ─────────────────────────────────────────────────────────────────────
// Admin Login dialog
// ─────────────────────────────────────────────────────────────────────
void ScreenRepeaters::_showAdminDialog()
{
    Repeater r;
    if (s_pendingRepeater < 0 || !repeaters::get(s_pendingRepeater, r)) {
        s_pendingRepeater = -1;
        return;
    }

    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(overlay);
    lv_obj_set_width(box, 220);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title
    char titleBuf[48];
    snprintf(titleBuf, sizeof(titleBuf), "Admin Login: %s", r.name);
    lv_obj_t* titleLbl = lv_label_create(box);
    lv_label_set_text(titleLbl, titleBuf);
    lv_obj_set_width(titleLbl, 200);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

    lv_obj_t* hintLbl = lv_label_create(box);
    lv_label_set_text(hintLbl, "Password:");
    lv_obj_set_style_text_color(hintLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hintLbl, &lv_font_montserrat_10, 0);

    // Password textarea
    s_adminPassInput = lv_textarea_create(box);
    lv_obj_set_size(s_adminPassInput, 200, 30);
    lv_textarea_set_one_line(s_adminPassInput, true);
    lv_textarea_set_password_mode(s_adminPassInput, true);
    lv_textarea_set_placeholder_text(s_adminPassInput, "enter password");
    lv_obj_set_style_bg_color(s_adminPassInput, theme::BG, 0);
    lv_obj_set_style_text_color(s_adminPassInput, theme::TEXT, 0);
    lv_obj_set_style_border_color(s_adminPassInput, theme::BORDER, 0);
    lv_obj_set_style_border_width(s_adminPassInput, 1, 0);
    lv_obj_set_style_radius(s_adminPassInput, 4, 0);
    lv_obj_set_style_pad_all(s_adminPassInput, 4, 0);

    // Button row
    lv_obj_t* btnRow = lv_obj_create(box);
    lv_obj_set_size(btnRow, 200, 30);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_style_pad_column(btnRow, 6, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto makeRowBtn = [&](const char* label, lv_color_t fg, lv_color_t border, lv_event_cb_t cb)
    {
        lv_obj_t* btn = lv_btn_create(btnRow);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 94, 26);
        lv_obj_set_style_bg_color(btn, theme::BG, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, border, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, overlay);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, fg, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
    };

    makeRowBtn("Cancel", theme::TEXT_MUTED, theme::BORDER, _onAdminCancel);
    makeRowBtn("Login",  theme::ACCENT,     theme::ACCENT, _onAdminOk);
}

// ── _onAdminOk() ─────────────────────────────────────────────────────
void ScreenRepeaters::_onAdminOk(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);

    if (s_pendingRepeater < 0 || !s_adminPassInput) {
        lv_obj_del(overlay);
        s_pendingRepeater = -1;
        s_adminPassInput  = nullptr;
        return;
    }

    Repeater r;
    if (!repeaters::get(s_pendingRepeater, r)) {
        lv_obj_del(overlay);
        s_pendingRepeater = -1;
        s_adminPassInput  = nullptr;
        return;
    }

    const char* pass = lv_textarea_get_text(s_adminPassInput);
    bool sent = ops::MeshService::instance().sendRepeaterLogin(r.pubKeyPrefix, pass ? pass : "");
    ScreenTerminal::setAdminTarget(r.pubKeyPrefix, r.name);

    // Save target and password for the login result handler (needed for Retry)
    memcpy(s_adminPrefix, r.pubKeyPrefix, 4);
    strncpy(s_adminName, r.name, sizeof(s_adminName) - 1);
    s_adminName[sizeof(s_adminName) - 1] = '\0';
    strncpy(s_adminPass, pass ? pass : "", sizeof(s_adminPass) - 1);
    s_adminPass[sizeof(s_adminPass) - 1] = '\0';

    lv_obj_del(overlay);
    s_pendingRepeater = -1;
    s_adminPassInput  = nullptr;

    if (sent) {
        s_awaitingLoginResult = true;
        s_loginSentMs         = (uint32_t)millis();
        _showAdminWaiting();
    } else {
        // Send failed immediately (contact not in mesh table)
        char buf[80];
        snprintf(buf, sizeof(buf), "[repeaters] Login FAILED — %s not reachable", r.name);
        ScreenTerminal::appendLine(buf);
    }
}

// ── _onAdminCancel() ─────────────────────────────────────────────────
void ScreenRepeaters::_onAdminCancel(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    s_pendingRepeater = -1;
    s_adminPassInput  = nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// Set Path dialog
// ─────────────────────────────────────────────────────────────────────
void ScreenRepeaters::_showSetPathDialog()
{
    Repeater r;
    if (s_pendingRepeater < 0 || !repeaters::get(s_pendingRepeater, r)) {
        s_pendingRepeater = -1;
        return;
    }

    s_pathHashSz = 1;  // reset to 1-byte default each time

    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(overlay);
    lv_obj_set_width(box, 240);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title
    char titleBuf[48];
    snprintf(titleBuf, sizeof(titleBuf), "Set Path: %s", r.name);
    lv_obj_t* titleLbl = lv_label_create(box);
    lv_label_set_text(titleLbl, titleBuf);
    lv_obj_set_width(titleLbl, 220);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

    lv_obj_t* hintLbl = lv_label_create(box);
    lv_label_set_text(hintLbl, "Hex path bytes (e.g. AABBCCDD):");
    lv_obj_set_style_text_color(hintLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hintLbl, &lv_font_montserrat_10, 0);

    // Hex path textarea
    s_pathInput = lv_textarea_create(box);
    lv_obj_set_size(s_pathInput, 220, 30);
    lv_textarea_set_one_line(s_pathInput, true);
    lv_textarea_set_max_length(s_pathInput, 32);  // up to 16 bytes = 32 hex chars
    lv_textarea_set_accepted_chars(s_pathInput, "0123456789ABCDEFabcdef");
    lv_textarea_set_placeholder_text(s_pathInput, "AABBCCDD...");
    lv_obj_set_style_bg_color(s_pathInput, theme::BG, 0);
    lv_obj_set_style_text_color(s_pathInput, theme::TEXT, 0);
    lv_obj_set_style_border_color(s_pathInput, theme::BORDER, 0);
    lv_obj_set_style_border_width(s_pathInput, 1, 0);
    lv_obj_set_style_radius(s_pathInput, 4, 0);
    lv_obj_set_style_pad_all(s_pathInput, 4, 0);

    // Hash-size toggle row
    lv_obj_t* szRow = lv_obj_create(box);
    lv_obj_set_size(szRow, 220, 30);
    lv_obj_set_style_bg_opa(szRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(szRow, 0, 0);
    lv_obj_set_style_pad_all(szRow, 0, 0);
    lv_obj_set_style_pad_column(szRow, 6, 0);
    lv_obj_clear_flag(szRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(szRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(szRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* szLabel = lv_label_create(szRow);
    lv_label_set_text(szLabel, "Hash size:");
    lv_obj_set_style_text_color(szLabel, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(szLabel, &lv_font_montserrat_10, 0);

    s_hashSz1Btn = lv_btn_create(szRow);
    lv_group_remove_obj(s_hashSz1Btn);
    lv_obj_set_size(s_hashSz1Btn, 50, 24);
    lv_obj_set_style_bg_color(s_hashSz1Btn, theme::ACCENT, 0);   // active initially
    lv_obj_set_style_border_color(s_hashSz1Btn, theme::ACCENT, 0);
    lv_obj_set_style_border_width(s_hashSz1Btn, 1, 0);
    lv_obj_set_style_radius(s_hashSz1Btn, 4, 0);
    lv_obj_set_style_shadow_width(s_hashSz1Btn, 0, 0);
    lv_obj_add_event_cb(s_hashSz1Btn, _onHashSz1Click, LV_EVENT_CLICKED, overlay);
    lv_obj_t* sz1Lbl = lv_label_create(s_hashSz1Btn);
    lv_label_set_text(sz1Lbl, "1-byte");
    lv_obj_set_style_text_color(sz1Lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(sz1Lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(sz1Lbl);

    s_hashSz2Btn = lv_btn_create(szRow);
    lv_group_remove_obj(s_hashSz2Btn);
    lv_obj_set_size(s_hashSz2Btn, 50, 24);
    lv_obj_set_style_bg_color(s_hashSz2Btn, theme::BG, 0);       // inactive
    lv_obj_set_style_border_color(s_hashSz2Btn, theme::BORDER, 0);
    lv_obj_set_style_border_width(s_hashSz2Btn, 1, 0);
    lv_obj_set_style_radius(s_hashSz2Btn, 4, 0);
    lv_obj_set_style_shadow_width(s_hashSz2Btn, 0, 0);
    lv_obj_add_event_cb(s_hashSz2Btn, _onHashSz2Click, LV_EVENT_CLICKED, overlay);
    lv_obj_t* sz2Lbl = lv_label_create(s_hashSz2Btn);
    lv_label_set_text(sz2Lbl, "2-byte");
    lv_obj_set_style_text_color(sz2Lbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(sz2Lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(sz2Lbl);

    // Save / Cancel button row
    lv_obj_t* btnRow = lv_obj_create(box);
    lv_obj_set_size(btnRow, 220, 30);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_style_pad_column(btnRow, 6, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto makeRowBtn = [&](const char* label, lv_color_t fg, lv_color_t border, lv_event_cb_t cb)
    {
        lv_obj_t* btn = lv_btn_create(btnRow);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 104, 26);
        lv_obj_set_style_bg_color(btn, theme::BG, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, border, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, overlay);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, fg, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
    };

    makeRowBtn("Cancel", theme::TEXT_MUTED, theme::BORDER, _onSetPathCancel);
    makeRowBtn("Save",   theme::ACCENT,     theme::ACCENT, _onSetPathSave);
}

// ── _onHashSz1Click() — select 1-byte hash size ───────────────────────
void ScreenRepeaters::_onHashSz1Click(lv_event_t* /*e*/)
{
    s_pathHashSz = 1;
    if (s_hashSz1Btn) {
        lv_obj_set_style_bg_color(s_hashSz1Btn, theme::ACCENT, 0);
        lv_obj_set_style_border_color(s_hashSz1Btn, theme::ACCENT, 0);
        lv_obj_t* lbl = lv_obj_get_child(s_hashSz1Btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    }
    if (s_hashSz2Btn) {
        lv_obj_set_style_bg_color(s_hashSz2Btn, theme::BG, 0);
        lv_obj_set_style_border_color(s_hashSz2Btn, theme::BORDER, 0);
        lv_obj_t* lbl = lv_obj_get_child(s_hashSz2Btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, theme::TEXT_MUTED, 0);
    }
}

// ── _onHashSz2Click() — select 2-byte hash size ───────────────────────
void ScreenRepeaters::_onHashSz2Click(lv_event_t* /*e*/)
{
    s_pathHashSz = 2;
    if (s_hashSz2Btn) {
        lv_obj_set_style_bg_color(s_hashSz2Btn, theme::ACCENT, 0);
        lv_obj_set_style_border_color(s_hashSz2Btn, theme::ACCENT, 0);
        lv_obj_t* lbl = lv_obj_get_child(s_hashSz2Btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    }
    if (s_hashSz1Btn) {
        lv_obj_set_style_bg_color(s_hashSz1Btn, theme::BG, 0);
        lv_obj_set_style_border_color(s_hashSz1Btn, theme::BORDER, 0);
        lv_obj_t* lbl = lv_obj_get_child(s_hashSz1Btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, theme::TEXT_MUTED, 0);
    }
}

// ── _onSetPathSave() ─────────────────────────────────────────────────
void ScreenRepeaters::_onSetPathSave(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);

    if (s_pendingRepeater < 0 || !s_pathInput) {
        lv_obj_del(overlay);
        s_pendingRepeater = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    Repeater r;
    if (!repeaters::get(s_pendingRepeater, r)) {
        lv_obj_del(overlay);
        s_pendingRepeater = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    const char* hex = lv_textarea_get_text(s_pathInput);
    int hexLen = hex ? (int)strlen(hex) : 0;

    // Validate: must be even, non-empty, and divisible by hash-size * 2
    if (hexLen == 0 || hexLen % 2 != 0) {
        ScreenTerminal::appendLine("[set path] Invalid hex: must be even number of chars");
        lv_obj_del(overlay);
        s_pendingRepeater = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    int byteCount = hexLen / 2;
    int hashSzBytes = (int)s_pathHashSz;

    if (byteCount % hashSzBytes != 0) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "[set path] Byte count (%d) not divisible by hash size (%d)",
                 byteCount, hashSzBytes);
        ScreenTerminal::appendLine(buf);
        lv_obj_del(overlay);
        s_pendingRepeater = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    // Parse hex bytes
    uint8_t pathBytes[16] = {};
    if (byteCount > 16) byteCount = 16;
    for (int i = 0; i < byteCount; i++) {
        char hb[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
        pathBytes[i] = (uint8_t)strtol(hb, nullptr, 16);
    }

    uint8_t numHops = (uint8_t)(byteCount / hashSzBytes);
    bool ok = ops::MeshService::instance().setContactPath(
        r.pubKeyPrefix, pathBytes, numHops, (uint8_t)hashSzBytes);

    char buf[80];
    if (ok)
        snprintf(buf, sizeof(buf), "[set path] OK — %s, %d hop(s), %d-byte hashes",
                 r.name, numHops, hashSzBytes);
    else
        snprintf(buf, sizeof(buf), "[set path] FAILED — %s not in mesh table", r.name);
    ScreenTerminal::appendLine(buf);

    lv_obj_del(overlay);
    s_pendingRepeater = -1;
    s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
}

// ── _onSetPathCancel() ────────────────────────────────────────────────
void ScreenRepeaters::_onSetPathCancel(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    s_pendingRepeater = -1;
    s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// Admin login flow — waiting overlay
// ─────────────────────────────────────────────────────────────────────

// ── _showAdminWaiting() ───────────────────────────────────────────────
void ScreenRepeaters::_showAdminWaiting()
{
    s_waitOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_waitOverlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_waitOverlay, 0, 0);
    lv_obj_set_style_bg_color(s_waitOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_waitOverlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_waitOverlay, 0, 0);
    lv_obj_clear_flag(s_waitOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(s_waitOverlay);
    lv_obj_set_width(box, 210);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char titleBuf[48];
    snprintf(titleBuf, sizeof(titleBuf), "Admin: %s", s_adminName);
    lv_obj_t* titleLbl = lv_label_create(box);
    lv_label_set_text(titleLbl, titleBuf);
    lv_obj_set_width(titleLbl, 188);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

    lv_obj_t* waitLbl = lv_label_create(box);
    lv_label_set_text(waitLbl, "Logging in...");
    lv_obj_set_style_text_color(waitLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(waitLbl, &lv_font_montserrat_10, 0);

    lv_obj_t* cancelBtn = lv_btn_create(box);
    lv_group_remove_obj(cancelBtn);
    lv_obj_set_size(cancelBtn, 188, 26);
    lv_obj_set_style_bg_color(cancelBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(cancelBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(cancelBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(cancelBtn, 1, 0);
    lv_obj_set_style_radius(cancelBtn, 4, 0);
    lv_obj_set_style_shadow_width(cancelBtn, 0, 0);
    lv_obj_add_event_cb(cancelBtn, _onWaitCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
    lv_label_set_text(cancelLbl, "Cancel");
    lv_obj_set_style_text_color(cancelLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(cancelLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(cancelLbl);
}

// ── _onWaitCancel() ───────────────────────────────────────────────────
void ScreenRepeaters::_onWaitCancel(lv_event_t* /*e*/)
{
    s_awaitingLoginResult = false;
    if (s_waitOverlay) { lv_obj_del(s_waitOverlay); s_waitOverlay = nullptr; }
}

// ── tickLoginResult() — called every UIScreen tick ────────────────────
void ScreenRepeaters::tickLoginResult()
{
    if (!s_awaitingLoginResult) return;

    bool ok = false;
    if (ops::MeshService::instance().pollLoginResult(ok)) {
        onLoginResult(ok);
        return;
    }

    // 15-second timeout
    if ((uint32_t)millis() - s_loginSentMs > 15000UL) {
        onLoginResult(false);
    }
}

// ── onLoginResult() ───────────────────────────────────────────────────
void ScreenRepeaters::onLoginResult(bool ok)
{
    s_awaitingLoginResult = false;
    if (s_waitOverlay) { lv_obj_del(s_waitOverlay); s_waitOverlay = nullptr; }

    if (ok) {
        _showAdminPanel();
    } else {
        // Show a brief failure notice as an overlay on the repeater screen
        lv_obj_t* overlay = lv_obj_create(lv_scr_act());
        lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
        lv_obj_set_pos(overlay, 0, 0);
        lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
        lv_obj_set_style_border_width(overlay, 0, 0);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* box = lv_obj_create(overlay);
        lv_obj_set_width(box, 210);
        lv_obj_set_height(box, LV_SIZE_CONTENT);
        lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
        lv_obj_set_style_border_color(box, theme::RED, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_pad_all(box, 10, 0);
        lv_obj_set_style_pad_row(box, 8, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        char titleBuf[48];
        snprintf(titleBuf, sizeof(titleBuf), "Admin: %s", s_adminName);
        lv_obj_t* titleLbl = lv_label_create(box);
        lv_label_set_text(titleLbl, titleBuf);
        lv_obj_set_width(titleLbl, 188);
        lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* errLbl = lv_label_create(box);
        lv_label_set_text(errLbl, "Login failed or timed out.\nCheck password and try again.");
        lv_obj_set_width(errLbl, 188);
        lv_label_set_long_mode(errLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(errLbl, theme::RED, 0);
        lv_obj_set_style_text_font(errLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* btnRow = lv_obj_create(box);
        lv_obj_set_size(btnRow, 188, 30);
        lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btnRow, 0, 0);
        lv_obj_set_style_pad_all(btnRow, 0, 0);
        lv_obj_set_style_pad_column(btnRow, 6, 0);
        lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        auto makeErrBtn = [&](const char* label, lv_color_t fg, lv_color_t border, lv_event_cb_t cb)
        {
            lv_obj_t* btn = lv_btn_create(btnRow);
            lv_group_remove_obj(btn);
            lv_obj_set_size(btn, 88, 26);
            lv_obj_set_style_bg_color(btn, theme::BG, 0);
            lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
            lv_obj_set_style_border_color(btn, border, 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_radius(btn, 4, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, overlay);
            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, label);
            lv_obj_set_style_text_color(lbl, fg, 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
            lv_obj_center(lbl);
        };

        makeErrBtn("OK",    theme::TEXT_MUTED, theme::BORDER, [](lv_event_t* ev){
            lv_obj_del((lv_obj_t*)lv_event_get_user_data(ev));
        });
        makeErrBtn("Retry", theme::ACCENT,     theme::ACCENT, _onRetryLogin);
    }
}

// ── _onRetryLogin() — re-attempt login with saved credentials ─────────
void ScreenRepeaters::_onRetryLogin(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);

    bool sent = ops::MeshService::instance().sendRepeaterLogin(s_adminPrefix, s_adminPass);
    if (sent) {
        s_awaitingLoginResult = true;
        s_loginSentMs         = (uint32_t)millis();
        _showAdminWaiting();
    } else {
        char buf[80];
        snprintf(buf, sizeof(buf), "[repeaters] Retry FAILED — %s not reachable", s_adminName);
        ScreenTerminal::appendLine(buf);
    }
}

// ─────────────────────────────────────────────────────────────────────
// Admin panel screen (shown after successful login)
// ─────────────────────────────────────────────────────────────────────

static constexpr int ADM_TOP_H  = 28;
static constexpr int ADM_BTN_H  = 68;   // two wrapped rows of buttons
static constexpr int ADM_RESP_Y = ADM_TOP_H + ADM_BTN_H;
static constexpr int ADM_RESP_H = OPS_SCREEN_H - ADM_RESP_Y;

// ── _showAdminPanel() ─────────────────────────────────────────────────
void ScreenRepeaters::_showAdminPanel()
{
    // Reset response buffer
    s_adminRespBuf[0] = '\0';

    s_adminScreen = lv_obj_create(nullptr);
    lv_obj_set_size(s_adminScreen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(s_adminScreen, theme::BG, 0);
    lv_obj_set_style_pad_all(s_adminScreen, 0, 0);
    lv_obj_clear_flag(s_adminScreen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ───────────────────────────────────────────────────────
    lv_obj_t* bar = lv_obj_create(s_adminScreen);
    lv_obj_set_size(bar, OPS_SCREEN_W, ADM_TOP_H);
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

    char titleBuf[52];
    snprintf(titleBuf, sizeof(titleBuf), LV_SYMBOL_SETTINGS "  %s", s_adminName);
    lv_obj_t* titleLbl = lv_label_create(bar);
    lv_obj_set_flex_grow(titleLbl, 1);
    lv_label_set_text(titleLbl, titleBuf);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

    lv_obj_t* closeBtn = lv_btn_create(bar);
    lv_group_remove_obj(closeBtn);
    lv_obj_set_height(closeBtn, ADM_TOP_H - 6);
    lv_obj_set_style_bg_color(closeBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(closeBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(closeBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(closeBtn, 1, 0);
    lv_obj_set_style_radius(closeBtn, 4, 0);
    lv_obj_set_style_shadow_width(closeBtn, 0, 0);
    lv_obj_set_style_pad_hor(closeBtn, 6, 0);
    lv_obj_add_event_cb(closeBtn, _onAdminClose, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* closeLbl = lv_label_create(closeBtn);
    lv_label_set_text(closeLbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(closeLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(closeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(closeLbl);

    // ── Button row ────────────────────────────────────────────────────
    lv_obj_t* btnRow = lv_obj_create(s_adminScreen);
    lv_obj_set_size(btnRow, OPS_SCREEN_W, ADM_BTN_H);
    lv_obj_align(btnRow, LV_ALIGN_TOP_LEFT, 0, ADM_TOP_H);
    lv_obj_set_style_bg_color(btnRow, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_radius(btnRow, 0, 0);
    lv_obj_set_style_pad_hor(btnRow, 6, 0);
    lv_obj_set_style_pad_ver(btnRow, 4, 0);
    lv_obj_set_style_pad_column(btnRow, 6, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Helper: add a small action button to the button row.
    // accent_col = border/text colour; cb = click callback.
    auto makeAdminBtn = [&](const char* label, lv_color_t accent_col,
                            lv_event_cb_t cb) -> lv_obj_t*
    {
        lv_obj_t* btn = lv_btn_create(btnRow);
        lv_group_remove_obj(btn);
        lv_obj_set_height(btn, 26);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(btn, theme::BG, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, accent_col, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_hor(btn, 8, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, accent_col, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
        return btn;
    };

    makeAdminBtn(LV_SYMBOL_REFRESH  " Status", theme::ACCENT,     _onAdminStatus);
    makeAdminBtn(LV_SYMBOL_LOOP     " Clk",    theme::TEXT_MUTED, _onAdminClockSync);
    makeAdminBtn(LV_SYMBOL_WIFI     " Advert", theme::TEXT_MUTED, _onAdminAdvert);
    makeAdminBtn(LV_SYMBOL_LIST     " Nbrs",   theme::TEXT_MUTED, _onAdminNbrs);
    makeAdminBtn(LV_SYMBOL_KEYBOARD " Term",   theme::TEXT_MUTED, _onAdminTerminal);
    makeAdminBtn(LV_SYMBOL_POWER    " Reboot", theme::RED,        _onAdminReboot);

    // ── Response area ─────────────────────────────────────────────────
    lv_obj_t* respArea = lv_obj_create(s_adminScreen);
    lv_obj_set_size(respArea, OPS_SCREEN_W, ADM_RESP_H);
    lv_obj_align(respArea, LV_ALIGN_TOP_LEFT, 0, ADM_RESP_Y);
    lv_obj_set_style_bg_color(respArea, theme::BG, 0);
    lv_obj_set_style_border_width(respArea, 0, 0);
    lv_obj_set_style_radius(respArea, 0, 0);
    lv_obj_set_style_pad_all(respArea, 4, 0);
    lv_obj_set_scroll_dir(respArea, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(respArea, LV_SCROLLBAR_MODE_AUTO);

    s_adminRespLbl = lv_label_create(respArea);
    lv_label_set_text(s_adminRespLbl, "Press Status to query the repeater.");
    lv_label_set_long_mode(s_adminRespLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_adminRespLbl, OPS_SCREEN_W - 10);
    lv_obj_set_style_text_color(s_adminRespLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_adminRespLbl, &lv_font_montserrat_10, 0);

    // Wire backspace key to close: add respArea to the default input group so
    // keyboard events reach it, then register the key handler.
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, respArea);
        lv_group_focus_obj(respArea);
    }
    lv_obj_add_event_cb(respArea, _onAdminKey, LV_EVENT_KEY, nullptr);

    lv_scr_load(s_adminScreen);
    OPS_LOG("UI", "Admin panel open: %s", s_adminName);
}

// ── _onAdminStatus() — GET_STATUS request via binary REQ_TYPE protocol ──
void ScreenRepeaters::_onAdminStatus(lv_event_t* /*e*/)
{
    uint32_t now = (uint32_t)millis();
    if (now - s_lastStatusMs < 4000) return;  // 4-second debounce
    s_lastStatusMs = now;

    s_adminRespBuf[0] = '\0';
    if (s_adminRespLbl) {
        lv_label_set_text(s_adminRespLbl, "Querying...");
        lv_obj_set_style_text_color(s_adminRespLbl, theme::TEXT_MUTED, 0);
    }

    ops::MeshService::instance().sendRepeaterStatusReq(s_adminPrefix);

    OPS_LOG("Admin", "Status request sent to %s", s_adminName);
}

// ── _onAdminClockSync() — "clock sync" CLI command ───────────────────
void ScreenRepeaters::_onAdminClockSync(lv_event_t* /*e*/)
{
    uint32_t now = (uint32_t)millis();
    if (now - s_lastStatusMs < 2000) return;  // light debounce
    s_lastStatusMs = now;
    // "clock sync" tells the repeater to adopt sender_timestamp (our current time).
    bool ok = ops::MeshService::instance().sendAdminCommand(s_adminPrefix, "clock sync");
    OPS_LOG("Admin", "Clock sync %s to %s", ok ? "sent" : "FAILED", s_adminName);
}

// ── _onAdminAdvert() — ask repeater to flood-advertise itself ─────────
void ScreenRepeaters::_onAdminAdvert(lv_event_t* /*e*/)
{
    uint32_t now = (uint32_t)millis();
    if (now - s_lastStatusMs < 2000) return;
    s_lastStatusMs = now;
    bool ok = ops::MeshService::instance().sendAdminCommand(s_adminPrefix, "advert");
    OPS_LOG("Admin", "Advert cmd %s to %s", ok ? "sent" : "FAILED", s_adminName);
}

// ── _onAdminNbrs() — GET_NEIGHBOURS request ───────────────────────────
void ScreenRepeaters::_onAdminNbrs(lv_event_t* /*e*/)
{
    uint32_t now = (uint32_t)millis();
    if (now - s_lastStatusMs < 4000) return;  // same debounce as Status
    s_lastStatusMs = now;

    s_adminRespBuf[0] = '\0';
    if (s_adminRespLbl) {
        lv_label_set_text(s_adminRespLbl, "Querying neighbours...");
        lv_obj_set_style_text_color(s_adminRespLbl, theme::TEXT_MUTED, 0);
    }
    ops::MeshService::instance().sendRepeaterNeighboursReq(s_adminPrefix);
    OPS_LOG("Admin", "Neighbours request sent to %s", s_adminName);
}

// ── _onAdminTerminal() — open admin-mode terminal for this repeater ───
void ScreenRepeaters::_onAdminTerminal(lv_event_t* /*e*/)
{
    // Null the response label so subsequent responses go to the terminal log.
    s_adminRespLbl = nullptr;
    s_adminRespBuf[0] = '\0';

    // Open admin terminal (loads new screen over the admin panel).
    ScreenTerminal::showAdmin(s_adminPrefix, s_adminName);

    // Now safe to delete the admin panel — it is no longer the active screen.
    lv_obj_t* dying = s_adminScreen;
    s_adminScreen = nullptr;
    if (dying) lv_obj_del(dying);
}

// ── _onAdminReboot() — show confirmation dialog before rebooting ──────
void ScreenRepeaters::_onAdminReboot(lv_event_t* /*e*/)
{
    if (!s_adminScreen) return;

    // Dim overlay
    lv_obj_t* overlay = lv_obj_create(s_adminScreen);
    lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Confirmation box
    lv_obj_t* box = lv_obj_create(overlay);
    lv_obj_set_size(box, 200, 90);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::RED, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_set_style_pad_column(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char msg[52];
    snprintf(msg, sizeof(msg), "Reboot %s?", s_adminName);
    lv_obj_t* msgLbl = lv_label_create(box);
    lv_label_set_text(msgLbl, msg);
    lv_label_set_long_mode(msgLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(msgLbl, 180);
    lv_obj_set_style_text_color(msgLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(msgLbl, &lv_font_montserrat_10, 0);

    lv_obj_t* btnRow = lv_obj_create(box);
    lv_obj_set_size(btnRow, 184, 30);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_style_pad_column(btnRow, 8, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto mkBtn = [&](const char* label, lv_color_t col, lv_event_cb_t cb)
    {
        lv_obj_t* btn = lv_btn_create(btnRow);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 84, 26);
        lv_obj_set_style_bg_color(btn, theme::BG, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, col, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, overlay);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, col, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
    };
    mkBtn("Cancel",             theme::TEXT_MUTED, _onAdminRebootCancel);
    mkBtn(LV_SYMBOL_POWER " Reboot", theme::RED,   _onAdminRebootConfirm);
}

// ── _onAdminRebootCancel() ────────────────────────────────────────────
void ScreenRepeaters::_onAdminRebootCancel(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (overlay) lv_obj_del(overlay);
}

// ── _onAdminRebootConfirm() ───────────────────────────────────────────
void ScreenRepeaters::_onAdminRebootConfirm(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (overlay) lv_obj_del(overlay);

    bool ok = ops::MeshService::instance().sendAdminCommand(s_adminPrefix, "reboot");
    OPS_LOG("Admin", "Reboot cmd %s to %s", ok ? "sent" : "FAILED", s_adminName);

    if (s_adminRespLbl) {
        lv_label_set_text(s_adminRespLbl,
            ok ? "Reboot command sent." : "Reboot FAILED — not reachable.");
        lv_obj_set_style_text_color(s_adminRespLbl,
            ok ? theme::ORANGE : theme::RED, 0);
    }
}

// ── _onAdminKey() — backspace / ESC closes admin panel ────────────────
void ScreenRepeaters::_onAdminKey(lv_event_t* e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC)
        _onAdminClose(e);
}

// ── _onAdminClose() ───────────────────────────────────────────────────
void ScreenRepeaters::_onAdminClose(lv_event_t* /*e*/)
{
    s_adminRespLbl = nullptr;
    s_adminRespBuf[0] = '\0';

    lv_obj_t* dying = s_adminScreen;
    s_adminScreen = nullptr;

    ScreenRepeaters::show();   // loads new screen first

    if (dying) lv_obj_del(dying);  // safe to delete now it's no longer active
}

// ── onContactResponse() — append to admin panel if open ───────────────
void ScreenRepeaters::onContactResponse(const char* line)
{
    if (!s_adminRespLbl) return;

    // Strip the "[Name] " prefix if present so the panel stays clean
    const char* text = line;
    const char* bracket = strstr(line, "] ");
    if (bracket) text = bracket + 2;

    int used = (int)strlen(s_adminRespBuf);
    int remaining = (int)sizeof(s_adminRespBuf) - used - 1;
    if (remaining <= 0) {
        // Scroll buffer: drop first half to make room
        int half = (int)sizeof(s_adminRespBuf) / 2;
        memmove(s_adminRespBuf, s_adminRespBuf + half,
                sizeof(s_adminRespBuf) - half);
        s_adminRespBuf[sizeof(s_adminRespBuf) - half - 1] = '\0';
        used = (int)strlen(s_adminRespBuf);
        remaining = (int)sizeof(s_adminRespBuf) - used - 1;
    }

    if (used > 0) {
        strncat(s_adminRespBuf, "\n", remaining);
        remaining--;
    }
    strncat(s_adminRespBuf, text, remaining);

    lv_label_set_text(s_adminRespLbl, s_adminRespBuf);
    lv_obj_set_style_text_color(s_adminRespLbl, theme::TEXT, 0);

    // Scroll response area to bottom
    lv_obj_t* parent = lv_obj_get_parent(s_adminRespLbl);
    if (parent) lv_obj_scroll_to_y(parent, LV_COORD_MAX, LV_ANIM_OFF);
}

}}  // namespace ops::ui
