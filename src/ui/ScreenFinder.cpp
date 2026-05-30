// Saitama — ScreenFinder.cpp
// Copyright 2026 Saitama — MIT License

#include "ScreenFinder.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../mesh/MeshService.h"
#include "../utils/Contacts.h"
#include "../utils/Repeaters.h"
#include "../utils/Config.h"
#include "../utils/Log.h"

#include <lvgl.h>
#include <helpers/AdvertDataHelpers.h>  // ADV_TYPE_CHAT, ADV_TYPE_REPEATER
#include <cstring>
#include <cstdio>

namespace ops { namespace ui {

// ── Layout ───────────────────────────────────────────────────────────
static constexpr int TOP_H  = 28;
static constexpr int BTN_H  = 36;
static constexpr int LIST_Y = TOP_H + BTN_H;

// ── Static widget pointers ───────────────────────────────────────────
lv_obj_t* ScreenFinder::_screen     = nullptr;
lv_obj_t* ScreenFinder::_statusLbl  = nullptr;
lv_obj_t* ScreenFinder::_list       = nullptr;
lv_obj_t* ScreenFinder::_scanBtn    = nullptr;
lv_obj_t* ScreenFinder::_scanBtnLbl = nullptr;

// ── Module-level state ───────────────────────────────────────────────
static constexpr int    MAX_RESULTS = 20;
static ops::DiscoverEntry s_results[MAX_RESULTS];
static int                s_resultCount    = 0;
static uint32_t           s_scanDeadlineMs = 0;
static lv_obj_t*          s_popup          = nullptr;
static int                s_popupIdx       = -1;

// ── Helpers ──────────────────────────────────────────────────────────

static const char* _typeLabel(uint8_t t)
{
    switch (t) {
        case 1: return "Chat";
        case 2: return "Rpt";
        case 3: return "Room";
        case 4: return "Sensor";
        default: return "?";
    }
}

static lv_color_t _typeColor(uint8_t t)
{
    switch (t) {
        case 1: return theme::ACCENT;
        case 2: return theme::ORANGE;
        case 3: return theme::GREEN;
        default: return theme::TEXT_MUTED;
    }
}

static bool _isInList(const ops::DiscoverEntry& e)
{
    bool isRpt = (e.nodeType == ADV_TYPE_REPEATER);
    return isRpt ? ops::repeaters::findByKey(e.pubKeyPrefix)
                 : ops::contacts::findByKey(e.pubKeyPrefix);
}

static void _displayName(const ops::DiscoverEntry& e, char* buf, int bufLen)
{
    if (e.name[0]) {
        strncpy(buf, e.name, bufLen - 1);
        buf[bufLen - 1] = '\0';
    } else {
        snprintf(buf, bufLen, "%02X:%02X:%02X:%02X",
                 e.pubKeyPrefix[0], e.pubKeyPrefix[1],
                 e.pubKeyPrefix[2], e.pubKeyPrefix[3]);
    }
}

// ── Popup ─────────────────────────────────────────────────────────────

void ScreenFinder::_onPopupClose(lv_event_t* /*e*/)
{
    if (s_popup) { lv_obj_del(s_popup); s_popup = nullptr; }
    s_popupIdx = -1;
}

void ScreenFinder::_onPopupAdd(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_resultCount) return;
    const ops::DiscoverEntry& en = s_results[idx];
    if (_isInList(en)) return;  // already added — button should have been disabled

    char fallbackName[16];
    snprintf(fallbackName, sizeof(fallbackName), "%02X%02X%02X%02X",
             en.pubKeyPrefix[0], en.pubKeyPrefix[1],
             en.pubKeyPrefix[2], en.pubKeyPrefix[3]);
    const char* useName = en.name[0] ? en.name : fallbackName;
    bool isRpt = (en.nodeType == ADV_TYPE_REPEATER);

    if (isRpt) {
        ops::Repeater r{};
        strncpy(r.name, useName, sizeof(r.name) - 1);
        memcpy(r.pubKeyPrefix, en.pubKeyPrefix, 4);
        memcpy(r.pubKey,       en.pubKey,       32);
        r.lastRssi     = en.rssi;
        r.outPathLen   = 0;    // direct zero-hop neighbor
        r.outPathValid = true;
        ops::repeaters::add(r);
        OPS_LOG("Finder", "Manually added repeater: %s", useName);
    } else {
        ops::Contact c{};
        strncpy(c.name, useName, sizeof(c.name) - 1);
        memcpy(c.pubKeyPrefix, en.pubKeyPrefix, 4);
        memcpy(c.pubKey,       en.pubKey,       32);
        c.lastRssi     = en.rssi;
        c.outPathLen   = 0;
        c.outPathValid = true;
        ops::contacts::add(c);
        OPS_LOG("Finder", "Manually added contact: %s", useName);
    }

    // Close popup and refresh list so rows reflect new state
    if (s_popup) { lv_obj_del(s_popup); s_popup = nullptr; }
    s_popupIdx = -1;
    _rebuildList();
}

void ScreenFinder::_showActionPopup(int idx)
{
    if (idx < 0 || idx >= s_resultCount) return;
    if (s_popup) { lv_obj_del(s_popup); s_popup = nullptr; }
    s_popupIdx = idx;

    const ops::DiscoverEntry& e = s_results[idx];

    // Floating panel — auto-height, centered on screen
    s_popup = lv_obj_create(lv_scr_act());
    lv_obj_set_width(s_popup, 260);
    lv_obj_set_height(s_popup, LV_SIZE_CONTENT);
    lv_obj_align(s_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_popup, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(s_popup, theme::BORDER, 0);
    lv_obj_set_style_border_width(s_popup, 1, 0);
    lv_obj_set_style_radius(s_popup, 6, 0);
    lv_obj_set_style_pad_all(s_popup, 10, 0);
    lv_obj_set_style_pad_row(s_popup, 6, 0);
    lv_obj_clear_flag(s_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_popup,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Name / prefix header
    char nameBuf[40];
    _displayName(e, nameBuf, sizeof(nameBuf));
    lv_obj_t* nameLbl = lv_label_create(s_popup);
    lv_label_set_text(nameLbl, nameBuf);
    lv_obj_set_style_text_color(nameLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(nameLbl, lv_pct(100));
    lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);

    // Type + RSSI info line
    char infoLine[48];
    snprintf(infoLine, sizeof(infoLine), "%s  RSSI: %.0f dBm  SNR: %.1f",
             _typeLabel(e.nodeType),
             (double)e.rssi,
             (double)e.snrInbound / 4.0f);
    lv_obj_t* infoLbl = lv_label_create(s_popup);
    lv_label_set_text(infoLbl, infoLine);
    lv_obj_set_style_text_color(infoLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(infoLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(infoLbl, lv_pct(100));

    // Helper: create a full-width button inside the popup
    auto mkBtn = [](lv_obj_t* parent, const char* label, lv_color_t bg,
                    lv_event_cb_t cb, void* ud) -> lv_obj_t*
    {
        lv_obj_t* btn = lv_btn_create(parent);
        lv_obj_set_size(btn, lv_pct(100), 30);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
        return btn;
    };

    // Add button — green if not yet in list; muted + disabled if already there
    bool isRpt    = (e.nodeType == ADV_TYPE_REPEATER);
    bool inList   = _isInList(e);
    char addLabel[32];
    if (inList) {
        snprintf(addLabel, sizeof(addLabel), LV_SYMBOL_OK " Already Added");
        lv_obj_t* addBtn = mkBtn(s_popup, addLabel, theme::TEXT_MUTED, nullptr, nullptr);
        lv_obj_clear_flag(addBtn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        snprintf(addLabel, sizeof(addLabel), "Add as %s", isRpt ? "Repeater" : "Contact");
        mkBtn(s_popup, addLabel, theme::GREEN, _onPopupAdd, (void*)(intptr_t)idx);
    }

    mkBtn(s_popup, "Close", theme::BG, _onPopupClose, nullptr);
}

// ── _onRowClick() ────────────────────────────────────────────────────
void ScreenFinder::_onRowClick(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    _showActionPopup(idx);
}

// ── _rebuildList() ───────────────────────────────────────────────────
void ScreenFinder::_rebuildList()
{
    if (!_list) return;
    // Close any open popup (row indices shift on rebuild)
    if (s_popup) { lv_obj_del(s_popup); s_popup = nullptr; s_popupIdx = -1; }

    lv_obj_clean(_list);

    if (s_resultCount == 0) {
        lv_obj_t* hint = lv_label_create(_list);
        bool scanning = s_scanDeadlineMs > 0 && millis() <= s_scanDeadlineMs;
        lv_label_set_text(hint, scanning ? "Listening for responses..." : "Press Scan to start");
        lv_obj_set_style_text_color(hint, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 12);
        return;
    }

    for (int i = 0; i < s_resultCount; i++) {
        const ops::DiscoverEntry& e = s_results[i];

        lv_obj_t* row = lv_obj_create(_list);
        lv_obj_set_size(row, lv_pct(100), 24);
        lv_obj_set_style_bg_color(row, (i & 1) ? theme::BG_CARD : theme::BG, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, _onRowClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        // Type badge
        lv_obj_t* typeLbl = lv_label_create(row);
        lv_label_set_text(typeLbl, _typeLabel(e.nodeType));
        lv_obj_set_style_text_color(typeLbl, _typeColor(e.nodeType), 0);
        lv_obj_set_style_text_font(typeLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_width(typeLbl, 44);

        // Name or hex prefix; green check if already saved
        char nameBuf[40];
        _displayName(e, nameBuf, sizeof(nameBuf));
        if (_isInList(e)) {
            char withCheck[48];
            snprintf(withCheck, sizeof(withCheck), LV_SYMBOL_OK " %s", nameBuf);
            strncpy(nameBuf, withCheck, sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
        }
        lv_obj_t* nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, nameBuf);
        lv_obj_set_style_text_color(nameLbl,
            _isInList(e) ? theme::GREEN : theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_flex_grow(nameLbl, 1);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);

        // RSSI
        char rssiBuf[12];
        snprintf(rssiBuf, sizeof(rssiBuf), "%.0fdBm", (double)e.rssi);
        lv_obj_t* rssiLbl = lv_label_create(row);
        lv_label_set_text(rssiLbl, rssiBuf);
        lv_color_t rc = (e.rssi > -80.f)  ? theme::GREEN
                      : (e.rssi > -100.f) ? theme::ORANGE
                                          : theme::RED;
        lv_obj_set_style_text_color(rssiLbl, rc, 0);
        lv_obj_set_style_text_font(rssiLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_width(rssiLbl, 58);
        lv_obj_set_style_text_align(rssiLbl, LV_TEXT_ALIGN_RIGHT, 0);

        // SNR (remote inbound SNR×4 → dB)
        char snrBuf[12];
        snprintf(snrBuf, sizeof(snrBuf), "snr:%.1f", (double)e.snrInbound / 4.0f);
        lv_obj_t* snrLbl = lv_label_create(row);
        lv_label_set_text(snrLbl, snrBuf);
        lv_obj_set_style_text_color(snrLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(snrLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_width(snrLbl, 58);
        lv_obj_set_style_text_align(snrLbl, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

// ── _onScan() ────────────────────────────────────────────────────────
void ScreenFinder::_onScan(lv_event_t* /*e*/)
{
    if (s_popup) { lv_obj_del(s_popup); s_popup = nullptr; s_popupIdx = -1; }
    s_resultCount    = 0;
    s_scanDeadlineMs = millis() + 10000;

    ops::MeshService::instance().suspendDutyCycle(true);
    ops::MeshService::instance().sendDiscoverReq(0x06);  // Chat + Repeater

    if (_statusLbl) {
        lv_label_set_text(_statusLbl, "Scanning...");
        lv_obj_set_style_text_color(_statusLbl, theme::GREEN, 0);
    }
    if (_scanBtnLbl) lv_label_set_text(_scanBtnLbl, LV_SYMBOL_REFRESH " Scanning...");

    _rebuildList();
    OPS_LOG("Finder", "Scan started — duty cycle suspended");
}

// ── _onBack() ────────────────────────────────────────────────────────
void ScreenFinder::_onBack(lv_event_t* /*e*/)
{
    if (s_popup) { lv_obj_del(s_popup); s_popup = nullptr; s_popupIdx = -1; }
    if (s_scanDeadlineMs > 0) {
        s_scanDeadlineMs = 0;
        ops::MeshService::instance().suspendDutyCycle(false);
        OPS_LOG("Finder", "Scan cancelled — duty cycle restored");
    }
    ScreenLauncher::show();
}

// ── _buildScreen() ───────────────────────────────────────────────────
void ScreenFinder::_buildScreen()
{
    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Title bar ─────────────────────────────────────────────────────
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
    lv_obj_set_flex_align(bar,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

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
    lv_obj_add_event_cb(homeBtn, _onBack, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    lv_obj_t* titleLbl = lv_label_create(bar);
    lv_label_set_text(titleLbl, LV_SYMBOL_GPS " Finder");
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_14, 0);

    lv_obj_t* spacer = lv_obj_create(bar);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    _statusLbl = lv_label_create(bar);
    lv_label_set_text(_statusLbl, "Idle");
    lv_obj_set_style_text_color(_statusLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_statusLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_pad_right(_statusLbl, 4, 0);

    // ── Scan button row ───────────────────────────────────────────────
    lv_obj_t* btnRow = lv_obj_create(_screen);
    lv_obj_set_size(btnRow, OPS_SCREEN_W, BTN_H);
    lv_obj_align(btnRow, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(btnRow, theme::BG, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_radius(btnRow, 0, 0);
    lv_obj_set_style_pad_hor(btnRow, 8, 0);
    lv_obj_set_style_pad_ver(btnRow, 4, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);

    _scanBtn = lv_btn_create(btnRow);
    lv_obj_set_size(_scanBtn, OPS_SCREEN_W - 16, BTN_H - 8);
    lv_obj_align(_scanBtn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_scanBtn, theme::PRIMARY, 0);
    lv_obj_set_style_bg_color(_scanBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(_scanBtn, 6, 0);
    lv_obj_set_style_border_width(_scanBtn, 0, 0);
    lv_obj_set_style_shadow_width(_scanBtn, 0, 0);
    lv_obj_add_event_cb(_scanBtn, _onScan, LV_EVENT_CLICKED, nullptr);

    _scanBtnLbl = lv_label_create(_scanBtn);
    lv_label_set_text(_scanBtnLbl, LV_SYMBOL_REFRESH " Scan Nearby Nodes");
    lv_obj_set_style_text_color(_scanBtnLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(_scanBtnLbl, &lv_font_montserrat_12, 0);
    lv_obj_center(_scanBtnLbl);

    // ── Results list ──────────────────────────────────────────────────
    _list = lv_obj_create(_screen);
    lv_obj_set_size(_list, OPS_SCREEN_W, OPS_SCREEN_H - LIST_Y);
    lv_obj_align(_list, LV_ALIGN_TOP_LEFT, 0, LIST_Y);
    lv_obj_set_style_bg_color(_list, theme::BG, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_radius(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 0, 0);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_list,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_AUTO);
    lv_group_focus_obj(_list);
}

// ── show() ───────────────────────────────────────────────────────────
void ScreenFinder::show()
{
    if (!_screen) _buildScreen();
    _rebuildList();
    lv_scr_load(_screen);
    OPS_LOG("UI", "Finder shown");
}

// ── addDiscoverResult() ──────────────────────────────────────────────
void ScreenFinder::addDiscoverResult(const ops::DiscoverEntry& e)
{
    if (s_resultCount >= MAX_RESULTS) return;
    s_results[s_resultCount++] = e;

    if (_screen && lv_scr_act() == _screen) {
        _rebuildList();
        if (_statusLbl) {
            char buf[20];
            snprintf(buf, sizeof(buf), "%d found", s_resultCount);
            lv_label_set_text(_statusLbl, buf);
            lv_obj_set_style_text_color(_statusLbl, theme::ACCENT, 0);
        }
    }
}

// ── tick() ───────────────────────────────────────────────────────────
void ScreenFinder::tick()
{
    if (!_screen || lv_scr_act() != _screen) return;
    if (s_scanDeadlineMs == 0) return;
    if (millis() > s_scanDeadlineMs) {
        s_scanDeadlineMs = 0;
        ops::MeshService::instance().suspendDutyCycle(false);
        OPS_LOG("Finder", "Scan complete — duty cycle restored");
        if (_scanBtnLbl)
            lv_label_set_text(_scanBtnLbl, LV_SYMBOL_REFRESH " Scan Nearby Nodes");
        if (_statusLbl) {
            char buf[20];
            snprintf(buf, sizeof(buf), "%d found", s_resultCount);
            lv_label_set_text(_statusLbl, buf);
            lv_obj_set_style_text_color(_statusLbl,
                s_resultCount > 0 ? theme::ACCENT : theme::TEXT_MUTED, 0);
        }
    }
}

}}  // namespace ops::ui
