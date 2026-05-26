// Saitama — ScreenContacts.cpp
// Copyright 2026 Saitama — MIT License

#include "ScreenContacts.h"
#include "ScreenLauncher.h"
#include "ScreenHome.h"
#include "ScreenTerminal.h"
#include "QRPopup.h"
#include "Theme.h"
#include "../mesh/MeshService.h"
#include "../utils/Contacts.h"
#include "../utils/Log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <time.h>

namespace ops { namespace ui {

lv_obj_t* ScreenContacts::_screen = nullptr;

static constexpr int TOP_H = 28;
static int s_pendingContact = -1;

// Dialog input widget pointers (valid only while a dialog is open)
static lv_obj_t* s_pathInput   = nullptr;
static lv_obj_t* s_hashSz1Btn = nullptr;
static lv_obj_t* s_hashSz2Btn = nullptr;
static uint8_t   s_pathHashSz = 1;

// ── Time formatter ────────────────────────────────────────────────────
static void fmtDateTime(uint32_t ts, char* buf, size_t len)
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
void ScreenContacts::show()
{
    lv_obj_t* old = _screen;
    _screen = nullptr;
    _build();                   // creates _screen, calls lv_scr_load
    if (old) lv_obj_del(old);
}

// ── _build() ─────────────────────────────────────────────────────────
void ScreenContacts::_build()
{
    contacts::reloadFromSD();
    int cnt = contacts::count();

    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ───────────────────────────────────────────────────────
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
    lv_obj_add_event_cb(homeBtn, _onHomeClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    char title[28];
    snprintf(title, sizeof(title), "Contacts (%d)", cnt);
    lv_obj_t* titleLbl = lv_label_create(bar);
    lv_label_set_text(titleLbl, title);
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

    // ── Contact list ──────────────────────────────────────────────────
    lv_obj_t* list = lv_obj_create(_screen);
    lv_obj_set_size(list, OPS_SCREEN_W, OPS_SCREEN_H - TOP_H);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(list, theme::BG, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (cnt == 0) {
        lv_obj_t* empty = lv_label_create(list);
        lv_label_set_text(empty, "No contacts saved.\nGo to Heard to save stations.");
        lv_obj_set_style_text_color(empty, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_10, 0);
        lv_obj_set_style_pad_all(empty, 8, 0);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, OPS_SCREEN_W - 16);
    }

    // Build display order: favourites first, then the rest
    static int s_order[contacts::CAPACITY];
    int j = 0;
    for (int i = 0; i < cnt; i++) { Contact c; if (contacts::get(i, c) && c.favourite)  s_order[j++] = i; }
    for (int i = 0; i < cnt; i++) { Contact c; if (contacts::get(i, c) && !c.favourite) s_order[j++] = i; }

    static const lv_color_t kAmber = LV_COLOR_MAKE(0xFF, 0xB3, 0x00);

    for (int vi = 0; vi < cnt; vi++) {
        int si = s_order[vi];   // storage index — passed as user_data
        Contact c;
        if (!contacts::get(si, c)) continue;

        char addrBuf[8];
        snprintf(addrBuf, sizeof(addrBuf), "%02X%02X%02X",
                 c.pubKeyPrefix[0], c.pubKeyPrefix[1], c.pubKeyPrefix[2]);

        char timeBuf[20];
        fmtDateTime(c.lastSeen, timeBuf, sizeof(timeBuf));

        lv_obj_t* row = lv_btn_create(list);
        lv_group_remove_obj(row);
        lv_obj_set_size(row, OPS_SCREEN_W, 28);
        lv_obj_set_style_bg_color(row, (vi & 1) ? theme::BG_CARD : theme::BG, 0);
        lv_obj_set_style_bg_color(row, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 2, 0);
        lv_obj_set_style_pad_right(row, 6, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, _onRowClick, LV_EVENT_CLICKED, (void*)(intptr_t)si);

        // Unread dot
        lv_obj_t* dot = lv_obj_create(row);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_shadow_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        if (c.hasUnread) {
            lv_obj_set_style_bg_color(dot, theme::RED, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
        }

        // Favourite dot
        lv_obj_t* favDot = lv_obj_create(row);
        lv_obj_set_size(favDot, 8, 8);
        lv_obj_set_style_radius(favDot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(favDot, 0, 0);
        lv_obj_set_style_shadow_width(favDot, 0, 0);
        lv_obj_clear_flag(favDot, LV_OBJ_FLAG_SCROLLABLE);
        if (c.favourite) {
            lv_obj_set_style_bg_color(favDot, kAmber, 0);
            lv_obj_set_style_bg_opa(favDot, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(favDot, LV_OPA_TRANSP, 0);
        }

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, c.name);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nameLbl, 112);  // reduced by 8px for fav dot
        lv_obj_set_style_text_color(nameLbl, c.hasUnread ? theme::TEXT : theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* addrLbl = lv_label_create(row);
        lv_label_set_text(addrLbl, addrBuf);
        lv_obj_set_width(addrLbl, 52);
        lv_obj_set_style_text_color(addrLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(addrLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* timeLbl = lv_label_create(row);
        lv_label_set_text(timeLbl, timeBuf);
        lv_obj_set_width(timeLbl, 116);
        lv_obj_set_style_text_color(timeLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(timeLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_align(timeLbl, LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_scr_load(_screen);
    OPS_LOG("UI", "Contacts shown (%d)", cnt);
}

// ── _onHomeClick() ────────────────────────────────────────────────────
void ScreenContacts::_onHomeClick(lv_event_t* /*e*/)
{
    ScreenLauncher::show();
}

// ── _onRowClick() — show action popup ────────────────────────────────
void ScreenContacts::_onRowClick(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_pendingContact = idx;
    contacts::setUnread(idx, false);

    Contact c;
    if (!contacts::get(idx, c)) return;

    // ── Dim overlay ───────────────────────────────────────────────────
    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, _onPopupClose, LV_EVENT_CLICKED, overlay);

    // ── Action box (auto-height) ──────────────────────────────────────
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
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title (contact name + key prefix)
    char titleBuf[48];
    snprintf(titleBuf, sizeof(titleBuf), "%s  %02X%02X%02X%02X",
             c.name, c.pubKeyPrefix[0], c.pubKeyPrefix[1],
             c.pubKeyPrefix[2], c.pubKeyPrefix[3]);
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
    const char* favLabel = c.favourite ? "Remove Favourite" : "Add Favourite";
    lv_color_t  favFg    = c.favourite ? theme::TEXT_MUTED  : kAmber;
    lv_color_t  favBd    = c.favourite ? theme::BORDER      : kAmber;

    makeBtn("Direct Message", theme::TEXT,  theme::PRIMARY, theme::PRIMARY, _onPopupDM);
    makeBtn(favLabel,         favFg,        theme::BG,      favBd,          _onPopupFavourite);
    makeBtn("Share QR",       theme::ACCENT,theme::BG,      theme::ACCENT,  _onPopupShareQR);
    makeBtn("Set Path",       theme::TEXT,  theme::BG,      theme::BORDER,  _onPopupSetPath);
    makeBtn("Reset Path",     theme::ORANGE,theme::BG,      theme::ORANGE,  _onPopupResetPath);
    makeBtn("Delete Contact", theme::RED,   theme::BG,      theme::RED,     _onPopupDelete);
    makeBtn("Close",     theme::TEXT_MUTED, theme::BG,      theme::BORDER,  _onPopupClose);
}

// ── _onPopupDM() ──────────────────────────────────────────────────────
void ScreenContacts::_onPopupDM(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    Contact c;
    bool ok = (s_pendingContact >= 0) && contacts::get(s_pendingContact, c);
    s_pendingContact = -1;
    if (ok)
        ScreenHome::openDM(c.pubKeyPrefix, c.name);
    else
        ScreenHome::show();
}

// ── _onPopupFavourite() — toggle favourite and rebuild list ───────────
void ScreenContacts::_onPopupFavourite(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (s_pendingContact >= 0) {
        Contact c;
        if (contacts::get(s_pendingContact, c))
            contacts::setFavourite(s_pendingContact, !c.favourite);
    }
    lv_obj_del(overlay);
    s_pendingContact = -1;
    lv_async_call([](void*){ ScreenContacts::show(); }, nullptr);
}

// ── _onPopupSetPath() — close popup and open set-path dialog ──────────
void ScreenContacts::_onPopupSetPath(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    // s_pendingContact preserved — dialog reads it
    _showSetPathDialog();
}

// ── _onPopupResetPath() — reset path and close popup ─────────────────
void ScreenContacts::_onPopupResetPath(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (s_pendingContact >= 0) {
        Contact c;
        if (contacts::get(s_pendingContact, c)) {
            ops::MeshService::instance().resetContactPath(c.pubKeyPrefix);
            char buf[64];
            snprintf(buf, sizeof(buf), "[contacts] Path reset: %s", c.name);
            ScreenTerminal::appendLine(buf);
            OPS_LOG("Contacts", "Path reset for %s", c.name);
        }
    }
    lv_obj_del(overlay);
    s_pendingContact = -1;
}

// ── _onPopupDelete() ─────────────────────────────────────────────────
void ScreenContacts::_onPopupDelete(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (s_pendingContact >= 0) {
        OPS_LOG("Contacts", "Deleted contact %d", s_pendingContact);
        contacts::remove(s_pendingContact);
        s_pendingContact = -1;
    }
    lv_obj_del(overlay);
    lv_async_call([](void*){ ScreenContacts::show(); }, nullptr);
}

// ── _onPopupClose() ───────────────────────────────────────────────────
void ScreenContacts::_onPopupClose(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    s_pendingContact = -1;
}

// ── _onPopupShareQR() — encode contact as QR code ────────────────────
void ScreenContacts::_onPopupShareQR(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);

    Contact c;
    if (s_pendingContact < 0 || !contacts::get(s_pendingContact, c)) {
        s_pendingContact = -1;
        return;
    }
    s_pendingContact = -1;

    char hexBuf[65] = {};
    bool hasKey = false;
    for (int i = 0; i < 32; i++) if (c.pubKey[i]) { hasKey = true; break; }

    char data[110];
    if (hasKey) {
        for (int i = 0; i < 32; i++)
            snprintf(hexBuf + i * 2, 3, "%02X", c.pubKey[i]);
        snprintf(data, sizeof(data), "MC:C:%s/%s", hexBuf, c.name);
    } else {
        // Full key not yet received — encode 4-byte prefix only
        snprintf(hexBuf, 9, "%02X%02X%02X%02X",
                 c.pubKeyPrefix[0], c.pubKeyPrefix[1],
                 c.pubKeyPrefix[2], c.pubKeyPrefix[3]);
        snprintf(data, sizeof(data), "MC:CP:%s/%s", hexBuf, c.name);
    }

    char title[48];
    snprintf(title, sizeof(title), "Contact: %s", c.name);
    ops::ui::showQrPopup(title, data);
}

// ─────────────────────────────────────────────────────────────────────
// Set Path dialog
// ─────────────────────────────────────────────────────────────────────
void ScreenContacts::_showSetPathDialog()
{
    Contact c;
    if (s_pendingContact < 0 || !contacts::get(s_pendingContact, c)) {
        s_pendingContact = -1;
        return;
    }

    s_pathHashSz = 1;

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
    snprintf(titleBuf, sizeof(titleBuf), "Set Path: %s", c.name);
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
    lv_textarea_set_max_length(s_pathInput, 32);
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
    lv_obj_set_style_bg_color(s_hashSz1Btn, theme::ACCENT, 0);
    lv_obj_set_style_border_color(s_hashSz1Btn, theme::ACCENT, 0);
    lv_obj_set_style_border_width(s_hashSz1Btn, 1, 0);
    lv_obj_set_style_radius(s_hashSz1Btn, 4, 0);
    lv_obj_set_style_shadow_width(s_hashSz1Btn, 0, 0);
    lv_obj_add_event_cb(s_hashSz1Btn, _onHashSz1Click, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* sz1Lbl = lv_label_create(s_hashSz1Btn);
    lv_label_set_text(sz1Lbl, "1-byte");
    lv_obj_set_style_text_color(sz1Lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(sz1Lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(sz1Lbl);

    s_hashSz2Btn = lv_btn_create(szRow);
    lv_group_remove_obj(s_hashSz2Btn);
    lv_obj_set_size(s_hashSz2Btn, 50, 24);
    lv_obj_set_style_bg_color(s_hashSz2Btn, theme::BG, 0);
    lv_obj_set_style_border_color(s_hashSz2Btn, theme::BORDER, 0);
    lv_obj_set_style_border_width(s_hashSz2Btn, 1, 0);
    lv_obj_set_style_radius(s_hashSz2Btn, 4, 0);
    lv_obj_set_style_shadow_width(s_hashSz2Btn, 0, 0);
    lv_obj_add_event_cb(s_hashSz2Btn, _onHashSz2Click, LV_EVENT_CLICKED, nullptr);
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

// ── _onHashSz1Click() ────────────────────────────────────────────────
void ScreenContacts::_onHashSz1Click(lv_event_t* /*e*/)
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

// ── _onHashSz2Click() ────────────────────────────────────────────────
void ScreenContacts::_onHashSz2Click(lv_event_t* /*e*/)
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
void ScreenContacts::_onSetPathSave(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);

    if (s_pendingContact < 0 || !s_pathInput) {
        lv_obj_del(overlay);
        s_pendingContact = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    Contact c;
    if (!contacts::get(s_pendingContact, c)) {
        lv_obj_del(overlay);
        s_pendingContact = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    const char* hex = lv_textarea_get_text(s_pathInput);
    int hexLen = hex ? (int)strlen(hex) : 0;

    if (hexLen == 0 || hexLen % 2 != 0) {
        ScreenTerminal::appendLine("[set path] Invalid hex: must be even number of chars");
        lv_obj_del(overlay);
        s_pendingContact = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    int byteCount   = hexLen / 2;
    int hashSzBytes = (int)s_pathHashSz;

    if (byteCount % hashSzBytes != 0) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "[set path] Byte count (%d) not divisible by hash size (%d)",
                 byteCount, hashSzBytes);
        ScreenTerminal::appendLine(buf);
        lv_obj_del(overlay);
        s_pendingContact = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    uint8_t pathBytes[16] = {};
    if (byteCount > 16) byteCount = 16;
    for (int i = 0; i < byteCount; i++) {
        char hb[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
        pathBytes[i] = (uint8_t)strtol(hb, nullptr, 16);
    }

    uint8_t numHops = (uint8_t)(byteCount / hashSzBytes);
    bool ok = ops::MeshService::instance().setContactPath(
        c.pubKeyPrefix, pathBytes, numHops, (uint8_t)hashSzBytes);

    char buf[80];
    if (ok)
        snprintf(buf, sizeof(buf), "[set path] OK — %s, %d hop(s), %d-byte hashes",
                 c.name, numHops, hashSzBytes);
    else
        snprintf(buf, sizeof(buf), "[set path] FAILED — %s not in mesh table", c.name);
    ScreenTerminal::appendLine(buf);

    lv_obj_del(overlay);
    s_pendingContact = -1;
    s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
}

// ── _onSetPathCancel() ────────────────────────────────────────────────
void ScreenContacts::_onSetPathCancel(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    s_pendingContact = -1;
    s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
}

}}  // namespace ops::ui
