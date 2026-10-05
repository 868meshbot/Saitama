// Saitama — ScreenContacts.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenContacts.h"
#include "ScreenLauncher.h"
#include "ScreenHome.h"
#include "ScreenTerminal.h"
#include "QRPopup.h"
#include "Theme.h"
#include "ListFilter.h"
#include "../mesh/MeshService.h"
#include "../utils/Contacts.h"
#include "../utils/Config.h"
#include "../utils/Log.h"
#include "../hardware/Board.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <time.h>

namespace ops { namespace ui {

lv_obj_t* ScreenContacts::_screen = nullptr;

static constexpr int TOP_H = 28;
// Type-to-filter box in the top bar: matches name or id.
static listfilter::State s_filter;
static void _filterRowText(int key, char* out, size_t outMax)
{
    Contact x;
    if (!contacts::get(key, x)) { out[0] = '\0'; return; }
    snprintf(out, outMax, "%s %02X%02X%02X%02X", x.name,
             x.pubKeyPrefix[0], x.pubKeyPrefix[1], x.pubKeyPrefix[2], x.pubKeyPrefix[3]);
}

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

// ── Row styling helpers (matches ScreenHome's channel-list look) ───────

// Up to 2 uppercase alnum initials, auto-derived from the contact name.
static void _initials(const char* name, char* out, int outSize)
{
    int n = 0;
    for (const char* p = name; *p && n < 2 && n < outSize - 1; p++) {
        if (isalnum((unsigned char)*p)) out[n++] = (char)toupper((unsigned char)*p);
    }
    if (n == 0) { out[0] = '#'; n = 1; }
    out[n] = '\0';
}

// Deterministic-but-varied avatar background colour per row.
static lv_color_t _avatarColor(int idx)
{
    return lv_color_hsv_to_rgb((uint16_t)(((idx + 1) * 53) % 360), 45, 60);
}

static double _haversineKm(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double kEarthRadiusKm = 6371.0;
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    double dLat = (lat2 - lat1) * kDegToRad;
    double dLon = (lon2 - lon1) * kDegToRad;
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1 * kDegToRad) * cos(lat2 * kDegToRad) *
               sin(dLon / 2) * sin(dLon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return kEarthRadiusKm * c;
}

// "--" when either end's position is unknown; "142m" / "3.4km" / "120km" otherwise.
static void fmtDistance(const Contact& c, char* buf, size_t len)
{
    if (c.lat == 0 && c.lon == 0) { snprintf(buf, len, "--"); return; }

    auto& b = ops::Board::instance();
    double slat, slng;
    if (b.hasGPSFix()) {
        slat = b.gpsLat(); slng = b.gpsLng();
    } else {
        const auto& cfg = ops::config::get();
        if (cfg.manualLat == 0.0f && cfg.manualLon == 0.0f) { snprintf(buf, len, "--"); return; }
        slat = cfg.manualLat; slng = cfg.manualLon;
    }

    double km = _haversineKm(slat, slng, (double)c.lat / 1e6, (double)c.lon / 1e6);
    if (km < 1.0)        snprintf(buf, len, "%.0fm", km * 1000.0);
    else if (km < 100.0) snprintf(buf, len, "%.1fkm", km);
    else                 snprintf(buf, len, "%.0fkm", km);
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
    s_filter.rowText = _filterRowText;
    s_filter.onEsc   = [] { ScreenLauncher::show(); };
    listfilter::create(bar, s_filter);

    // ── Contact list ──────────────────────────────────────────────────
    lv_obj_t* list = lv_obj_create(_screen);
    s_filter.list = list;
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

    static constexpr int kRowH = 60;  // matches ScreenHome's channel-list row height

    for (int vi = 0; vi < cnt; vi++) {
        int si = s_order[vi];   // storage index — passed as user_data
        Contact c;
        if (!contacts::get(si, c)) continue;

        char idBuf[10];
        snprintf(idBuf, sizeof(idBuf), "%02X%02X%02X%02X",
                 c.pubKeyPrefix[0], c.pubKeyPrefix[1], c.pubKeyPrefix[2], c.pubKeyPrefix[3]);

        char distBuf[12];
        fmtDistance(c, distBuf, sizeof(distBuf));

        char timeBuf[20];
        fmtDateTime(c.lastSeen, timeBuf, sizeof(timeBuf));

        char subtitle[48];
        snprintf(subtitle, sizeof(subtitle), "%s | %s | %s", idBuf, distBuf, timeBuf);

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

        // ── Avatar: auto-derived initials on a hashed-colour circle ────────
        lv_obj_t* avatar = lv_obj_create(row);
        lv_obj_set_size(avatar, 44, 44);
        lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(avatar, _avatarColor(si), 0);
        lv_obj_set_style_border_width(avatar, 0, 0);
        lv_obj_set_style_pad_all(avatar, 0, 0);
        lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        char initials[4];
        _initials(c.name, initials, sizeof(initials));
        lv_obj_t* iconLbl = lv_label_create(avatar);
        lv_label_set_text(iconLbl, initials);
        lv_obj_set_style_text_color(iconLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(iconLbl, theme::bodyFont12(), 0);
        lv_obj_center(iconLbl);

        // ── Text column: name + "id | distance | last seen" subtitle ───────
        lv_obj_t* col = lv_obj_create(row);
        lv_obj_set_height(col, 44);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 0, 0);
        lv_obj_set_style_pad_row(col, 1, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        lv_obj_t* nameLbl = lv_label_create(col);
        lv_label_set_text(nameLbl, c.name);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nameLbl, LV_PCT(100));
        lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, theme::bodyFont12(), 0);

        lv_obj_t* subLbl = lv_label_create(col);
        lv_label_set_text(subLbl, subtitle);
        lv_label_set_long_mode(subLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(subLbl, LV_PCT(100));
        lv_obj_set_style_text_color(subLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(subLbl, &lv_font_montserrat_10, 0);

        // ── Unread-count blob (hidden entirely when there's nothing unread) ─
        lv_obj_t* blob = lv_obj_create(row);
        lv_obj_set_size(blob, LV_SIZE_CONTENT, 22);
        lv_obj_set_style_min_width(blob, 22, 0);
        lv_obj_set_style_radius(blob, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(blob, theme::RED, 0);
        lv_obj_set_style_bg_opa(blob, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(blob, 0, 0);
        lv_obj_set_style_pad_hor(blob, 4, 0);
        lv_obj_set_style_pad_ver(blob, 0, 0);
        lv_obj_clear_flag(blob, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        if (c.unreadCount > 0) {
            char cntBuf[6];
            if (c.unreadCount > 99) snprintf(cntBuf, sizeof(cntBuf), "99+");
            else                    snprintf(cntBuf, sizeof(cntBuf), "%u", (unsigned)c.unreadCount);
            lv_obj_t* blobLbl = lv_label_create(blob);
            lv_label_set_text(blobLbl, cntBuf);
            lv_obj_set_style_text_color(blobLbl, theme::BG, 0);
            lv_obj_set_style_text_font(blobLbl, &lv_font_montserrat_10, 0);
            lv_obj_center(blobLbl);
        } else {
            lv_obj_add_flag(blob, LV_OBJ_FLAG_HIDDEN);
        }

        // ── Signal bars (last-heard RSSI), left of the star ───────────────
        theme::addSignalBars(row, c.lastRssi);

        // ── Favourite star — the compiled gold-star emoji, hidden when unset ─
        lv_obj_t* starLbl = lv_label_create(row);
        lv_label_set_text(starLbl, "\xE2\xAD\x90");  // U+2B50 — matches kOpsEmoji "star gold"
        lv_obj_set_style_text_font(starLbl, theme::bodyFont12(), 0);
        if (!c.favourite || c.blocked) lv_obj_add_flag(starLbl, LV_OBJ_FLAG_HIDDEN);

        // ── Blocked: red no-entry sign in the star's place ─────────────────
        // Drawn (red disc + white bar) — the emoji set has no no-entry glyph.
        if (c.blocked) {
            lv_obj_t* sign = lv_obj_create(row);
            lv_obj_set_size(sign, 16, 16);
            lv_obj_set_style_radius(sign, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(sign, theme::RED, 0);
            lv_obj_set_style_bg_opa(sign, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(sign, 0, 0);
            lv_obj_set_style_pad_all(sign, 0, 0);
            lv_obj_clear_flag(sign, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* bar = lv_obj_create(sign);
            lv_obj_set_size(bar, 10, 3);
            lv_obj_set_style_radius(bar, 1, 0);
            lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_center(bar);
        }
    }

    listfilter::apply(s_filter);   // keep the filter across rebuilds
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

    // ── Action box: name + id, then a 2-column grid of big buttons; Direct
    //    Message and Close span both columns. ──────────────────────────
    static constexpr int BOX_W  = 296;
    static constexpr int GAP    = 6;
    static constexpr int BORDER = 1;
    // Content width = box minus padding AND border; two halves + the gap
    // must fit in it or flex wraps every button onto its own row.
    static constexpr int FULL_W = BOX_W - 2 * 8 - 2 * BORDER;
    static constexpr int HALF_W = (FULL_W - GAP) / 2;
    static constexpr int BTN_H  = 30;

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

    // Title row: callsign left, id right
    lv_obj_t* head = lv_obj_create(box);
    lv_obj_set_size(head, FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    char shown[32];
    snprintf(shown, sizeof(shown), "%s", c.name);
    theme::sanitizeText(shown);
    lv_obj_t* nameLbl = lv_label_create(head);
    lv_label_set_text(nameLbl, shown);
    lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nameLbl, FULL_W - 80);
    lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(nameLbl, theme::bodyFont12(), 0);
    char idBuf[12];
    snprintf(idBuf, sizeof(idBuf), "%02X%02X%02X%02X",
             c.pubKeyPrefix[0], c.pubKeyPrefix[1], c.pubKeyPrefix[2], c.pubKeyPrefix[3]);
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
    makeBtn("Direct Message",   FULL_W, theme::TEXT,   theme::PRIMARY, theme::PRIMARY, _onPopupDM);
    makeBtn("Share QR",         HALF_W, theme::ACCENT, theme::BG, theme::ACCENT, _onPopupShareQR);
    makeBtn("Ping",             HALF_W, theme::ACCENT, theme::BG, theme::ACCENT, _onPopupPing);
    makeBtn("Favourite",        HALF_W, kAmber,        theme::BG, kAmber,        _onPopupFavourite, !c.favourite);
    makeBtn("Remove Favourite", HALF_W, theme::TEXT,   theme::BG, theme::BORDER, _onPopupUnfavourite, c.favourite);
    makeBtn("Set Path",         HALF_W, theme::TEXT,   theme::BG, theme::BORDER, _onPopupSetPath);
    makeBtn("Reset Path",       HALF_W, theme::ORANGE, theme::BG, theme::ORANGE, _onPopupResetPath);
    makeBtn("Delete Contact",   HALF_W, theme::RED,    theme::BG, theme::RED,    _onPopupDelete);
    makeBtn(c.blocked ? "Unblock Contact" : "Block Contact",
                                HALF_W, theme::RED,    theme::BG, theme::RED,    _onPopupBlock);
    makeBtn("Close",            FULL_W, theme::TEXT_MUTED, theme::BG, theme::BORDER, _onPopupClose);
}

// ── _onPopupDM() ──────────────────────────────────────────────────────
void ScreenContacts::_onPopupDM(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // handler runs on one of its children
    Contact c;
    bool ok = (s_pendingContact >= 0) && contacts::get(s_pendingContact, c);
    s_pendingContact = -1;
    if (ok)
        ScreenHome::openDM(c.pubKeyPrefix, c.name);
    else
        ScreenHome::show();
}

// ── _onPopupFavourite() / _onPopupUnfavourite() — set and rebuild list ─
static void _setFavAndClose(lv_event_t* e, bool fav)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (s_pendingContact >= 0) contacts::setFavourite(s_pendingContact, fav);
    // Hide only: show() rebuilds the screen and deletes this overlay with it.
    // (Deferred delete + deferred rebuild ran in the wrong order — LVGL runs
    // the newest async call first — and freed the overlay twice: a crash.)
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    s_pendingContact = -1;
    lv_async_call([](void*){ ScreenContacts::show(); }, nullptr);
}

void ScreenContacts::_onPopupUnfavourite(lv_event_t* e) { _setFavAndClose(e, false); }

// ── _onPopupBlock() — toggle blocked: messages hidden in chat ─────────
void ScreenContacts::_onPopupBlock(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (s_pendingContact >= 0) {
        Contact c;
        if (contacts::get(s_pendingContact, c)) {
            contacts::setBlocked(s_pendingContact, !c.blocked);
            char buf[64];
            snprintf(buf, sizeof(buf), "[contacts] %s %s", c.name,
                     c.blocked ? "unblocked" : "blocked - messages hidden");
            ScreenTerminal::appendLine(buf);
            ScreenLauncher::refreshRain();
        }
    }
    // Hide only: show() rebuilds the screen and deletes this overlay with it.
    // (Deferred delete + deferred rebuild ran in the wrong order — LVGL runs
    // the newest async call first — and freed the overlay twice: a crash.)
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    s_pendingContact = -1;
    lv_async_call([](void*){ ScreenContacts::show(); }, nullptr);
}

void ScreenContacts::_onPopupFavourite(lv_event_t* e) { _setFavAndClose(e, true); }

// ── _onPopupSetPath() — close popup and open set-path dialog ──────────
void ScreenContacts::_onPopupSetPath(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // handler runs on one of its children
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
            contacts::clearPath(s_pendingContact);   // or a reboot reloads the stale route
            char buf[64];
            snprintf(buf, sizeof(buf), "[contacts] Path reset: %s", c.name);
            ScreenTerminal::appendLine(buf);
            OPS_LOG("Contacts", "Path reset for %s", c.name);
        }
    }
    lv_obj_del_async(overlay);   // handler runs on one of its children
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
    // Hide only: show() rebuilds the screen and deletes this overlay with it.
    // (Deferred delete + deferred rebuild ran in the wrong order — LVGL runs
    // the newest async call first — and freed the overlay twice: a crash.)
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_async_call([](void*){ ScreenContacts::show(); }, nullptr);
}

// ── Ping ─────────────────────────────────────────────────────────────
// A TRACE along the contact's known route, out and back (see
// MeshService::sendTrace). Shows round-trip time, hop count and the weakest
// link's SNR. Polled by a short-lived LVGL timer while the box is open.

static lv_obj_t*  s_pingBox   = nullptr;   // overlay
static lv_obj_t*  s_pingLbl   = nullptr;
static lv_timer_t* s_pingTimer = nullptr;
static uint32_t   s_pingTag   = 0;
static uint32_t   s_pingSent  = 0;
static int        s_pingHops  = 0;
static char       s_pingName[32];
static constexpr uint32_t PING_TIMEOUT_MS = 20000;

static void _pingStop()
{
    if (s_pingTimer) { lv_timer_del(s_pingTimer); s_pingTimer = nullptr; }
    s_pingTag = 0;
}

static void _onPingClose(lv_event_t* /*e*/)
{
    _pingStop();
    if (s_pingBox) lv_obj_del_async(s_pingBox);   // called from its own button
    s_pingBox = nullptr;
    s_pingLbl = nullptr;
}

static void _pingSet(const char* text, lv_color_t col)
{
    if (!s_pingLbl) return;
    lv_label_set_text(s_pingLbl, text);
    lv_obj_set_style_text_color(s_pingLbl, col, 0);
}

static void _pingTick(lv_timer_t*)
{
    // The box goes with the Contacts screen if that is rebuilt meanwhile.
    if (!s_pingLbl || !lv_obj_is_valid(s_pingLbl)) {
        s_pingBox = s_pingLbl = nullptr;
        _pingStop();
        return;
    }
    ops::TraceResult r;
    while (ops::MeshService::instance().pollTraceResult(r)) {
        if (r.tag != s_pingTag) continue;
        uint32_t rtt = millis() - s_pingSent;
        float worst = r.rxSnr / 4.0f;
        for (int i = 0; i < r.numSnrs && i < 64; i++)
            if (r.snrs[i] / 4.0f < worst) worst = r.snrs[i] / 4.0f;
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "%s replied in %.1f s\n%d hop%s out, back the same way\n"
                 "Weakest link: %.1f dB SNR",
                 s_pingName, rtt / 1000.0f, s_pingHops, s_pingHops == 1 ? "" : "s", (double)worst);
        _pingSet(buf, theme::GREEN);
        _pingStop();
        return;
    }
    if (millis() - s_pingSent > PING_TIMEOUT_MS) {
        char buf[120];
        snprintf(buf, sizeof(buf),
                 "No reply from %s in %lu s.\nThe route may have changed - try Reset Path, "
                 "then send a DM.", s_pingName, (unsigned long)(PING_TIMEOUT_MS / 1000));
        _pingSet(buf, theme::ORANGE);
        _pingStop();
    }
}

static void _openPingBox(const char* text, lv_color_t col)
{
    s_pingBox = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_pingBox, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_pingBox, 0, 0);
    lv_obj_set_style_bg_color(s_pingBox, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_pingBox, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_pingBox, 0, 0);
    lv_obj_clear_flag(s_pingBox, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(s_pingBox);
    lv_obj_set_size(box, 270, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::ACCENT, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(box);
    lv_label_set_text(title, "Ping");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    s_pingLbl = lv_label_create(box);
    lv_obj_set_width(s_pingLbl, 250);
    lv_label_set_long_mode(s_pingLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_pingLbl, theme::bodyFont12(), 0);
    _pingSet(text, col);

    lv_obj_t* btn = lv_btn_create(box);
    lv_group_remove_obj(btn);
    lv_obj_set_size(btn, 250, 30);
    lv_obj_set_style_bg_color(btn, theme::BG, 0);
    lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, theme::BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, _onPingClose, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(btn);
    lv_label_set_text(l, "Close");
    lv_obj_set_style_text_color(l, theme::TEXT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);
}

void ScreenContacts::_onPopupPing(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // handler runs on one of its children
    Contact c;
    bool ok = s_pendingContact >= 0 && contacts::get(s_pendingContact, c);
    s_pendingContact = -1;
    if (!ok) return;

    _pingStop();
    snprintf(s_pingName, sizeof(s_pingName), "%s", c.name);
    theme::sanitizeText(s_pingName);
    auto& mesh = ops::MeshService::instance();
    char buf[160];
    if (!mesh.hasPathTo(c.pubKeyPrefix)) {
        snprintf(buf, sizeof(buf),
                 "No route to %s is known yet.\nSend a DM first (its reply sets the route), "
                 "or use Set Path.", s_pingName);
        _openPingBox(buf, theme::ORANGE);
        return;
    }
    uint32_t tag = 0;
    int nodes = 0;
    if (!mesh.sendTrace(c.pubKeyPrefix, tag, nodes)) {
        snprintf(buf, sizeof(buf), "Couldn't send a ping to %s.", s_pingName);
        _openPingBox(buf, theme::RED);
        return;
    }
    s_pingTag  = tag;
    s_pingSent = millis();
    s_pingHops = nodes;
    snprintf(buf, sizeof(buf), "Pinging %s...", s_pingName);
    _openPingBox(buf, theme::TEXT_MUTED);
    s_pingTimer = lv_timer_create(_pingTick, 100, nullptr);
}

// ── _onPopupClose() ───────────────────────────────────────────────────
void ScreenContacts::_onPopupClose(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // handler runs on one of its children
    s_pendingContact = -1;
}

// ── URL-encode a string (spaces→+, others→%XX) ───────────────────────
static void _urlEncode(const char* src, char* dst, int dstMax)
{
    int j = 0;
    for (int i = 0; src[i] && j < dstMax - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else if (c == ' ') {
            dst[j++] = '+';
        } else if (j < dstMax - 3) {
            snprintf(dst + j, 4, "%%%02X", c);
            j += 3;
        }
    }
    dst[j] = '\0';
}

// ── _onPopupShareQR() — encode contact as QR code ────────────────────
void ScreenContacts::_onPopupShareQR(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // handler runs on one of its children

    Contact c;
    if (s_pendingContact < 0 || !contacts::get(s_pendingContact, c)) {
        s_pendingContact = -1;
        return;
    }
    s_pendingContact = -1;

    char hexBuf[65] = {};
    bool hasKey = false;
    for (int i = 0; i < 32; i++) if (c.pubKey[i]) { hasKey = true; break; }

    char encodedName[96];
    _urlEncode(c.name, encodedName, sizeof(encodedName));

    char data[160];
    if (hasKey) {
        for (int i = 0; i < 32; i++)
            snprintf(hexBuf + i * 2, 3, "%02x", c.pubKey[i]);
        snprintf(data, sizeof(data),
                 "meshcore://contact/add?name=%s&public_key=%s&type=1",
                 encodedName, hexBuf);
    } else {
        // Full key not yet received — use Saitama prefix-only fallback
        snprintf(hexBuf, 9, "%02x%02x%02x%02x",
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
        lv_obj_del_async(overlay);   // called from its Save button
        s_pendingContact = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    Contact c;
    if (!contacts::get(s_pendingContact, c)) {
        lv_obj_del_async(overlay);   // called from its Save button
        s_pendingContact = -1;
        s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
        return;
    }

    const char* hex = lv_textarea_get_text(s_pathInput);
    int hexLen = hex ? (int)strlen(hex) : 0;

    if (hexLen == 0 || hexLen % 2 != 0) {
        ScreenTerminal::appendLine("[set path] Invalid hex: must be even number of chars");
        lv_obj_del_async(overlay);   // called from its Save button
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
        lv_obj_del_async(overlay);   // called from its Save button
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

    lv_obj_del_async(overlay);   // handler runs on one of its children
    s_pendingContact = -1;
    s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
}

// ── _onSetPathCancel() ────────────────────────────────────────────────
void ScreenContacts::_onSetPathCancel(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del_async(overlay);   // handler runs on one of its children
    s_pendingContact = -1;
    s_pathInput = s_hashSz1Btn = s_hashSz2Btn = nullptr;
}

}}  // namespace ops::ui
