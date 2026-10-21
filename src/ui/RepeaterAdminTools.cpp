// Saitama — RepeaterAdminTools.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Change Password / Access Control / Regions windows for the repeater admin
// panel. See RepeaterAdminTools.h.
//
// Repeater side (MeshCore simple_repeater, admin login):
//   password <pw>              → "password now: <pw>"
//   set guest.password <pw>    → "OK"
//   REQ_TYPE_GET_ACCESS_LIST   → 6-byte key prefix + permissions per client
//   setperm <hex> <perms>      → "OK" / "Err - ..."; role 0 (guest) removes the
//                                entry and accepts a key prefix, any other role
//                                needs the full 64-hex key
//   region                     → "*[ F]" then one line per region, indented by
//                                depth: "<name>[^][ F]" (^ = home, F = flood allowed)
//   region default [<name>]    → " default scope is <name>"
//   region put <name> [parent] / remove <name> / allowf|denyf <name>
//   region save                → "OK" (otherwise changes are lost on reboot)
// A CLI reply is at most ~160 characters, so a long region list arrives cut off.

#include "RepeaterAdminTools.h"
#include "Theme.h"
#include "ListFilter.h"
#include "../mesh/MeshService.h"
#include "../utils/Contacts.h"
#include "../utils/Repeaters.h"
#include "../utils/Log.h"
#include "../utils/PsramAlloc.h"
#include <lvgl.h>
#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace ops { namespace ui { namespace repadmin {

// ── Shared window frame ───────────────────────────────────────────────

enum Tool { T_NONE, T_PASS, T_ACL, T_REG };

static constexpr int BAR_H    = 28;
static constexpr int STATUS_H = 18;

static Tool      s_tool      = T_NONE;
static lv_obj_t* s_win       = nullptr;   // full-screen window
static lv_obj_t* s_body      = nullptr;   // area between bar and status line
static lv_obj_t* s_status    = nullptr;   // bottom status line
static lv_obj_t* s_prevFocus = nullptr;   // admin panel's focused object
static lv_timer_t* s_timeout = nullptr;   // "no reply" timer
static uint8_t   s_prefix[4] = {};
static char      s_name[32]  = {};

static void _close();

static void _setStatus(const char* text, lv_color_t col)
{
    if (!s_status) return;
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, col, 0);
}

static void _onTimeout(lv_timer_t* /*t*/)
{
    s_timeout = nullptr;   // one-shot: LVGL deletes it after this call
    _setStatus("No reply - check the login and try again", theme::RED);
}

// Status "waiting" + a 25 s no-reply timer (replies come back by flood).
static void _expectReply(const char* text)
{
    _setStatus(text, theme::TEXT_MUTED);
    if (s_timeout) lv_timer_del(s_timeout);
    s_timeout = lv_timer_create(_onTimeout, 25000, nullptr);
    lv_timer_set_repeat_count(s_timeout, 1);
}

static void _gotReply()
{
    if (s_timeout) { lv_timer_del(s_timeout); s_timeout = nullptr; }
}

static bool _send(const char* cmd)
{
    OPS_LOG("RepAdmin", "%s <- %s", s_name, cmd);
    return MeshService::instance().sendAdminCommand(s_prefix, cmd);
}

static bool _escInner();   // steps back out of a sub-view; false = none open

static void _onWinKey(lv_event_t* e)
{
    if (lv_event_get_key(e) != LV_KEY_ESC) return;
    if (!_escInner()) _close();
}

static void _onCloseClick(lv_event_t* /*e*/) { _close(); }

// Focusable widgets get the Esc handler (the default group adds them).
static void _wire(lv_obj_t* obj)
{
    lv_obj_add_event_cb(obj, _onWinKey, LV_EVENT_KEY, nullptr);
}

static lv_obj_t* _mkBtn(lv_obj_t* parent, const char* text, lv_color_t col,
                        lv_event_cb_t cb, void* ud = nullptr, lv_coord_t w = LV_SIZE_CONTENT)
{
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, 26);
    lv_obj_set_style_bg_color(btn, theme::BG, 0);
    lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, col, 0);
    lv_obj_set_style_border_color(btn, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_width(btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 8, 0);
    lv_obj_set_style_pad_ver(btn, 0, 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    _wire(btn);
    lv_obj_t* l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);
    return btn;
}

static void _btnEnable(lv_obj_t* btn, bool on)
{
    if (!btn) return;
    if (on) lv_obj_clear_state(btn, LV_STATE_DISABLED);
    else    lv_obj_add_state(btn, LV_STATE_DISABLED);
    lv_obj_set_style_opa(btn, on ? LV_OPA_COVER : LV_OPA_40, 0);
}

static lv_obj_t* _mkLabel(lv_obj_t* parent, const char* text, lv_color_t col,
                          const lv_font_t* font = &lv_font_montserrat_12)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, font, 0);
    return l;
}

static lv_obj_t* _mkRow(lv_obj_t* parent, lv_coord_t h = LV_SIZE_CONTENT)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), h);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static void _styleDropdown(lv_obj_t* dd)
{
    lv_obj_set_style_bg_color(dd, theme::BG, 0);
    lv_obj_set_style_text_color(dd, theme::TEXT, 0);
    lv_obj_set_style_border_color(dd, theme::BORDER, 0);
    lv_obj_set_style_border_color(dd, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_ver(dd, 4, 0);
    if (lv_obj_t* list = lv_dropdown_get_list(dd)) {
        lv_obj_set_style_bg_color(list, theme::BG_CARD, 0);
        lv_obj_set_style_text_color(list, theme::TEXT, 0);
        lv_obj_set_style_text_font(list, &lv_font_montserrat_12, 0);
    }
    _wire(dd);
}

static lv_obj_t* _mkTextarea(lv_obj_t* parent, const char* placeholder, int maxLen, bool password)
{
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_height(ta, 30);
    lv_obj_set_flex_grow(ta, 1);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_max_length(ta, maxLen);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_bg_color(ta, theme::BG, 0);
    lv_obj_set_style_text_color(ta, theme::TEXT, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_12, 0);
    lv_obj_set_style_border_color(ta, theme::BORDER, 0);
    lv_obj_set_style_border_color(ta, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 4, 0);
    lv_obj_set_style_pad_all(ta, 5, 0);
    _wire(ta);
    return ta;
}

// Builds the window: title bar (title, optional extra button, close X), a
// scrollable column body and a status line.
static void _openWindow(Tool tool, const uint8_t prefix4[4], const char* name,
                        const char* title, const char* extraBtn = nullptr,
                        lv_event_cb_t extraCb = nullptr)
{
    if (s_win) _close();
    s_tool = tool;
    memcpy(s_prefix, prefix4, 4);
    strncpy(s_name, name ? name : "", sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';

    lv_group_t* g = lv_group_get_default();
    s_prevFocus = g ? lv_group_get_focused(g) : nullptr;

    s_win = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_win);
    lv_obj_set_size(s_win, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(s_win, theme::BG, 0);
    lv_obj_set_style_bg_opa(s_win, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_win, LV_OBJ_FLAG_CLICKABLE);   // swallow taps meant for the panel below

    lv_obj_t* bar = lv_obj_create(s_win);
    lv_obj_set_size(bar, OPS_SCREEN_W, BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_set_style_pad_ver(bar, 2, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char tbuf[64];
    snprintf(tbuf, sizeof(tbuf), "%s  %s", title, s_name);
    lv_obj_t* t = _mkLabel(bar, tbuf, theme::ACCENT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(t, 1);

    if (extraBtn) {
        lv_obj_t* b = _mkBtn(bar, extraBtn, theme::GREEN, extraCb);
        lv_obj_set_height(b, BAR_H - 6);
    }
    lv_obj_t* x = _mkBtn(bar, LV_SYMBOL_CLOSE, theme::TEXT_MUTED, _onCloseClick);
    lv_obj_set_height(x, BAR_H - 6);
    lv_obj_set_style_pad_hor(x, 6, 0);

    s_body = lv_obj_create(s_win);
    lv_obj_set_size(s_body, OPS_SCREEN_W, OPS_SCREEN_H - BAR_H - STATUS_H);
    lv_obj_set_pos(s_body, 0, BAR_H);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_radius(s_body, 0, 0);
    lv_obj_set_style_pad_hor(s_body, 6, 0);
    lv_obj_set_style_pad_ver(s_body, 4, 0);
    lv_obj_set_style_pad_row(s_body, 5, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);

    s_status = _mkLabel(s_win, "", theme::TEXT_MUTED, &lv_font_montserrat_10);
    lv_obj_set_width(s_status, OPS_SCREEN_W - 12);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_status, 6, OPS_SCREEN_H - STATUS_H + 3);
}

static void _forgetState();

static void _close()
{
    if (!s_win) return;
    lv_obj_t* dying = s_win;
    _forgetState();
    lv_obj_del_async(dying);   // may be running inside one of its handlers
    lv_group_t* g = lv_group_get_default();
    if (g && s_prevFocus && lv_obj_is_valid(s_prevFocus)) lv_group_focus_obj(s_prevFocus);
    s_prevFocus = nullptr;
}

// ── Change Password ───────────────────────────────────────────────────

static struct {
    lv_obj_t* which[2];   // Admin / Guest
    lv_obj_t* pw;
    lv_obj_t* pw2;
    lv_obj_t* remember;
    int       sel;        // 0 admin, 1 guest
    bool      waiting;
    char      sent[16];   // the password being set (until the reply)
} s_pw;

static void _pwRefresh()
{
    for (int i = 0; i < 2; i++) {
        bool on = (s_pw.sel == i);
        lv_obj_t* b = s_pw.which[i];
        lv_obj_set_style_bg_color(b, on ? theme::ACCENT : theme::BG, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), on ? lv_color_black() : theme::TEXT_MUTED, 0);
    }
    if (s_pw.remember) {
        int idx = -1;
        bool stored = repeaters::findByKey(s_prefix, &idx) && repeaters::hasAdminPassword(idx);
        if (s_pw.sel == 0 && stored) lv_obj_clear_flag(s_pw.remember, LV_OBJ_FLAG_HIDDEN);
        else                         lv_obj_add_flag(s_pw.remember, LV_OBJ_FLAG_HIDDEN);
    }
}

static void _onPwWhich(lv_event_t* e)
{
    s_pw.sel = (int)(intptr_t)lv_event_get_user_data(e);
    _pwRefresh();
}

static void _onPwSave(lv_event_t* /*e*/)
{
    if (s_pw.waiting) return;
    const char* a = lv_textarea_get_text(s_pw.pw);
    const char* b = lv_textarea_get_text(s_pw.pw2);
    if (!a[0])           { _setStatus("Enter a new password", theme::ORANGE); return; }
    if (strcmp(a, b))    { _setStatus("The two passwords don't match", theme::ORANGE); return; }
    if (a[0] == ' ')     { _setStatus("A password can't start with a space", theme::ORANGE); return; }
    strncpy(s_pw.sent, a, sizeof(s_pw.sent) - 1);
    s_pw.sent[sizeof(s_pw.sent) - 1] = '\0';
    char cmd[48];
    if (s_pw.sel == 0) snprintf(cmd, sizeof(cmd), "password %s", s_pw.sent);
    else               snprintf(cmd, sizeof(cmd), "set guest.password %s", s_pw.sent);
    if (!_send(cmd)) { _setStatus("Send failed", theme::RED); return; }
    s_pw.waiting = true;
    _expectReply(s_pw.sel == 0 ? "Changing admin password..." : "Changing guest password...");
}

static bool _pwResponse(const char* text)
{
    if (!s_pw.waiting) return false;
    bool ok = (s_pw.sel == 0) ? (strncmp(text, "password now:", 13) == 0)
                              : (strcmp(text, "OK") == 0);
    bool err = strncmp(text, "Err", 3) == 0 || strncmp(text, "Error", 5) == 0;
    if (!ok && !err) return false;
    _gotReply();
    s_pw.waiting = false;
    if (err) {
        _setStatus(text, theme::RED);
    } else if (s_pw.sel == 0) {
        int idx = -1;
        bool updated = false;
        if (s_pw.remember && !lv_obj_has_flag(s_pw.remember, LV_OBJ_FLAG_HIDDEN) &&
            lv_obj_has_state(s_pw.remember, LV_STATE_CHECKED) &&
            repeaters::findByKey(s_prefix, &idx))
            updated = repeaters::setAdminPassword(idx, s_pw.sent);
        _setStatus(updated ? "Admin password changed - remembered password updated"
                           : "Admin password changed", theme::GREEN);
    } else {
        _setStatus("Guest password changed", theme::GREEN);
    }
    memset(s_pw.sent, 0, sizeof(s_pw.sent));
    return true;
}

void openPassword(const uint8_t prefix4[4], const char* name)
{
    _openWindow(T_PASS, prefix4, name, LV_SYMBOL_EDIT " Password");
    memset(&s_pw, 0, sizeof(s_pw));

    lv_obj_t* row = _mkRow(s_body, 28);
    lv_obj_set_width(_mkLabel(row, "Which", theme::TEXT), 64);
    s_pw.which[0] = _mkBtn(row, "Admin", theme::BORDER, _onPwWhich, (void*)(intptr_t)0, 80);
    s_pw.which[1] = _mkBtn(row, "Guest", theme::BORDER, _onPwWhich, (void*)(intptr_t)1, 80);

    // Repeater passwords are at most 15 characters (NodePrefs char[16]).
    row = _mkRow(s_body, 32);
    lv_obj_set_width(_mkLabel(row, "New", theme::TEXT), 64);
    s_pw.pw = _mkTextarea(row, "new password", 15, true);

    row = _mkRow(s_body, 32);
    lv_obj_set_width(_mkLabel(row, "Confirm", theme::TEXT), 64);
    s_pw.pw2 = _mkTextarea(row, "again", 15, true);

    s_pw.remember = lv_checkbox_create(s_body);
    lv_checkbox_set_text(s_pw.remember, "Update remembered password");
    lv_obj_set_style_text_color(s_pw.remember, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_pw.remember, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(s_pw.remember, theme::ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(s_pw.remember, theme::BORDER, LV_PART_INDICATOR);
    lv_obj_add_state(s_pw.remember, LV_STATE_CHECKED);
    _wire(s_pw.remember);

    row = _mkRow(s_body, 30);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _mkBtn(row, "Cancel", theme::TEXT_MUTED, _onCloseClick, nullptr, 96);
    _mkBtn(row, LV_SYMBOL_SAVE " Save", theme::ACCENT, _onPwSave, nullptr, 96);

    _pwRefresh();
    _setStatus("Sent to the repeater; up to 15 characters", theme::TEXT_MUTED);
    lv_group_t* g = lv_group_get_default();
    if (g) lv_group_focus_obj(s_pw.pw);
}

// ── Access Control ────────────────────────────────────────────────────

static constexpr int ACL_MAX = 32;
struct AclEntry {
    uint8_t key[6];
    uint8_t perms;
    bool    full;          // full 32-byte key known (from contacts/repeaters)
    bool    self;          // our own node
    uint8_t pub[32];
    char    name[32];
};
static AclEntry (&s_aclE)[ACL_MAX] = ops::psram::array<AclEntry[ACL_MAX]>();
static AclEntry* _aclEntries() { return s_aclE; }

static struct {
    int       count;
    bool      loading;
    bool      busy;       // a setperm is outstanding
    bool      picking;    // Add picker shown
    lv_obj_t* roleDd;     // picker: role to add with
    listfilter::State filter;
} s_ac;

static const char* kRoles = "Read-only\nRead/Write\nAdmin";

static void _aclLoad();

// Name and full key for a 6-byte prefix from Contacts, then Repeaters.
static void _aclResolve(AclEntry& e)
{
    uint8_t self[32];
    MeshService::instance().getSelfPubKey(self);
    e.self = memcmp(self, e.key, 6) == 0;
    if (e.self) {
        memcpy(e.pub, self, 32);
        e.full = true;
        snprintf(e.name, sizeof(e.name), "(this T-Deck)");
        return;
    }
    static const uint8_t zero[32] = {};
    static Contact c;   // 160 B: keep off the stack
    for (int i = 0, n = contacts::count(); i < n; i++) {
        if (!contacts::get(i, c) || memcmp(c.pubKeyPrefix, e.key, 4) != 0) continue;
        bool fk = memcmp(c.pubKey, zero, 32) != 0;
        if (fk && memcmp(c.pubKey, e.key, 6) != 0) continue;
        strncpy(e.name, c.name, sizeof(e.name) - 1);
        if (fk) { memcpy(e.pub, c.pubKey, 32); e.full = true; }
        return;
    }
    static Repeater r;
    for (int i = 0, n = repeaters::count(); i < n; i++) {
        if (!repeaters::get(i, r) || memcmp(r.pubKeyPrefix, e.key, 4) != 0) continue;
        bool fk = memcmp(r.pubKey, zero, 32) != 0;
        if (fk && memcmp(r.pubKey, e.key, 6) != 0) continue;
        strncpy(e.name, r.name, sizeof(e.name) - 1);
        if (fk) { memcpy(e.pub, r.pubKey, 32); e.full = true; }
        return;
    }
    snprintf(e.name, sizeof(e.name), "unknown");
}

static void _hex(const uint8_t* b, int n, char* out)
{
    for (int i = 0; i < n; i++) sprintf(out + i * 2, "%02X", b[i]);
    out[n * 2] = '\0';
}

static void _onAclRole(lv_event_t* e)
{
    if (s_ac.busy) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    AclEntry* a = _aclEntries();
    if (!a || i < 0 || i >= s_ac.count || !a[i].full) return;
    uint8_t role = (uint8_t)(lv_dropdown_get_selected(lv_event_get_target(e)) + 1);   // 1..3
    uint8_t perms = (uint8_t)((a[i].perms & ~3) | role);
    char hex[65], cmd[96];
    _hex(a[i].pub, 32, hex);
    snprintf(cmd, sizeof(cmd), "setperm %s %u", hex, (unsigned)perms);
    if (!_send(cmd)) { _setStatus("Send failed", theme::RED); return; }
    s_ac.busy = true;
    _expectReply("Changing role...");
}

static void _onAclRemove(lv_event_t* e)
{
    if (s_ac.busy) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    AclEntry* a = _aclEntries();
    if (!a || i < 0 || i >= s_ac.count || a[i].self) return;
    char hex[13], cmd[40];
    _hex(a[i].key, 6, hex);
    snprintf(cmd, sizeof(cmd), "setperm %s 0", hex);   // guest = remove
    if (!_send(cmd)) { _setStatus("Send failed", theme::RED); return; }
    s_ac.busy = true;
    _expectReply("Removing...");
}

static void _aclRender()
{
    if (!s_body) return;
    lv_obj_clean(s_body);
    s_ac.picking = false;
    s_ac.roleDd  = nullptr;
    s_ac.filter.ta = s_ac.filter.list = nullptr;
    AclEntry* a = _aclEntries();
    lv_obj_t* first = nullptr;

    if (!s_ac.count) {
        _mkLabel(s_body, "No clients with a role. Guests log in with the guest "
                         "password and are not listed.", theme::TEXT_MUTED);
        lv_obj_set_width(lv_obj_get_child(s_body, 0), LV_PCT(100));
        lv_label_set_long_mode(lv_obj_get_child(s_body, 0), LV_LABEL_LONG_WRAP);
    }
    for (int i = 0; i < s_ac.count && a; i++) {
        lv_obj_t* row = _mkRow(s_body, 30);
        char hex[9];
        _hex(a[i].key, 4, hex);
        lv_obj_t* h = _mkLabel(row, hex, theme::TEXT_MUTED, &lv_font_montserrat_10);
        lv_obj_set_width(h, 52);
        lv_obj_t* n = _mkLabel(row, a[i].name, a[i].self ? theme::ACCENT : theme::TEXT);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(n, 1);

        uint8_t role = a[i].perms & 3;
        if (role >= 1 && a[i].full && !a[i].self) {
            lv_obj_t* dd = lv_dropdown_create(row);
            lv_obj_set_size(dd, 104, 28);
            lv_dropdown_set_options_static(dd, kRoles);
            lv_dropdown_set_selected(dd, role - 1);
            _styleDropdown(dd);
            lv_obj_add_event_cb(dd, _onAclRole, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);
            if (!first) first = dd;
        } else {
            static const char* kNames[] = { "Guest", "Read-only", "Read/Write", "Admin" };
            lv_obj_t* r = _mkLabel(row, kNames[role], theme::TEXT);
            lv_obj_set_width(r, 104);
        }
        if (!a[i].self) {
            lv_obj_t* x = _mkBtn(row, LV_SYMBOL_TRASH, theme::RED, _onAclRemove, (void*)(intptr_t)i, 32);
            lv_obj_set_style_pad_hor(x, 0, 0);
            if (!first) first = x;
        } else {
            lv_obj_t* sp = lv_obj_create(row);   // keep the columns aligned
            lv_obj_remove_style_all(sp);
            lv_obj_set_size(sp, 32, 1);
        }
    }
    char st[64];
    snprintf(st, sizeof(st), "%d client%s. Role changes need the full key (saved contact).",
             s_ac.count, s_ac.count == 1 ? "" : "s");
    _setStatus(st, theme::TEXT_MUTED);
    lv_group_t* g = lv_group_get_default();
    if (g && first) lv_group_focus_obj(first);
}

// ── Add picker: saved contacts with a full key ───────────────────────

static void _acPickText(int key, char* out, size_t max)
{
    static Contact c;
    if (contacts::get(key, c)) snprintf(out, max, "%s %02X%02X%02X%02X", c.name,
                                        c.pubKeyPrefix[0], c.pubKeyPrefix[1],
                                        c.pubKeyPrefix[2], c.pubKeyPrefix[3]);
    else out[0] = '\0';
}

static void _onAcPick(lv_event_t* e)
{
    if (s_ac.busy) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    static Contact c;
    if (!contacts::get(idx, c)) return;
    uint8_t role = (uint8_t)(s_ac.roleDd ? lv_dropdown_get_selected(s_ac.roleDd) + 1 : 2);
    char hex[65], cmd[96];
    _hex(c.pubKey, 32, hex);
    snprintf(cmd, sizeof(cmd), "setperm %s %u", hex, (unsigned)role);
    if (!_send(cmd)) { _setStatus("Send failed", theme::RED); return; }
    s_ac.busy = true;
    char st[64];
    snprintf(st, sizeof(st), "Adding %s...", c.name);
    _expectReply(st);
}

// Rebuilding s_body from a key handler would delete the event's own target;
// rebuild on the next LVGL pass instead.
static void _aclRenderAsync(void* /*unused*/) { if (s_win && s_tool == T_ACL) _aclRender(); }
static void _acPickerEsc() { lv_async_call(_aclRenderAsync, nullptr); }

static void _onAclAdd(lv_event_t* /*e*/)
{
    if (s_ac.loading || s_ac.busy || s_ac.picking || !s_body) return;
    lv_obj_clean(s_body);
    s_ac.picking = true;

    lv_obj_t* top = _mkRow(s_body, 30);
    _mkLabel(top, "Add as", theme::TEXT);
    s_ac.roleDd = lv_dropdown_create(top);
    lv_obj_set_size(s_ac.roleDd, 104, 28);
    lv_dropdown_set_options_static(s_ac.roleDd, kRoles);
    lv_dropdown_set_selected(s_ac.roleDd, 1);   // Read/Write
    _styleDropdown(s_ac.roleDd);
    lv_obj_t* sp = lv_obj_create(top);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);

    lv_obj_t* list = lv_obj_create(s_body);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_height(list, OPS_SCREEN_H - BAR_H - STATUS_H - 50);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 3, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    static const uint8_t zero[32] = {};
    static Contact c;
    int shown = 0;
    for (int i = 0, n = contacts::count(); i < n && shown < 100; i++) {
        if (!contacts::get(i, c) || memcmp(c.pubKey, zero, 32) == 0) continue;
        char txt[48];
        snprintf(txt, sizeof(txt), "%s  %02X%02X%02X%02X", c.name, c.pubKeyPrefix[0],
                 c.pubKeyPrefix[1], c.pubKeyPrefix[2], c.pubKeyPrefix[3]);
        lv_obj_t* b = _mkBtn(list, txt, theme::TEXT, _onAcPick, (void*)(intptr_t)i, LV_PCT(100));
        lv_obj_set_style_text_align(lv_obj_get_child(b, 0), LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(lv_obj_get_child(b, 0), LV_ALIGN_LEFT_MID, 0, 0);
        listfilter::setRowKey(b, i);
        shown++;
    }
    if (!shown) _mkLabel(list, "No saved contacts with a full key yet.", theme::TEXT_MUTED);

    s_ac.filter = listfilter::State{};
    s_ac.filter.list    = list;
    s_ac.filter.rowText = _acPickText;
    s_ac.filter.onEsc   = _acPickerEsc;
    listfilter::create(top, s_ac.filter, 120, 0);
    _setStatus("Pick a contact (type to filter); Esc goes back", theme::TEXT_MUTED);
}

static void _aclLoad()
{
    s_ac.count   = 0;
    s_ac.loading = true;
    if (s_body) {
        lv_obj_clean(s_body);
        _mkLabel(s_body, "Loading...", theme::TEXT_MUTED);
    }
    if (!MeshService::instance().sendRepeaterAclReq(s_prefix)) {
        s_ac.loading = false;
        _setStatus("Send failed", theme::RED);
        return;
    }
    _expectReply("Asking the repeater for its access list...");
}

static bool _aclResponse(const char* text)
{
    if (strncmp(text, "ACL ", 4) == 0 && s_ac.loading) {
        AclEntry* a = _aclEntries();
        if (strncmp(text + 4, "end", 3) == 0) {
            _gotReply();
            s_ac.loading = false;
            _aclRender();
            return true;
        }
        unsigned b[6], perms = 0;
        if (a && s_ac.count < ACL_MAX &&
            sscanf(text + 4, "%2X%2X%2X%2X%2X%2X %u", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &perms) == 7) {
            AclEntry& e = a[s_ac.count++];
            memset(&e, 0, sizeof(e));
            for (int k = 0; k < 6; k++) e.key[k] = (uint8_t)b[k];
            e.perms = (uint8_t)perms;
            _aclResolve(e);
        }
        return true;
    }
    if (s_ac.busy && (strcmp(text, "OK") == 0 || strncmp(text, "Err", 3) == 0)) {
        _gotReply();
        s_ac.busy = false;
        if (text[0] == 'O') _aclLoad();          // show the new list
        else                _setStatus(text, theme::RED);
        return true;
    }
    return false;
}

void openAccess(const uint8_t prefix4[4], const char* name)
{
    _openWindow(T_ACL, prefix4, name, LV_SYMBOL_EYE_OPEN " Access", LV_SYMBOL_PLUS " Add", _onAclAdd);
    s_ac = {};
        _aclLoad();
}

// ── Regions ───────────────────────────────────────────────────────────

static constexpr int REG_MAX = 32;
struct RegEntry {
    char name[32];
    uint8_t depth;   // 1 = top level (child of *)
    bool flood;
    bool home;
};

static RegEntry (&s_rgE)[REG_MAX] = ops::psram::array<RegEntry[REG_MAX]>();

static struct {
    RegEntry*  e;             // → s_rgE
    int        count;
    bool       wildFlood;
    bool       listing;       // collecting list lines
    bool       truncated;
    int        listChars;
    char       def[32];       // default scope ("" = unknown)
    int        sel;           // selected row, -1 = none
    bool       busy;          // an action is outstanding
    bool       wantDefault;   // ask "region default" once the list is in
    lv_timer_t* settle;       // fires when the list lines stop arriving
    lv_obj_t*  list;
    lv_obj_t*  bFlood; lv_obj_t* bDef; lv_obj_t* bDel;
    lv_obj_t*  addOverlay;
    lv_obj_t*  addName; lv_obj_t* addParent;
} s_rg;

static void _regRender();

static void _regRequestList()
{
    s_rg.listing   = false;
    s_rg.truncated = false;
    s_rg.listChars = 0;
    if (!_send("region")) { _setStatus("Send failed", theme::RED); return; }
    _expectReply("Reading regions...");
}

static void _onRegSettle(lv_timer_t* /*t*/)
{
    s_rg.settle  = nullptr;
    s_rg.listing = false;
    s_rg.truncated = s_rg.listChars >= 150;   // CLI replies stop at ~160 chars
    _regRender();
    if (s_rg.wantDefault) {
        s_rg.wantDefault = false;
        if (_send("region default")) _expectReply("Reading the default scope...");
    }
}

static void _regSettleSoon()
{
    if (s_rg.settle) lv_timer_reset(s_rg.settle);
    else {
        s_rg.settle = lv_timer_create(_onRegSettle, 600, nullptr);
        lv_timer_set_repeat_count(s_rg.settle, 1);
    }
}

static void _regSelRefresh()
{
    bool any = s_rg.sel >= 0 && s_rg.sel < s_rg.count;
    _btnEnable(s_rg.bFlood, any && !s_rg.busy);
    _btnEnable(s_rg.bDef,   any && !s_rg.busy);
    _btnEnable(s_rg.bDel,   any && !s_rg.busy);
    if (!s_rg.list) return;
    for (int i = 0; i < (int)lv_obj_get_child_cnt(s_rg.list); i++) {
        lv_obj_t* row = lv_obj_get_child(s_rg.list, i);
        int key = (int)(intptr_t)lv_obj_get_user_data(row);
        lv_obj_set_style_bg_opa(row, key == s_rg.sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    if (any && s_rg.bFlood) {
        lv_label_set_text(lv_obj_get_child(s_rg.bFlood, 0),
                          s_rg.e[s_rg.sel].flood ? "Flood off" : "Flood on");
    }
}

static void _onRegRow(lv_event_t* e)
{
    s_rg.sel = (int)(intptr_t)lv_event_get_user_data(e);
    _regSelRefresh();
}

static void _regAction(const char* verb)
{
    if (s_rg.busy || s_rg.sel < 0 || s_rg.sel >= s_rg.count) return;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "region %s %s", verb, s_rg.e[s_rg.sel].name);
    if (!_send(cmd)) { _setStatus("Send failed", theme::RED); return; }
    s_rg.busy = true;
    _regSelRefresh();
    _expectReply("Sending...");
}

static void _onRegFlood(lv_event_t* /*e*/)
{
    if (s_rg.sel >= 0 && s_rg.sel < s_rg.count)
        _regAction(s_rg.e[s_rg.sel].flood ? "denyf" : "allowf");
}
static void _onRegDefault(lv_event_t* /*e*/) { _regAction("default"); }
static void _onRegDelete(lv_event_t* /*e*/)  { _regAction("remove"); }

static void _onRegSave(lv_event_t* /*e*/)
{
    if (s_rg.busy) return;
    if (!_send("region save")) { _setStatus("Send failed", theme::RED); return; }
    s_rg.busy = true;
    _regSelRefresh();
    _expectReply("Saving on the repeater...");
}

// Add dialog (over the window)
static void _regAddClose()
{
    if (!s_rg.addOverlay) return;
    lv_obj_t* o = s_rg.addOverlay;
    s_rg.addOverlay = s_rg.addName = s_rg.addParent = nullptr;
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_del_async(o);
    _regSelRefresh();
}
static void _onRegAddCancel(lv_event_t* /*e*/) { _regAddClose(); }

static void _onRegAddOk(lv_event_t* /*e*/)
{
    if (!s_rg.addName || s_rg.busy) return;
    char name[32];
    strncpy(name, lv_textarea_get_text(s_rg.addName), sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    if (!name[0]) { _setStatus("Enter a region name", theme::ORANGE); return; }
    char cmd[80];
    int p = s_rg.addParent ? (int)lv_dropdown_get_selected(s_rg.addParent) : 0;
    if (p > 0 && p <= s_rg.count)
        snprintf(cmd, sizeof(cmd), "region put %s %s", name, s_rg.e[p - 1].name);
    else
        snprintf(cmd, sizeof(cmd), "region put %s", name);
    _regAddClose();
    if (!_send(cmd)) { _setStatus("Send failed", theme::RED); return; }
    s_rg.busy = true;
    _regSelRefresh();
    _expectReply("Adding region...");
}

static void _onRegAdd(lv_event_t* /*e*/)
{
    if (!s_win || s_rg.addOverlay || s_rg.busy || s_rg.listing) return;
    lv_obj_t* o = lv_obj_create(s_win);
    s_rg.addOverlay = o;
    lv_obj_set_size(o, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(o);
    lv_obj_set_size(box, 280, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::BORDER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

    _mkLabel(box, LV_SYMBOL_PLUS " New region", theme::ACCENT);
    lv_obj_t* row = _mkRow(box, 32);
    lv_obj_set_width(_mkLabel(row, "Name", theme::TEXT), 56);
    s_rg.addName = _mkTextarea(row, "e.g. uk-north", 30, false);

    row = _mkRow(box, 32);
    lv_obj_set_width(_mkLabel(row, "Parent", theme::TEXT), 56);
    s_rg.addParent = lv_dropdown_create(row);
    lv_obj_set_height(s_rg.addParent, 30);
    lv_obj_set_flex_grow(s_rg.addParent, 1);
    {
        // Options: "* (top level)" then each region name.
        static char (&opts)[REG_MAX * 33 + 16] = ops::psram::array<char[REG_MAX * 33 + 16]>();
        size_t n = (size_t)snprintf(opts, sizeof(opts), "* (top level)");
        for (int i = 0; i < s_rg.count && n < sizeof(opts) - 34; i++)
            n += (size_t)snprintf(opts + n, sizeof(opts) - n, "\n%s", s_rg.e[i].name);
        lv_dropdown_set_options(s_rg.addParent, opts);
        int p = (s_rg.sel >= 0 && s_rg.sel < s_rg.count) ? s_rg.sel + 1 : 0;   // under the selected one
        lv_dropdown_set_selected(s_rg.addParent, (uint16_t)p);
    }
    _styleDropdown(s_rg.addParent);

    lv_obj_t* note = _mkLabel(box, "New regions allow flooding. Press Save to keep them after a reboot.",
                              theme::TEXT_MUTED, &lv_font_montserrat_10);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    row = _mkRow(box, 30);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _mkBtn(row, "Cancel", theme::TEXT_MUTED, _onRegAddCancel, nullptr, 90);
    _mkBtn(row, LV_SYMBOL_PLUS " Add", theme::ACCENT, _onRegAddOk, nullptr, 90);

    lv_group_t* g = lv_group_get_default();
    if (g) lv_group_focus_obj(s_rg.addName);
}

static void _regRender()
{
    if (!s_body) return;
    lv_obj_clean(s_body);
    s_rg.bFlood = s_rg.bDef = s_rg.bDel = nullptr;
    if (s_rg.sel >= s_rg.count) s_rg.sel = -1;

    s_rg.list = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_rg.list);
    lv_obj_set_width(s_rg.list, LV_PCT(100));
    lv_obj_set_height(s_rg.list, OPS_SCREEN_H - BAR_H - STATUS_H - 8 - 2 * 31);
    lv_obj_set_flex_flow(s_rg.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_rg.list, 2, 0);
    lv_obj_set_scroll_dir(s_rg.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_rg.list, LV_SCROLLBAR_MODE_AUTO);

    // "*" (every region) — shown for its flood state; actions need a name.
    {
        lv_obj_t* row = _mkRow(s_rg.list, 24);
        lv_obj_set_user_data(row, (void*)(intptr_t)-2);
        lv_obj_t* n = _mkLabel(row, "*  (all / unscoped)", theme::TEXT_MUTED);
        lv_obj_set_flex_grow(n, 1);
        _mkLabel(row, s_rg.wildFlood ? "Flood " LV_SYMBOL_OK : "Flood " LV_SYMBOL_CLOSE,
                 s_rg.wildFlood ? theme::GREEN : theme::RED);
    }
    lv_obj_t* first = nullptr;
    for (int i = 0; i < s_rg.count; i++) {
        const RegEntry& r = s_rg.e[i];
        lv_obj_t* row = lv_btn_create(s_rg.list);
        lv_obj_set_size(row, LV_PCT(100), 26);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_set_style_bg_color(row, theme::PRIMARY, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_color(row, theme::ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(row, 1, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_radius(row, 3, 0);
        lv_obj_set_style_pad_hor(row, 4, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, _onRegRow, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, _onRegRow, LV_EVENT_FOCUSED, (void*)(intptr_t)i);
        _wire(row);
        if (!first) first = row;

        char txt[64];
        bool isDef = s_rg.def[0] && strcmp(s_rg.def, r.name) == 0;
        int ind = r.depth > 1 ? (r.depth - 1) * 2 : 0;
        snprintf(txt, sizeof(txt), "%s%*s%s%s%s", isDef ? LV_SYMBOL_OK " " : "", ind, "",
                 r.depth > 1 ? "> " : "", r.name, r.home ? "  (home)" : "");
        lv_obj_t* n = _mkLabel(row, txt, isDef ? theme::ACCENT : theme::TEXT);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(n, 1);
        _mkLabel(row, r.flood ? "Flood " LV_SYMBOL_OK : "Flood " LV_SYMBOL_CLOSE,
                 r.flood ? theme::GREEN : theme::RED);
    }
    if (!s_rg.count) {
        lv_obj_t* l = _mkLabel(s_rg.list, "No regions yet - use + Add.", theme::TEXT_MUTED);
        lv_obj_set_user_data(l, (void*)(intptr_t)-2);
    }

    lv_obj_t* row = _mkRow(s_body, 26);
    s_rg.bFlood = _mkBtn(row, "Flood on", theme::ACCENT, _onRegFlood, nullptr, 96);
    s_rg.bDef   = _mkBtn(row, LV_SYMBOL_OK " Default", theme::ACCENT, _onRegDefault, nullptr, 96);
    s_rg.bDel   = _mkBtn(row, LV_SYMBOL_TRASH " Delete", theme::RED, _onRegDelete, nullptr, 96);
    row = _mkRow(s_body, 26);
    _mkBtn(row, LV_SYMBOL_SAVE " Save to repeater", theme::GREEN, _onRegSave, nullptr, LV_PCT(100));

    _regSelRefresh();
    if (s_rg.truncated)
        _setStatus("List may be cut off (repeater replies stop at 160 chars)", theme::ORANGE);
    else
        _setStatus(LV_SYMBOL_OK " = default scope. Changes are live; Save keeps them.", theme::TEXT_MUTED);
    lv_group_t* g = lv_group_get_default();
    if (g && first && s_rg.sel < 0) lv_group_focus_obj(first);
}

static bool _regResponse(const char* text)
{
    // List start: "*" or "* F" (the wildcard), then indented region lines.
    if (text[0] == '*' && (text[1] == '\0' || text[1] == ' ' || text[1] == '^')) {
        _gotReply();
        s_rg.listing   = true;
        s_rg.count     = 0;
        s_rg.wildFlood = strstr(text, " F") != nullptr;
        s_rg.listChars = (int)strlen(text) + 1;
        _regSettleSoon();
        return true;
    }
    const char* d = strstr(text, "default scope is ");
    if (d) {
        _gotReply();
        d += 17;
        if (strncmp(d, "now ", 4) == 0) d += 4;
        strncpy(s_rg.def, strcmp(d, "<null>") == 0 ? "" : d, sizeof(s_rg.def) - 1);
        s_rg.def[sizeof(s_rg.def) - 1] = '\0';
        bool wasAction = s_rg.busy;
        s_rg.busy = false;
        if (wasAction) {
            _setStatus("Default scope set", theme::GREEN);
            _regRequestList();
        } else {
            _regRender();
        }
        return true;
    }
    if (s_rg.listing && text[0] == ' ') {
        int depth = 0;
        while (text[depth] == ' ') depth++;
        RegEntry r = {};
        r.depth = (uint8_t)depth;
        const char* p = text + depth;
        int n = 0;
        while (*p && *p != ' ' && *p != '^' && n < (int)sizeof(r.name) - 1) r.name[n++] = *p++;
        r.home  = (*p == '^');
        r.flood = strstr(p, " F") != nullptr;
        if (n && s_rg.count < REG_MAX) s_rg.e[s_rg.count++] = r;
        s_rg.listChars += (int)strlen(text) + 1;
        _regSettleSoon();
        return true;
    }
    if (s_rg.busy && (strncmp(text, "OK", 2) == 0 || strncmp(text, "Err", 3) == 0)) {
        _gotReply();
        s_rg.busy = false;
        if (text[0] == 'E') {
            _setStatus(text, theme::RED);
            _regSelRefresh();
        } else if (strcmp(text, "OK") == 0 && !s_rg.listing && lv_obj_is_valid(s_status) &&
                   strstr(lv_label_get_text(s_status), "Saving")) {
            _setStatus("Saved on the repeater", theme::GREEN);
            _regSelRefresh();
        } else {
            _regRequestList();   // show the change
        }
        return true;
    }
    return false;
}

void openRegions(const uint8_t prefix4[4], const char* name)
{
    _openWindow(T_REG, prefix4, name, LV_SYMBOL_GPS " Regions", LV_SYMBOL_PLUS " Add", _onRegAdd);
    memset(&s_rg, 0, sizeof(s_rg));
    s_rg.e   = s_rgE;
    s_rg.sel = -1;
    s_rg.wantDefault = true;
    _mkLabel(s_body, "Loading...", theme::TEXT_MUTED);
    _regRequestList();
}

// ── Routing / teardown ────────────────────────────────────────────────

static bool _escInner()
{
    if (s_tool == T_REG && s_rg.addOverlay) { _regAddClose(); return true; }
    if (s_tool == T_ACL && s_ac.picking)    { s_ac.picking = false; _acPickerEsc(); return true; }
    return false;
}


bool onResponse(const char* text)
{
    if (!s_win || !text) return false;
    switch (s_tool) {
        case T_PASS: return _pwResponse(text);
        case T_ACL:  return _aclResponse(text);
        case T_REG:  return _regResponse(text);
        default:     return false;
    }
}

static void _forgetState()
{
    if (s_timeout) { lv_timer_del(s_timeout); s_timeout = nullptr; }
    if (s_rg.settle) { lv_timer_del(s_rg.settle); s_rg.settle = nullptr; }
    memset(s_pw.sent, 0, sizeof(s_pw.sent));
    s_tool = T_NONE;
    s_win = s_body = s_status = nullptr;
    s_rg.list = s_rg.addOverlay = nullptr;
    s_ac.filter.ta = s_ac.filter.list = nullptr;
}

void forget()
{
    _forgetState();
    s_prevFocus = nullptr;
}

}}}  // namespace ops::ui::repadmin
