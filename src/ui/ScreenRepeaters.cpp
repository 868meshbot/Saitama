// Saitama — ScreenRepeaters.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenRepeaters.h"
#include "../utils/PsramAlloc.h"
#include "ScreenLauncher.h"
#include "ScreenTerminal.h"
#include "../features.h"
#if OPS_FEAT_TRACE
#include "ScreenTrace.h"
#endif
#include "Theme.h"
#include "ListFilter.h"
#include "Emoji.h"
#include "RepeaterAdminTools.h"
#include "../mesh/MeshService.h"
#include "../utils/Repeaters.h"
#include "../utils/Crypto.h"
#include "../utils/Log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <time.h>
#include <Arduino.h>

// Latin-1 Montserrat 12 (has the degree sign; C object, so global scope).
extern const lv_font_t font_montserrat_12_ext;

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
char      (&ScreenRepeaters::s_adminRespBuf)[2048] = ops::psram::array<char[2048]>();

static constexpr int REP_TOP_H = OPS_TOP_H;
// Type-to-filter box in the top bar: matches name or id.
static listfilter::State s_filter;
static void _filterRowText(int key, char* out, size_t outMax)
{
    Repeater x;
    if (!repeaters::get(key, x)) { out[0] = '\0'; return; }
    snprintf(out, outMax, "%s %02X%02X%02X%02X", x.name,
             x.pubKeyPrefix[0], x.pubKeyPrefix[1], x.pubKeyPrefix[2], x.pubKeyPrefix[3]);
}

static int s_pendingRepeater = -1;

// Dialog input widget pointers (valid only while a dialog is open)
static lv_obj_t* s_adminPassInput = nullptr;
static lv_obj_t* s_adminRememberCb = nullptr;
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

// ── Row styling helpers (matches ScreenContacts' row look) ─────────────

// Up to 2 uppercase alnum initials, auto-derived from the repeater name.
static void _repInitials(const char* name, char* out, int outSize)
{
    int n = 0;
    for (const char* p = name; *p && n < 2 && n < outSize - 1; p++) {
        if (isalnum((unsigned char)*p)) out[n++] = (char)toupper((unsigned char)*p);
    }
    if (n == 0) { out[0] = '#'; n = 1; }
    out[n] = '\0';
}

// Deterministic-but-varied avatar background colour per row.
static lv_color_t _repAvatarColor(int idx)
{
    return lv_color_hsv_to_rgb((uint16_t)(((idx + 1) * 53) % 360), 45, 60);
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
    s_filter.rowText = _filterRowText;
    s_filter.onEsc   = [] { ScreenLauncher::show(); };
    listfilter::create(bar, s_filter);

    // ── Repeater list ─────────────────────────────────────────────────
    lv_obj_t* list = lv_obj_create(_screen);
    s_filter.list = list;
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
    static int (&s_order)[repeaters::CAPACITY] = ops::psram::array<int[repeaters::CAPACITY]>();
    int j = 0;
    for (int i = 0; i < cnt; i++) { Repeater r; if (repeaters::get(i, r) && r.favourite)  s_order[j++] = i; }
    for (int i = 0; i < cnt; i++) { Repeater r; if (repeaters::get(i, r) && !r.favourite) s_order[j++] = i; }

    static constexpr int kRowH = OPS_LIST_ROW_H;  // matches ScreenContacts' row height

    for (int vi = 0; vi < cnt; vi++) {
        int si = s_order[vi];   // storage index — passed as user_data
        Repeater r;
        if (!repeaters::get(si, r)) continue;

        char idBuf[10];
        snprintf(idBuf, sizeof(idBuf), "%02X%02X%02X%02X",
                 r.pubKeyPrefix[0], r.pubKeyPrefix[1], r.pubKeyPrefix[2], r.pubKeyPrefix[3]);

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
        lv_obj_set_size(row, OPS_SCREEN_W, kRowH);
        lv_obj_set_style_bg_color(row, (vi & 1) ? theme::BG_CARD : theme::BG, 0);
        lv_obj_set_style_bg_color(row, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 4, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, _onRowClick, LV_EVENT_CLICKED, (void*)(intptr_t)si);
        listfilter::setRowKey(row, si);

        // ── Icon: auto-derived initials on a hashed-colour circle ──────────
        lv_obj_t* avatar = lv_obj_create(row);
        lv_obj_set_size(avatar, OPS_AVATAR_SZ, OPS_AVATAR_SZ);
        if (OPS_AVATAR_SZ == 0) lv_obj_add_flag(avatar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(avatar, _repAvatarColor(si), 0);
        lv_obj_set_style_border_width(avatar, 0, 0);
        lv_obj_set_style_pad_all(avatar, 0, 0);
        lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        char initials[4];
        _repInitials(r.name, initials, sizeof(initials));
        lv_obj_t* iconLbl = lv_label_create(avatar);
        lv_label_set_text(iconLbl, initials);
        lv_obj_set_style_text_color(iconLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(iconLbl, theme::bodyFont12(), 0);
        lv_obj_center(iconLbl);

        // ── Text column: name, then "id | rssi | last seen" subtitle ───────
        lv_obj_t* col = lv_obj_create(row);
        lv_obj_set_height(col, OPS_LIST_COL_H);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 0, 0);
        lv_obj_set_style_pad_row(col, 1, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        // Name line: name, then the favourite star beside it.
        lv_obj_t* nameRow = theme::addNameRow(col);
        lv_obj_t* nameLbl = lv_label_create(nameRow);
        lv_label_set_text(nameLbl, r.name);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nameLbl, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(nameLbl, LV_PCT(85), 0);   // room for the star
        lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, theme::bodyFont12(), 0);

        // ── Favourite star — the compiled gold-star emoji, hidden when unset ─
        lv_obj_t* starLbl = lv_label_create(nameRow);
        lv_label_set_text(starLbl, "\xE2\xAD\x90");  // U+2B50 — matches kOpsEmoji "star gold"
        lv_obj_set_style_text_font(starLbl, theme::bodyFont12(), 0);
        if (!r.favourite) lv_obj_add_flag(starLbl, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* line2 = lv_obj_create(col);
        lv_obj_set_width(line2, LV_PCT(100));
        lv_obj_set_height(line2, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(line2, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(line2, 0, 0);
        lv_obj_set_style_pad_all(line2, 0, 0);
        lv_obj_set_style_pad_column(line2, 4, 0);
        lv_obj_clear_flag(line2, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(line2, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* idLbl = lv_label_create(line2);
        lv_label_set_text(idLbl, idBuf);
        lv_obj_set_style_text_color(idLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(idLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* sep1Lbl = lv_label_create(line2);
        lv_label_set_text(sep1Lbl, "|");
        lv_obj_set_style_text_color(sep1Lbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(sep1Lbl, &lv_font_montserrat_10, 0);

        lv_obj_t* rssiLbl = lv_label_create(line2);
        lv_label_set_text(rssiLbl, rssiBuf);
        lv_obj_set_style_text_color(rssiLbl, rssiCol, 0);
        lv_obj_set_style_text_font(rssiLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* sep2Lbl = lv_label_create(line2);
        lv_label_set_text(sep2Lbl, "|");
        lv_obj_set_style_text_color(sep2Lbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(sep2Lbl, &lv_font_montserrat_10, 0);

        lv_obj_t* timeLbl = lv_label_create(line2);
        lv_label_set_text(timeLbl, timeBuf);
        lv_obj_set_style_text_color(timeLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(timeLbl, &lv_font_montserrat_10, 0);

        // Signal bars (last-heard RSSI)
        theme::addSignalBars(row, r.lastRssi);
    }

    listfilter::apply(s_filter);   // keep the filter across rebuilds
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

    // ── Action box: name + id, then a 2-column grid of big buttons; Admin
    //    Login and Close span both columns (same style as Contacts). ─────
    // Narrow screen (Cardputer): the full width less a margin, one column.
    static constexpr bool NARROW = OPS_SCREEN_W < 300;
    static constexpr int BOX_W  = NARROW ? OPS_SCREEN_W - 6 : 296;
    static constexpr int GAP    = 6;
    static constexpr int BORDER = 1;
    // Content width = box minus padding AND border, or two halves + the gap
    // overflow it and flex wraps every button onto its own row.
    static constexpr int FULL_W = BOX_W - 2 * 8 - 2 * BORDER;
    static constexpr int HALF_W = NARROW ? FULL_W : (FULL_W - GAP) / 2;
    static constexpr int BTN_H  = NARROW ? 26 : 30;

    lv_obj_t* box = lv_obj_create(overlay);
    lv_obj_set_width(box, BOX_W);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, BORDER, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_style_pad_column(box, GAP, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);   // taps on the box don't close it
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (NARROW)
    {
        // One column is taller than the screen: cap the box at the screen
        // height and let it scroll, or the top buttons sit above the edge.
        lv_obj_set_style_max_height(box, OPS_SCREEN_H - 4, 0);
        lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(box, LV_DIR_VER);
    }

    // Title row: name left, id right
    lv_obj_t* head = lv_obj_create(box);
    lv_obj_set_size(head, FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    char shown[32];
    snprintf(shown, sizeof(shown), "%s", r.name);
    theme::sanitizeText(shown);
    lv_obj_t* nameLbl = lv_label_create(head);
    lv_label_set_text(nameLbl, shown);
    lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nameLbl, FULL_W - 80);
    lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(nameLbl, theme::bodyFont12(), 0);
    char idBuf[12];
    snprintf(idBuf, sizeof(idBuf), "%02X%02X%02X%02X",
             r.pubKeyPrefix[0], r.pubKeyPrefix[1], r.pubKeyPrefix[2], r.pubKeyPrefix[3]);
    lv_obj_t* idLbl = lv_label_create(head);
    lv_label_set_text(idLbl, idBuf);
    lv_obj_set_style_text_color(idLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(idLbl, &lv_font_montserrat_10, 0);

    auto makeBtn = [&](const char* label, int w, lv_color_t fg,
                       lv_color_t bg, lv_color_t border,
                       lv_event_cb_t cb, bool enabled = true)
    {
        lv_obj_t* btn = lv_btn_create(box);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, w, BTN_H);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, enabled ? border : theme::BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 2, 0);
        if (enabled) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, overlay);
        else         lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, enabled ? fg : theme::BORDER, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
    };

    static const lv_color_t kAmber = LV_COLOR_MAKE(0xFF, 0xB3, 0x00);
    makeBtn("Admin Login",      FULL_W, theme::TEXT,   theme::PRIMARY, theme::PRIMARY, _onPopupAdmin);
    makeBtn("Add Favourite",    HALF_W, kAmber,        theme::BG, kAmber,        _onPopupFavourite, !r.favourite);
    makeBtn("Remove Favourite", HALF_W, theme::TEXT,   theme::BG, theme::BORDER, _onPopupUnfavourite, r.favourite);
    makeBtn("Set Path",         HALF_W, theme::TEXT,   theme::BG, theme::BORDER, _onPopupSetPath);
    makeBtn("Reset Path",       HALF_W, theme::ORANGE, theme::BG, theme::ORANGE, _onPopupResetPath);
#if OPS_FEAT_TRACE
    makeBtn("Trace Repeater",   HALF_W, theme::ACCENT, theme::BG, theme::ACCENT, _onPopupTrace);
    makeBtn("Delete Repeater",  HALF_W, theme::RED,    theme::BG, theme::RED,    _onPopupDelete);
#else
    makeBtn("Delete Repeater",  FULL_W, theme::RED,    theme::BG, theme::RED,    _onPopupDelete);
#endif
    makeBtn("Close",            FULL_W, theme::TEXT_MUTED, theme::BG, theme::BORDER, _onPopupClose);
}

// ── _onPopupAdmin() — close popup and open admin login dialog ─────────
void ScreenRepeaters::_onPopupAdmin(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // handler runs on one of its children
    // s_pendingRepeater is preserved — dialog will read it
    _showAdminDialog();
}

// ── _onPopupFavourite() — toggle favourite and rebuild list ───────────
static void _setRepFavAndClose(lv_event_t* e, bool fav)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (s_pendingRepeater >= 0) repeaters::setFavourite(s_pendingRepeater, fav);
    // Hide only: show() rebuilds the screen and deletes this overlay with it.
    // (Deferred delete + deferred rebuild ran in the wrong order — LVGL runs
    // the newest async call first — and freed the overlay twice: a crash.)
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    s_pendingRepeater = -1;
    lv_async_call([](void*){ ScreenRepeaters::show(); }, nullptr);
}

void ScreenRepeaters::_onPopupFavourite(lv_event_t* e)   { _setRepFavAndClose(e, true); }
void ScreenRepeaters::_onPopupUnfavourite(lv_event_t* e) { _setRepFavAndClose(e, false); }

// ── _onPopupTrace() — open Trace with this repeater selected ──────────
void ScreenRepeaters::_onPopupTrace(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    Repeater r;
    bool ok = s_pendingRepeater >= 0 && repeaters::get(s_pendingRepeater, r);
    lv_obj_del_async(overlay);   // handler runs on one of its children
    s_pendingRepeater = -1;
    if (!ok) return;
    static uint8_t prefix[4];
    memcpy(prefix, r.pubKeyPrefix, 4);
    // Switch screens next frame, once this button's handler has returned.
#if OPS_FEAT_TRACE
    lv_async_call([](void*) { ScreenTrace::showFor(prefix); }, nullptr);
#endif
}

// ── _onPopupSetPath() — close popup and open set-path dialog ──────────
void ScreenRepeaters::_onPopupSetPath(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // handler runs on one of its children
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
            repeaters::clearPath(s_pendingRepeater);   // or a reboot reloads the stale route
            char buf[64];
            snprintf(buf, sizeof(buf), "[repeaters] Path reset: %s", r.name);
            ScreenTerminal::appendLine(buf);
            OPS_LOG("Repeaters", "Path reset for %s", r.name);
        }
    }
    lv_obj_del_async(overlay);   // handler runs on one of its children
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
    // Hide only: show() rebuilds the screen and deletes this overlay with it.
    // (Deferred delete + deferred rebuild ran in the wrong order — LVGL runs
    // the newest async call first — and freed the overlay twice: a crash.)
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_async_call([](void*){ ScreenRepeaters::show(); }, nullptr);
}

// ── _onPopupClose() ───────────────────────────────────────────────────
void ScreenRepeaters::_onPopupClose(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // handler runs on one of its children
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
    lv_textarea_set_max_length(s_adminPassInput, (uint32_t)crypto::SECRET_MAX - 1);
    lv_textarea_set_placeholder_text(s_adminPassInput, "enter password");
    lv_obj_set_style_bg_color(s_adminPassInput, theme::BG, 0);
    lv_obj_set_style_text_color(s_adminPassInput, theme::TEXT, 0);
    lv_obj_set_style_border_color(s_adminPassInput, theme::BORDER, 0);
    lv_obj_set_style_border_width(s_adminPassInput, 1, 0);
    lv_obj_set_style_radius(s_adminPassInput, 4, 0);
    lv_obj_set_style_pad_all(s_adminPassInput, 4, 0);

    // Pre-fill from the sealed store so a remembered login is just Enter.
    bool remembered = false;
    {
        char stored[crypto::SECRET_MAX] = {};
        if (repeaters::getAdminPassword(s_pendingRepeater, stored, sizeof(stored))) {
            lv_textarea_set_text(s_adminPassInput, stored);
            remembered = true;
        } else if (repeaters::hasAdminPassword(s_pendingRepeater)) {
            // A blob is stored but will not unseal — the storage password was
            // changed without this record being re-keyed, or the SD file was
            // edited. Say so instead of silently offering an empty box.
            lv_label_set_text(hintLbl, "Password: (stored one unreadable)");
        }
        memset(stored, 0, sizeof(stored));
    }

    // Remember tickbox
    s_adminRememberCb = lv_checkbox_create(box);
    lv_checkbox_set_text(s_adminRememberCb, "Remember password");
    lv_obj_set_style_text_color(s_adminRememberCb, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_adminRememberCb, &lv_font_montserrat_10, 0);
    lv_obj_set_style_bg_color(s_adminRememberCb, theme::ACCENT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(s_adminRememberCb, theme::BORDER, LV_PART_INDICATOR);
    if (remembered) lv_obj_add_state(s_adminRememberCb, LV_STATE_CHECKED);
    if (!crypto::ready()) {
        // Nothing to store into — do not offer a tickbox that cannot work.
        lv_obj_add_state(s_adminRememberCb, LV_STATE_DISABLED);
        lv_checkbox_set_text(s_adminRememberCb, "Remember (unavailable)");
    }

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

    // Enter in the password box submits — with a remembered password that is
    // the whole interaction.
    lv_obj_add_event_cb(s_adminPassInput, _onAdminOk, LV_EVENT_READY, overlay);
    lv_group_t* g = lv_group_get_default();
    if (g) lv_group_focus_obj(s_adminPassInput);
}

// ── _onAdminOk() ─────────────────────────────────────────────────────
void ScreenRepeaters::_onAdminOk(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);

    // The dialog is torn down on every path below. It is deleted async
    // because Login/Cancel are children of the overlay they dismiss, and
    // deleting an ancestor of the object LVGL is dispatching on corrupts
    // its group state.
    // Enter (LV_EVENT_READY on the textarea) and the Login button both land
    // here. The async delete leaves the widgets alive for one more frame, so
    // bail out if the dialog has already been dismissed rather than queueing
    // a second delete of the same object.
    if (!s_adminPassInput) return;

    auto closeDialog = [&]() {
        if (overlay) lv_obj_del_async(overlay);
        s_pendingRepeater  = -1;
        s_adminPassInput   = nullptr;
        s_adminRememberCb  = nullptr;
    };

    int  idx = s_pendingRepeater;
    bool remember = s_adminRememberCb &&
                    lv_obj_has_state(s_adminRememberCb, LV_STATE_CHECKED);

    if (idx < 0) { closeDialog(); return; }

    Repeater r;
    if (!repeaters::get(idx, r)) { closeDialog(); return; }

    const char* pass = lv_textarea_get_text(s_adminPassInput);
    bool sent = ops::MeshService::instance().sendRepeaterLogin(r.pubKeyPrefix, pass ? pass : "");
    ScreenTerminal::setAdminTarget(r.pubKeyPrefix, r.name);

    // Save target and password for the login result handler (needed for Retry)
    memcpy(s_adminPrefix, r.pubKeyPrefix, 4);
    strncpy(s_adminName, r.name, sizeof(s_adminName) - 1);
    s_adminName[sizeof(s_adminName) - 1] = '\0';
    strncpy(s_adminPass, pass ? pass : "", sizeof(s_adminPass) - 1);
    s_adminPass[sizeof(s_adminPass) - 1] = '\0';

    // Store or forget before the send result is known: the tickbox records
    // what the user asked for, not whether this particular login worked.
    if (remember && pass && pass[0]) {
        if (!repeaters::setAdminPassword(idx, pass))
            ScreenTerminal::appendLine("[repeaters] Could not store password (crypto unavailable)");
    } else {
        repeaters::setAdminPassword(idx, nullptr);
    }

    closeDialog();

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
    if (!s_adminPassInput) return;            // already dismissed this frame
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (overlay) lv_obj_del_async(overlay);   // see the note in _onAdminOk
    s_pendingRepeater = -1;
    s_adminPassInput  = nullptr;
    s_adminRememberCb = nullptr;
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
    // Show the current route, and pre-fill it below so it can be edited.
    ops::PathInfo curPath{};
    bool havePath = ops::MeshService::instance().getContactPath(r.pubKeyPrefix, curPath)
                    && curPath.known;
    char curHex[129] = {};
    char curLine[96];
    if (!havePath) {
        snprintf(curLine, sizeof(curLine), "Current: none (messages flood)\nHex path bytes, e.g. AABBCCDD:");
    } else if (curPath.direct) {
        snprintf(curLine, sizeof(curLine), "Current: direct (0 hops)\nHex path bytes, e.g. AABBCCDD:");
    } else {
        int nb = curPath.hopCount * curPath.hashSz;
        if (nb > 64) nb = 64;
        for (int b = 0; b < nb; b++) snprintf(curHex + b * 2, 3, "%02X", curPath.bytes[b]);
        snprintf(curLine, sizeof(curLine), "Current: %u hop%s, %u-byte hashes - edit below:",
                 curPath.hopCount, curPath.hopCount == 1 ? "" : "s", curPath.hashSz);
    }
    lv_label_set_text(hintLbl, curLine);
    lv_obj_set_width(hintLbl, 220);
    lv_label_set_long_mode(hintLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hintLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hintLbl, &lv_font_montserrat_10, 0);

    // Hex path textarea
    s_pathInput = lv_textarea_create(box);
    lv_obj_set_size(s_pathInput, 220, 30);
    lv_textarea_set_one_line(s_pathInput, true);
    lv_textarea_set_max_length(s_pathInput, 128);  // up to 64 bytes = 128 hex chars
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

    if (curHex[0]) {
        lv_textarea_set_text(s_pathInput, curHex);
        if (curPath.hashSz == 2) _onHashSz2Click(nullptr);   // select the matching size
    }

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
        lv_obj_del_async(overlay);   // called from its own Save button
        s_pendingRepeater = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    Repeater r;
    if (!repeaters::get(s_pendingRepeater, r)) {
        lv_obj_del_async(overlay);   // called from its own Save button
        s_pendingRepeater = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    const char* hex = lv_textarea_get_text(s_pathInput);
    int hexLen = hex ? (int)strlen(hex) : 0;

    // Validate: must be even, non-empty, and divisible by hash-size * 2
    if (hexLen == 0 || hexLen % 2 != 0) {
        ScreenTerminal::appendLine("[set path] Invalid hex: must be even number of chars");
        lv_obj_del_async(overlay);   // called from its own Save button
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
        lv_obj_del_async(overlay);   // called from its own Save button
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

    lv_obj_del_async(overlay);   // called from its own Save button
    s_pendingRepeater = -1;
    s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
}

// ── _onSetPathCancel() ────────────────────────────────────────────────
void ScreenRepeaters::_onSetPathCancel(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // called from its own Cancel button
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
    if (s_waitOverlay) { lv_obj_del_async(s_waitOverlay);   /* its own Cancel button */ s_waitOverlay = nullptr; }
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
        lv_obj_set_width(box, 290);
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
        lv_obj_set_width(titleLbl, 268);
        lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* errLbl = lv_label_create(box);
        lv_label_set_text(errLbl, "Login failed or timed out.\nCheck the password, or Reset & Retry if "
                                  "the route may have changed (e.g. you moved).");
        lv_obj_set_width(errLbl, 268);
        lv_label_set_long_mode(errLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(errLbl, theme::RED, 0);
        lv_obj_set_style_text_font(errLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* btnRow = lv_obj_create(box);
        lv_obj_set_size(btnRow, 268, 34);
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
            lv_obj_set_size(btn, 84, 30);
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

        // The handlers run on a child of the overlay: delete it async.
        makeErrBtn("Cancel", theme::TEXT_MUTED, theme::BORDER, [](lv_event_t* ev){
            lv_obj_del_async((lv_obj_t*)lv_event_get_user_data(ev));
        });
        makeErrBtn("Retry",         theme::ACCENT, theme::ACCENT, _onRetryLogin);
        makeErrBtn("Reset & Retry", theme::ORANGE, theme::ORANGE, _onResetRetryLogin);
    }
}

// ── _onResetRetryLogin() — forget the route, then retry (floods) ─────
// A stale route (moved, a hop changed) makes every retry go down the same
// dead path. With no route the login floods, and the reply brings a new one.
void ScreenRepeaters::_onResetRetryLogin(lv_event_t* e)
{
    ops::MeshService::instance().resetContactPath(s_adminPrefix);
    int idx;
    if (ops::repeaters::findByKey(s_adminPrefix, &idx)) ops::repeaters::clearPath(idx);
    char buf[80];
    snprintf(buf, sizeof(buf), "[repeaters] Path reset for %s - retrying by flood", s_adminName);
    ScreenTerminal::appendLine(buf);
    _onRetryLogin(e);
}

// ── _onRetryLogin() — re-attempt login with saved credentials ─────────
void ScreenRepeaters::_onRetryLogin(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // this handler runs on one of its buttons

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

static constexpr int ADM_TOP_H  = OPS_TOP_H;
static constexpr int ADM_BTN_H  = 34;   // action dropdown + Open
static constexpr int ADM_RESP_Y = ADM_TOP_H + ADM_BTN_H;
static constexpr int ADM_RESP_H = OPS_SCREEN_H - ADM_RESP_Y;

// Response pane font: Latin-1 Montserrat 12 (degree sign) → Montserrat 12
// (LV_SYMBOL_* icons) → emoji images (thermometer, droplet, gauge).
static const lv_font_t* _adminRespFont()
{
    static lv_font_t s_font;
    static bool s_ready = false;
    if (!s_ready) {
        s_font = font_montserrat_12_ext;
        s_font.fallback = ops::emoji::emojiFont(&lv_font_montserrat_12);
        s_ready = true;
    }
    return &s_font;
}

// Set by Status / Neighbours (which clear the pane): keep the pane at the top.
static bool s_respFromTop = false;
// The pane is showing CLI replies (Clock sync / Send advert). While it shows
// Status or Neighbours, CLI replies are left out: late answers to a closed
// Regions / Access / Password window used to land there.
static bool s_respWantCli = false;

// ── Admin actions (dropdown on the panel; Open runs the selected one) ──
static void _actAccess(lv_event_t*)   { repadmin::openAccess(ScreenRepeaters::adminPrefix(), ScreenRepeaters::adminName()); }
static void _actPassword(lv_event_t*) { repadmin::openPassword(ScreenRepeaters::adminPrefix(), ScreenRepeaters::adminName()); }
static void _actRegions(lv_event_t*)  { repadmin::openRegions(ScreenRepeaters::adminPrefix(), ScreenRepeaters::adminName()); }

const ScreenRepeaters::AdminAction ScreenRepeaters::kAdminActions[] = {
    { LV_SYMBOL_REFRESH  "  Status",          ScreenRepeaters::_onAdminStatus    },
    { LV_SYMBOL_LIST     "  Neighbours",      ScreenRepeaters::_onAdminNbrs      },
    { LV_SYMBOL_LOOP     "  Clock sync",      ScreenRepeaters::_onAdminClockSync },
    { LV_SYMBOL_WIFI     "  Send advert",     ScreenRepeaters::_onAdminAdvert    },
    { LV_SYMBOL_KEYBOARD "  Terminal",        ScreenRepeaters::_onAdminTerminal  },
    { LV_SYMBOL_EYE_OPEN "  Access control",  _actAccess                         },
    { LV_SYMBOL_EDIT     "  Change password", _actPassword                       },
    { LV_SYMBOL_GPS      "  Regions",         _actRegions                        },
    { LV_SYMBOL_POWER    "  Reboot",          ScreenRepeaters::_onAdminReboot    },
};
const int ScreenRepeaters::kAdminActionCount =
    (int)(sizeof(ScreenRepeaters::kAdminActions) / sizeof(ScreenRepeaters::kAdminActions[0]));
lv_obj_t* ScreenRepeaters::s_adminActionDd = nullptr;

void ScreenRepeaters::_onAdminOpen(lv_event_t* e)
{
    if (!s_adminActionDd) return;
    int i = (int)lv_dropdown_get_selected(s_adminActionDd);
    if (i >= 0 && i < kAdminActionCount) kAdminActions[i].cb(e);
}

// ── _showAdminPanel() ─────────────────────────────────────────────────
void ScreenRepeaters::_showAdminPanel()
{
    // Reset response buffer
    s_adminRespBuf[0] = '\0';
    s_respWantCli = false;

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

    // ── Action row: dropdown + Open ───────────────────────────────────
    lv_obj_t* btnRow = lv_obj_create(s_adminScreen);
    lv_obj_set_size(btnRow, OPS_SCREEN_W, ADM_BTN_H);
    lv_obj_align(btnRow, LV_ALIGN_TOP_LEFT, 0, ADM_TOP_H);
    lv_obj_set_style_bg_color(btnRow, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_radius(btnRow, 0, 0);
    lv_obj_set_style_pad_hor(btnRow, 6, 0);
    lv_obj_set_style_pad_ver(btnRow, 2, 0);
    lv_obj_set_style_pad_column(btnRow, 6, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_adminActionDd = lv_dropdown_create(btnRow);
    lv_obj_set_height(s_adminActionDd, 28);
    lv_obj_set_flex_grow(s_adminActionDd, 1);
    {
        static char opts[200];
        size_t n = 0;
        for (int i = 0; i < kAdminActionCount; i++)
            n += (size_t)snprintf(opts + n, sizeof(opts) - n, "%s%s", i ? "\n" : "", kAdminActions[i].label);
        lv_dropdown_set_options_static(s_adminActionDd, opts);
    }
    lv_obj_set_style_bg_color(s_adminActionDd, theme::BG, 0);
    lv_obj_set_style_text_color(s_adminActionDd, theme::TEXT, 0);
    lv_obj_set_style_text_font(s_adminActionDd, &lv_font_montserrat_12, 0);
    lv_obj_set_style_border_color(s_adminActionDd, theme::BORDER, 0);
    lv_obj_set_style_border_color(s_adminActionDd, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_ver(s_adminActionDd, 5, 0);
    if (lv_obj_t* list = lv_dropdown_get_list(s_adminActionDd)) {
        lv_obj_set_style_bg_color(list, theme::BG_CARD, 0);
        lv_obj_set_style_text_color(list, theme::TEXT, 0);
        lv_obj_set_style_text_font(list, &lv_font_montserrat_12, 0);
        lv_obj_set_style_max_height(list, 200, 0);
    }
    lv_obj_add_event_cb(s_adminActionDd, _onAdminKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* openBtn = lv_btn_create(btnRow);
    lv_obj_set_size(openBtn, 70, 28);
    lv_obj_set_style_bg_color(openBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(openBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(openBtn, theme::ACCENT, 0);
    lv_obj_set_style_border_width(openBtn, 1, 0);
    lv_obj_set_style_border_width(openBtn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(openBtn, 4, 0);
    lv_obj_set_style_shadow_width(openBtn, 0, 0);
    lv_obj_add_event_cb(openBtn, _onAdminOpen, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(openBtn, _onAdminKey, LV_EVENT_KEY, nullptr);
    {
        lv_obj_t* lbl = lv_label_create(openBtn);
        lv_label_set_text(lbl, "Open " LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(lbl, theme::ACCENT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
    }

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
    lv_obj_add_flag(respArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(respArea, LV_OBJ_FLAG_USER_1);   // trackball up/down scrolls it (UIScreen)

    s_adminRespLbl = lv_label_create(respArea);
    lv_label_set_text(s_adminRespLbl, "Press Status to query the repeater.");
    lv_label_set_long_mode(s_adminRespLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_adminRespLbl, OPS_SCREEN_W - 10);
    lv_obj_set_style_text_color(s_adminRespLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_adminRespLbl, _adminRespFont(), 0);
    lv_obj_set_style_text_line_space(s_adminRespLbl, 2, 0);
    lv_label_set_recolor(s_adminRespLbl, true);   // status colours, see _fmtAdminLine

    // Wire backspace key to close: add respArea to the default input group so
    // keyboard events reach it, then register the key handler.
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, respArea);
        lv_group_focus_obj(s_adminActionDd);   // pick an action, then Open
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
    s_respWantCli = false;
    if (s_adminRespLbl) {
        s_respFromTop = true;
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
    s_respFromTop = false;   // the reply is appended below
    s_respWantCli = true;
    bool ok = ops::MeshService::instance().sendAdminCommand(s_adminPrefix, "clock sync");
    OPS_LOG("Admin", "Clock sync %s to %s", ok ? "sent" : "FAILED", s_adminName);
}

// ── _onAdminAdvert() — ask repeater to flood-advertise itself ─────────
void ScreenRepeaters::_onAdminAdvert(lv_event_t* /*e*/)
{
    uint32_t now = (uint32_t)millis();
    if (now - s_lastStatusMs < 2000) return;
    s_lastStatusMs = now;
    s_respFromTop = false;
    s_respWantCli = true;
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
    s_respWantCli = false;
    if (s_adminRespLbl) {
        s_respFromTop = true;
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
    repadmin::forget();   // its windows live on the admin screen
    s_adminActionDd = nullptr;

    // Open admin terminal (loads new screen over the admin panel).
    ScreenTerminal::showAdmin(s_adminPrefix, s_adminName);

    // Now safe to delete the admin panel — it is no longer the active screen.
    lv_obj_t* dying = s_adminScreen;
    s_adminScreen = nullptr;
    if (dying) lv_obj_del_async(dying);   // this handler runs on one of its buttons
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
    if (overlay) lv_obj_del_async(overlay);   // its own button
}

// ── _onAdminRebootConfirm() ───────────────────────────────────────────
void ScreenRepeaters::_onAdminRebootConfirm(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (overlay) lv_obj_del_async(overlay);   // its own button

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
    repadmin::forget();   // its windows live on the admin screen
    s_adminActionDd = nullptr;

    lv_obj_t* dying = s_adminScreen;
    s_adminScreen = nullptr;

    ScreenRepeaters::show();   // loads new screen first

    // Deferred: this handler runs on a button inside the dying screen.
    if (dying) lv_obj_del_async(dying);
}

// ── Admin panel line formatting ───────────────────────────────────────
// The label uses LVGL recolour ("#RRGGBB text#"), so a literal '#' in a
// reply is doubled. Status lines from MeshService get an icon / colours.

static void _hexOf(lv_color_t c, char* out)   // out: 7 bytes
{
    uint32_t v = lv_color_to32(c) & 0xFFFFFF;
    snprintf(out, 7, "%06X", (unsigned)v);
}

static void _catPlain(char* out, size_t max, const char* text);

// Appends "#RRGGBB text#" to out. A '#' can't be escaped inside a colour
// span, so such text goes in uncoloured.
static void _catColored(char* out, size_t max, lv_color_t c, const char* text)
{
    if (strchr(text, '#')) { _catPlain(out, max, text); return; }
    char hex[7];
    _hexOf(c, hex);
    size_t n = strlen(out);
    if (n < max) snprintf(out + n, max - n, "#%s %s#", hex, text);
}

static void _catPlain(char* out, size_t max, const char* text)
{
    size_t n = strlen(out);
    while (*text && n + 2 < max) {
        if (*text == '#') out[n++] = '#';   // escape for recolour
        out[n++] = *text++;
    }
    out[n] = '\0';
}

static lv_color_t _rssiColor(int rssi)
{
    return rssi >= -90 ? theme::GREEN : rssi >= -105 ? theme::ORANGE : theme::RED;
}
static lv_color_t _snrColor(float snr)
{
    return snr >= 5.0f ? theme::GREEN : snr >= -5.0f ? theme::ORANGE : theme::RED;
}
static lv_color_t _noiseColor(int nf)
{
    return nf <= -110 ? theme::GREEN : nf <= -100 ? theme::ORANGE : theme::RED;
}

static void _fmtAdminLine(const char* text, char* out, size_t max)
{
    out[0] = '\0';
    char tmp[48];

    if (strcmp(text, "--- Status ---") == 0) return;   // the panel title says it

    if (strncmp(text, "Batt: ", 6) == 0) {
        const char* val = text + 6;
        const char* pc  = strchr(val, '(');
        int pct = pc ? atoi(pc + 1) : -1;
        const char* sym = LV_SYMBOL_BATTERY_EMPTY;
        lv_color_t  col = theme::TEXT_MUTED;
        if (pct >= 0 && strcmp(val, "n/a") != 0) {
            sym = pct >= 80 ? LV_SYMBOL_BATTERY_FULL
                : pct >= 55 ? LV_SYMBOL_BATTERY_3
                : pct >= 30 ? LV_SYMBOL_BATTERY_2
                : pct >= 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
            col = pct >= 30 ? theme::GREEN : pct >= 10 ? theme::ORANGE : theme::RED;
        }
        // Header line (icon coloured by charge), then the reading.
        _catColored(out, max, col, sym);
        _catColored(out, max, theme::ACCENT, " Battery");
        _catPlain(out, max, "\n");
        _catPlain(out, max, val);
        return;
    }

    // Section headers with a symbol (Montserrat has no plane: the
    // location arrow stands in for Airtime).
    static const struct { const char* name; const char* sym; } kHeaders[] = {
        { "Radio",   LV_SYMBOL_WIFI    },
        { "Packets", LV_SYMBOL_SHUFFLE },
        { "Airtime", LV_SYMBOL_GPS     },
        { "Uptime",  LV_SYMBOL_UP      },
    };
    for (const auto& h : kHeaders) {
        if (strcmp(text, h.name) == 0) {
            snprintf(tmp, sizeof(tmp), "%s %s", h.sym, h.name);
            _catColored(out, max, theme::ACCENT, tmp);
            return;
        }
    }

    // Sensor telemetry (MeshService::_emitTelemetry): emoji header, value below.
    float fv = 0.0f;
    if (sscanf(text, "Temp: %f", &fv) == 1) {
        _catPlain(out, max, "\xF0\x9F\x8C\xA1");                 // U+1F321 thermometer
        _catColored(out, max, theme::ACCENT, " Temperature");
        snprintf(tmp, sizeof(tmp), "\n%.1f\xC2\xB0" "C   %.1f\xC2\xB0" "F",
                 fv, fv * 9.0f / 5.0f + 32.0f);
        _catPlain(out, max, tmp);
        return;
    }
    if (sscanf(text, "Humidity: %f", &fv) == 1) {
        _catPlain(out, max, "\xF0\x9F\x92\xA7");                 // U+1F4A7 droplet
        _catColored(out, max, theme::ACCENT, " Humidity");
        snprintf(tmp, sizeof(tmp), "\n%.1f%%", fv);
        _catPlain(out, max, tmp);
        return;
    }
    if (sscanf(text, "Pressure: %f", &fv) == 1) {
        _catPlain(out, max, "\xE2\x8F\xB2");                      // U+23F2 timer clock (gauge)
        _catColored(out, max, theme::ACCENT, " Pressure");
        snprintf(tmp, sizeof(tmp), "\n%.1f hPa", fv);
        _catPlain(out, max, tmp);
        return;
    }

    int rssi = 0, noise = 0;
    float snr = 0.0f;
    if (sscanf(text, "RSSI: %d SNR: %f Noise: %d", &rssi, &snr, &noise) == 3) {
        _catPlain(out, max, "RSSI ");
        snprintf(tmp, sizeof(tmp), "%d dBm", rssi);
        _catColored(out, max, _rssiColor(rssi), tmp);
        _catPlain(out, max, "   SNR ");
        snprintf(tmp, sizeof(tmp), "%.2f dB", snr);
        _catColored(out, max, _snrColor(snr), tmp);
        _catPlain(out, max, "   Noise ");
        snprintf(tmp, sizeof(tmp), "%d dBm", noise);
        _catColored(out, max, _noiseColor(noise), tmp);
        return;
    }

    // "Nbr AA:BB:CC:DD  5m ago  SNR:-3.25" → prefix, repeater name, coloured SNR
    unsigned b[4];
    char ago[16];
    if (sscanf(text, "Nbr %2X:%2X:%2X:%2X %15s ago SNR:%f",
               &b[0], &b[1], &b[2], &b[3], ago, &snr) == 6) {
        uint8_t pfx[4] = { (uint8_t)b[0], (uint8_t)b[1], (uint8_t)b[2], (uint8_t)b[3] };
        static Repeater r;   // large record: keep it off the stack
        int idx = -1;
        bool known = ops::repeaters::findByKey(pfx, &idx) && ops::repeaters::get(idx, r) && r.name[0];
        // The name when it is one of our repeaters, otherwise the id.
        if (known) {
            _catPlain(out, max, r.name);
        } else {
            snprintf(tmp, sizeof(tmp), "%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3]);
            _catColored(out, max, theme::TEXT_MUTED, tmp);
        }
        snprintf(tmp, sizeof(tmp), "  %s ago  SNR ", ago);
        _catPlain(out, max, tmp);
        snprintf(tmp, sizeof(tmp), "%.2f dB", snr);
        _catColored(out, max, _snrColor(snr), tmp);
        return;
    }

    _catPlain(out, max, text);
}

// ── onContactResponse() — append to admin panel if open ───────────────
void ScreenRepeaters::onContactResponse(const char* line, bool isCli)
{
    if (!s_adminRespLbl) return;

    // Strip the "[Name] " prefix if present so the panel stays clean
    const char* raw = line;
    const char* bracket = strstr(line, "] ");
    if (bracket) raw = bracket + 2;

    // An open Access / Password / Regions window takes its replies, and they
    // stay out of the pane; so do stray CLI replies while it shows a query.
    if (repadmin::onResponse(raw)) return;
    if (isCli && !s_respWantCli) return;
    if (strncmp(raw, "ACL ", 4) == 0) return;   // late access list, window gone

    char text[320];
    _fmtAdminLine(raw, text, sizeof(text));
    if (!text[0]) return;

    int used = (int)strlen(s_adminRespBuf);
    int remaining = (int)sizeof(s_adminRespBuf) - used - 1;
    if (remaining < (int)strlen(text) + 1) {
        // Scroll buffer: drop first half to make room
        int half = (int)sizeof(s_adminRespBuf) / 2;
        memmove(s_adminRespBuf, s_adminRespBuf + half,
                sizeof(s_adminRespBuf) - half);
        s_adminRespBuf[sizeof(s_adminRespBuf) - half - 1] = '\0';
        // Start at a line boundary so no colour tag is cut in half.
        char* nl = strchr(s_adminRespBuf, '\n');
        if (nl) memmove(s_adminRespBuf, nl + 1, strlen(nl + 1) + 1);
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

    // A Status / Neighbours reply is read from its first line; anything
    // else (command replies) follows the newest line.
    lv_obj_t* parent = lv_obj_get_parent(s_adminRespLbl);
    if (parent) lv_obj_scroll_to_y(parent, s_respFromTop ? 0 : LV_COORD_MAX, LV_ANIM_OFF);
}

}}  // namespace ops::ui
