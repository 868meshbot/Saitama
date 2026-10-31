// Saitama — cardputer/KeyNav.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "KeyNav.h"
#include "../Theme.h"
#include "../../features.h"
#include <lvgl.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

namespace ops { namespace ui { namespace keynav {

static constexpr int MAX_CAND = 160;   // a full contact list fits

static lv_obj_t* s_hl = nullptr;   // highlighted object (validated before use)

// ── Helpers ─────────────────────────────────────────────────────────

static bool _hiddenUpTree(lv_obj_t* o)
{
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return true;
    return false;
}

static bool _isFullSize(lv_obj_t* o)
{
    return lv_obj_get_width(o)  >= OPS_SCREEN_W - 2 &&
           lv_obj_get_height(o) >= OPS_SCREEN_H - 2;
}

static bool _isDescendant(lv_obj_t* o, lv_obj_t* root)
{
    for (; o; o = lv_obj_get_parent(o))
        if (o == root) return true;
    return false;
}

// The layer input goes to: the newest visible full-screen child of the top
// layer or of the active screen (child 0 is the screen's own body), else the
// active screen.
static lv_obj_t* _scope()
{
    lv_obj_t* top = lv_layer_top();
    for (int i = (int)lv_obj_get_child_cnt(top) - 1; i >= 0; i--) {
        lv_obj_t* c = lv_obj_get_child(top, i);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) && _isFullSize(c)) return c;
    }
    lv_obj_t* scr = lv_scr_act();
    for (int i = (int)lv_obj_get_child_cnt(scr) - 1; i >= 1; i--) {
        lv_obj_t* c = lv_obj_get_child(scr, i);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) && _isFullSize(c)) return c;
    }
    return scr;
}

static bool _hasHandlers(lv_obj_t* o)
{
    return o->spec_attr && o->spec_attr->event_dsc_cnt > 0;
}

// On screen, or scrolled out of a scrolling container that is on screen
// (the highlight scrolls it into view).
static bool _reachable(lv_obj_t* o)
{
    if (lv_obj_is_visible(o)) return true;
    for (lv_obj_t* p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE) &&
            lv_obj_get_scroll_dir(p) != LV_DIR_NONE)
            return lv_obj_is_visible(p);
    }
    return false;
}

// Covered by a later (higher) child of the scope, e.g. chat bubbles under
// the emoji picker.
static bool _occluded(lv_obj_t* o, lv_obj_t* scope)
{
    lv_obj_t* top = o;
    while (top && lv_obj_get_parent(top) != scope) top = lv_obj_get_parent(top);
    if (!top) return false;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_point_t c = { (lv_coord_t)((a.x1 + a.x2) / 2), (lv_coord_t)((a.y1 + a.y2) / 2) };
    uint32_t cnt = lv_obj_get_child_cnt(scope);
    for (uint32_t i = lv_obj_get_index(top) + 1; i < cnt; i++) {
        lv_obj_t* above = lv_obj_get_child(scope, i);
        if (lv_obj_has_flag(above, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t b;
        lv_obj_get_coords(above, &b);
        if (_lv_area_is_point_on(&b, &c, 0)) return true;
    }
    return false;
}

static bool _isCandidate(lv_obj_t* o, lv_obj_t* scope)
{
    if (o == scope) return false;
    if (!lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) return false;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_has_state(o, LV_STATE_DISABLED)) return false;
    if (lv_obj_has_class(o, &lv_textarea_class)) return false;
    if (_isFullSize(o)) return false;              // a backdrop, not a button
    // A drop-down needs no callback of its own to be useful (the dialog
    // reads its value on Save): Enter opens it as a button list.
    if (!_hasHandlers(o) && !lv_obj_has_class(o, &lv_dropdown_class)) return false;
    return _reachable(o);
}

// A text box Tab can move into.
static bool _isTextBox(lv_obj_t* o)
{
    return lv_obj_has_class(o, &lv_textarea_class) &&
           !lv_obj_has_state(o, LV_STATE_DISABLED) && _reachable(o);
}

static void _collect(lv_obj_t* parent, lv_obj_t* scope, lv_obj_t** out, int& n,
                     bool textBoxes = false)
{
    uint32_t cnt = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < cnt && n < MAX_CAND; i++) {
        lv_obj_t* c = lv_obj_get_child(parent, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if ((_isCandidate(c, scope) || (textBoxes && _isTextBox(c))) && !_occluded(c, scope))
            out[n++] = c;
        _collect(c, scope, out, n, textBoxes);
    }
}

// Reading order: top to bottom, then left to right.
static int _cmp(lv_obj_t* a, lv_obj_t* b)
{
    lv_area_t ca, cb;
    lv_obj_get_coords(a, &ca);
    lv_obj_get_coords(b, &cb);
    if (ca.y1 != cb.y1) return ca.y1 < cb.y1 ? -1 : 1;
    return ca.x1 < cb.x1 ? -1 : (ca.x1 > cb.x1 ? 1 : 0);
}

static void _sort(lv_obj_t** v, int n)
{
    for (int i = 1; i < n; i++) {
        lv_obj_t* k = v[i];
        int j = i - 1;
        while (j >= 0 && _cmp(v[j], k) > 0) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
}

static bool _hlValid(lv_obj_t* scope)
{
    return s_hl && lv_obj_is_valid(s_hl) && _isDescendant(s_hl, scope) &&
           !_hiddenUpTree(s_hl);
}

static void _setHl(lv_obj_t* o)
{
    if (s_hl && lv_obj_is_valid(s_hl))
        lv_obj_clear_state(s_hl, LV_STATE_FOCUS_KEY);
    s_hl = o;
    if (!o) return;
    // Outline in the accent colour, only while highlighted.
    lv_obj_set_style_outline_color(o, theme::ACCENT, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(o, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(o, -2, LV_STATE_FOCUS_KEY);
    lv_obj_add_state(o, LV_STATE_FOCUS_KEY);
    lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
    // Near the top of a scrolling box: show the box from its top, so a
    // header above the first button (popup title, name) is not left cut off.
    for (lv_obj_t* p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p)) {
        if (lv_obj_get_scroll_y(p) <= 0) continue;
        lv_area_t a, pa;
        lv_obj_get_coords(o, &a);
        lv_obj_get_coords(p, &pa);
        lv_coord_t contentTop = pa.y1 - lv_obj_get_scroll_y(p);
        if (a.y2 - contentTop <= lv_obj_get_height(p) - 4)
            lv_obj_scroll_to_y(p, 0, LV_ANIM_OFF);
    }
}

static bool _focusOwnsInput(lv_obj_t* scope)
{
    lv_group_t* g = lv_group_get_default();
    lv_obj_t* f = g ? lv_group_get_focused(g) : nullptr;
    if (!f || !lv_obj_is_valid(f) || _hiddenUpTree(f)) return false;
    if (!_isDescendant(f, scope)) return false;
    if (lv_obj_has_class(f, &lv_textarea_class)) return false;
    // A plain container focused only so the trackball scrolls it (chat
    // list, signal page): the keys step through its rows instead. A text
    // pane flagged USER_1 keeps scrolling.
    if (lv_obj_get_class(f) == &lv_obj_class && !lv_obj_has_flag(f, LV_OBJ_FLAG_USER_1))
        return false;
    return true;
}

// ── API ─────────────────────────────────────────────────────────────

// Moves the highlight `step` places through the clickable objects and text
// boxes of the top layer, in reading order. A text box gets the group focus
// (typing goes there) instead of the highlight.
// The nearest ancestor that scrolls vertically and has content to scroll.
static lv_obj_t* _scrollParent(lv_obj_t* o)
{
    for (lv_obj_t* p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p)) {
        if (!lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) continue;
        if (!(lv_obj_get_scroll_dir(p) & LV_DIR_VER)) continue;
        if (lv_obj_get_scroll_top(p) > 0 || lv_obj_get_scroll_bottom(p) > 0) return p;
    }
    return nullptr;
}

// The nearest candidate in the arrow's direction: down/up = the next row
// (closest horizontally within it), left/right = the same row. Null when
// there is none that way.
static lv_obj_t* _spatial(lv_obj_t** cand, int n, lv_obj_t* cur, int dx, int dy)
{
    lv_area_t ca;
    lv_obj_get_coords(cur, &ca);
    int cx = (ca.x1 + ca.x2) / 2, cy = (ca.y1 + ca.y2) / 2;
    int half = (ca.y2 - ca.y1) / 2;
    lv_obj_t* best = nullptr;
    long bestScore = 0;
    for (int i = 0; i < n; i++) {
        lv_obj_t* o = cand[i];
        if (o == cur) continue;
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        int ox = (a.x1 + a.x2) / 2, oy = (a.y1 + a.y2) / 2;
        long score;
        if (dy) {
            int v = (oy - cy) * (dy > 0 ? 1 : -1);
            if (v <= 2) continue;
            score = (long)v * 8 + labs((long)(ox - cx));
        } else {
            if (abs(oy - cy) > half) continue;   // not the same row
            int h = (ox - cx) * (dx > 0 ? 1 : -1);
            if (h <= 0) continue;
            score = h;
        }
        if (!best || score < bestScore) { best = o; bestScore = score; }
    }
    return best;
}

static bool _step(int step, bool wrap, int dx = 0, int dy = 0)
{
    lv_obj_t* scope = _scope();
    static lv_obj_t* cand[MAX_CAND];
    int n = 0;
    _collect(scope, scope, cand, n, true);
    if (n == 0) { _setHl(nullptr); return false; }
    _sort(cand, n);

    // Current position: the highlight, else the focused text box.
    lv_obj_t* cur = _hlValid(scope) ? s_hl : nullptr;
    if (!cur) {
        lv_group_t* g = lv_group_get_default();
        lv_obj_t* f = g ? lv_group_get_focused(g) : nullptr;
        if (f && lv_obj_is_valid(f) && _isDescendant(f, scope)) cur = f;
    }
    int idx = -1;
    for (int i = 0; i < n; i++) if (cand[i] == cur) { idx = i; break; }

    lv_obj_t* o = nullptr;
    if (idx >= 0 && (dx || dy)) {
        o = _spatial(cand, n, cur, dx, dy);
        if (dy) {
            // The next item is off screen (text between, or nothing more
            // that way): scroll a step first, so the content in between can
            // be read; the highlight follows once the item is in view.
            lv_obj_t* sc = _scrollParent(o ? o : cur);
            if (sc && (!o || !lv_obj_is_visible(o))) {
                lv_coord_t before = lv_obj_get_scroll_y(sc);
                lv_obj_scroll_by_bounded(sc, 0, dy > 0 ? -40 : 40, LV_ANIM_OFF);
                if (o && lv_obj_is_visible(o)) { _setHl(o); return true; }
                if (lv_obj_get_scroll_y(sc) != before || !o) return true;
            } else if (!o) {
                return true;   // nothing further that way: stay
            }
        }
    }
    if (!o) {
        int next;
        if (idx < 0)   next = step > 0 ? 0 : n - 1;
        else if (wrap) next = ((idx + step) % n + n) % n;
        else           next = idx + step < 0 ? 0 : (idx + step >= n ? n - 1 : idx + step);
        // Tab leaves a grid (emoji picker, region chips) in one go.
        lv_obj_t* grid = idx >= 0 ? lv_obj_get_parent(cur) : nullptr;
        if (wrap && grid &&
            lv_obj_get_style_flex_flow(grid, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP) {
            for (int k = 0; k < n && lv_obj_get_parent(cand[next]) == grid; k++)
                next = ((next + step) % n + n) % n;
        }
        o = cand[next];
    }

    if (lv_obj_has_class(o, &lv_textarea_class)) {
        _setHl(nullptr);
        lv_group_t* g = (lv_group_t*)lv_obj_get_group(o);
        if (!g) {
            g = lv_group_get_default();
            if (g) lv_group_add_obj(g, o);
        }
        if (g) lv_group_focus_obj(o);
        lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
    } else {
        _setHl(o);
    }
    return true;
}

bool handleMove(int dx, int dy)
{
    int step = dy ? (dy > 0 ? 1 : -1) : (dx > 0 ? 1 : dx < 0 ? -1 : 0);
    if (!step) return false;
    lv_obj_t* scope = _scope();
    if (_focusOwnsInput(scope)) { _setHl(nullptr); return false; }
    // A focused plain container with nothing to pick in it (signal page):
    // up/down scroll it.
    lv_group_t* g = lv_group_get_default();
    lv_obj_t* f = g ? lv_group_get_focused(g) : nullptr;
    if (dy && f && lv_obj_is_valid(f) && lv_obj_get_class(f) == &lv_obj_class &&
        _isDescendant(f, scope)) {
        static lv_obj_t* inner[MAX_CAND];
        int m = 0;
        _collect(f, scope, inner, m);
        if (m == 0) {
            _setHl(nullptr);
            lv_obj_scroll_by_bounded(f, 0, dy > 0 ? -30 : 30, LV_ANIM_ON);
            return true;
        }
    }
    return _step(step, false, dy ? 0 : dx, dy);
}

bool handleTab()
{
    return _step(1, true);
}

// ── Drop-down picker ────────────────────────────────────────────────
// LVGL's own drop-down list needs a pointer release to open and the encoder
// to move in it. Here Enter on a drop-down opens a full-screen column of
// buttons, one per option; Enter picks, Esc (Cancel) leaves it unchanged.

static lv_obj_t* s_ddTarget  = nullptr;
static lv_obj_t* s_ddOverlay = nullptr;

static void _closeDdPicker()
{
    if (s_ddOverlay && lv_obj_is_valid(s_ddOverlay)) lv_obj_del_async(s_ddOverlay);
    s_ddOverlay = nullptr;
    s_ddTarget  = nullptr;
}

static void _onDdOption(lv_event_t* e)
{
    uint16_t idx = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t* dd = s_ddTarget;
    _closeDdPicker();
    if (dd && lv_obj_is_valid(dd) && idx != lv_dropdown_get_selected(dd)) {
        lv_dropdown_set_selected(dd, idx);
        lv_event_send(dd, LV_EVENT_VALUE_CHANGED, nullptr);
    }
}

static void _onDdCancel(lv_event_t* /*e*/)
{
    _closeDdPicker();
}

static lv_obj_t* _ddButton(lv_obj_t* parent, const char* text, bool current)
{
    lv_obj_t* b = lv_btn_create(parent);
    lv_group_remove_obj(b);
    lv_obj_set_size(b, LV_PCT(100), 24);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, current ? theme::PRIMARY : theme::BG, 0);
    lv_obj_set_style_border_color(b, current ? theme::ACCENT : theme::BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_color(l, theme::TEXT, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);
    return b;
}

bool openDropdown(lv_obj_t* dd)
{
    if (!dd || !lv_obj_is_valid(dd) || !lv_obj_has_class(dd, &lv_dropdown_class))
        return false;
    if (lv_obj_has_state(dd, LV_STATE_DISABLED)) return true;
    if (lv_dropdown_is_open(dd)) lv_dropdown_close(dd);
    _closeDdPicker();
    s_ddTarget = dd;

    s_ddOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_ddOverlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_ddOverlay, 0, 0);
    lv_obj_set_style_bg_color(s_ddOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ddOverlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_ddOverlay, 0, 0);
    lv_obj_set_style_radius(s_ddOverlay, 0, 0);
    lv_obj_set_style_pad_all(s_ddOverlay, 0, 0);
    lv_obj_clear_flag(s_ddOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(s_ddOverlay);
    lv_obj_set_size(panel, OPS_SCREEN_W - 6, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, OPS_SCREEN_H - 4, 0);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::ACCENT, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);
    lv_obj_set_style_pad_row(panel, 3, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);

    // Options are '\n'-separated.
    const char* opts = lv_dropdown_get_options(dd);
    uint16_t sel = lv_dropdown_get_selected(dd);
    lv_obj_t* selBtn = nullptr;
    char text[48];
    uint16_t idx = 0;
    for (const char* p = opts; p && *p; idx++) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(text)) len = sizeof(text) - 1;
        memcpy(text, p, len);
        text[len] = '\0';
        lv_obj_t* b = _ddButton(panel, text, idx == sel);
        lv_obj_add_event_cb(b, _onDdOption, LV_EVENT_CLICKED, (void*)(uintptr_t)idx);
        if (idx == sel) selBtn = b;
        p = nl ? nl + 1 : p + strlen(p);
    }
    lv_obj_t* cancel = _ddButton(panel, "Cancel", false);
    lv_obj_add_event_cb(cancel, _onDdCancel, LV_EVENT_CLICKED, nullptr);

    lv_obj_update_layout(s_ddOverlay);
    _setHl(selBtn ? selBtn : cancel);
    return true;
}

bool handleEnter()
{
    lv_obj_t* scope = _scope();
    if (!_hlValid(scope)) { s_hl = nullptr; return false; }
    lv_obj_t* o = s_hl;
    _setHl(nullptr);
    if (openDropdown(o)) return true;
    lv_event_send(o, LV_EVENT_CLICKED, nullptr);
    return true;
}

// A button in the overlay whose label reads like "close".
static lv_obj_t* _findCloseBtn(lv_obj_t* parent)
{
    uint32_t cnt = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* c = lv_obj_get_child(parent, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_has_class(c, &lv_label_class)) {
            const char* t = lv_label_get_text(c);
            char low[16] = {};
            for (int k = 0; t && t[k] && k < 15; k++) low[k] = (char)tolower((unsigned char)t[k]);
            if (strstr(low, "close") || strstr(low, "cancel") || strstr(low, "exit") ||
                strstr(low, "back")  || !strcmp(low, LV_SYMBOL_CLOSE)) {
                lv_obj_t* p = lv_obj_get_parent(c);
                if (p && lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE) && _hasHandlers(p)) return p;
            }
        }
        lv_obj_t* r = _findCloseBtn(c);
        if (r) return r;
    }
    return nullptr;
}

bool handleEsc(bool backspace)
{
    lv_obj_t* scope = _scope();
    if (scope == lv_scr_act()) { _setHl(nullptr); return false; }
    if (backspace) {
        lv_group_t* g = lv_group_get_default();
        lv_obj_t* f = g ? lv_group_get_focused(g) : nullptr;
        if (f && lv_obj_is_valid(f) && _isDescendant(f, scope)) return false;
    }
    _setHl(nullptr);
    lv_obj_t* btn = _findCloseBtn(scope);
    if (btn) {
        lv_event_send(btn, LV_EVENT_CLICKED, nullptr);
        return true;
    }
    uint32_t k = LV_KEY_ESC;
    lv_event_send(scope, LV_EVENT_KEY, &k);
    return true;
}

// Fits a box built for the 320 px T-Deck into this screen: a child wider
// than its parent's content area is cut to it, and a flex row whose
// children overflow shares the width out equally. Widgets with their own
// inner layout (text box, drop-down, slider) are not entered.
static void _fitWidths(lv_obj_t* o, int depth)
{
    uint32_t cnt = lv_obj_get_child_cnt(o);
    if (!cnt || depth > 8) return;
    if (lv_obj_has_class(o, &lv_textarea_class) || lv_obj_has_class(o, &lv_dropdown_class) ||
        lv_obj_has_class(o, &lv_slider_class)   || lv_obj_has_class(o, &lv_label_class))
        return;
    lv_coord_t avail = lv_obj_get_content_width(o);
    lv_coord_t sum = 0;
    int vis = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* c = lv_obj_get_child(o, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_coord_t w = lv_obj_get_width(c);
        if (w > avail) { lv_obj_set_width(c, avail); w = avail; }
        sum += w;
        vis++;
    }
    if (vis > 1 && lv_obj_get_style_flex_flow(o, LV_PART_MAIN) == LV_FLEX_FLOW_ROW &&
        sum + lv_obj_get_style_pad_column(o, LV_PART_MAIN) * (vis - 1) > avail) {
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_t* c = lv_obj_get_child(o, i);
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
            lv_obj_set_width(c, 1);
            lv_obj_set_flex_grow(c, 1);
        }
    }
    lv_obj_update_layout(o);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* c = lv_obj_get_child(o, i);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) _fitWidths(c, depth + 1);
    }
}

void fitTopLayer()
{
    lv_obj_t* scope = _scope();
    if (scope == lv_scr_act()) return;
    uint32_t cnt = lv_obj_get_child_cnt(scope);
    // Once per overlay (USER_4 marks it done): boxes wider than the screen
    // get a 3 px margin, then everything inside is fitted.
    if (!lv_obj_has_flag(scope, LV_OBJ_FLAG_USER_4)) {
        lv_obj_add_flag(scope, LV_OBJ_FLAG_USER_4);
        lv_obj_update_layout(scope);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_t* c = lv_obj_get_child(scope, i);
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
            if (lv_obj_get_width(c) > OPS_SCREEN_W - 6) lv_obj_set_width(c, OPS_SCREEN_W - 6);
        }
        lv_obj_update_layout(scope);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_t* c = lv_obj_get_child(scope, i);
            if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) _fitWidths(c, 0);
        }
    }
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* c = lv_obj_get_child(scope, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_get_height(c) <= OPS_SCREEN_H) continue;
        lv_obj_set_height(c, OPS_SCREEN_H);
        lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(c, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_AUTO);
    }
}

void clear()
{
    _setHl(nullptr);
}

// ── Top bars ────────────────────────────────────────────────────────
// A screen's title bar: a direct child of the screen, full width, at the
// top, no taller than a title bar.
static bool _isTopBar(lv_obj_t* o)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return a.y1 == 0 && lv_obj_get_width(o) >= OPS_SCREEN_W - 2 &&
           lv_obj_get_height(o) <= OPS_TOP_H + 10;
}

// A bar button that only goes back: its label is Home or Close.
static bool _isBackBtn(lv_obj_t* btn)
{
    if (!lv_obj_has_class(btn, &lv_btn_class)) return false;
    lv_obj_t* l = lv_obj_get_child(btn, 0);
    if (!l || !lv_obj_has_class(l, &lv_label_class)) return false;
    const char* t = lv_label_get_text(l);
    if (!t) return false;
    return strncmp(t, LV_SYMBOL_HOME, strlen(LV_SYMBOL_HOME)) == 0 ||
           strcmp(t, LV_SYMBOL_CLOSE) == 0;
}

void trimTopBars()
{
    lv_obj_t* scr = lv_scr_act();
    uint32_t cnt = lv_obj_get_child_cnt(scr);
    for (uint32_t i = 0; i < cnt && i < 4; i++) {
        lv_obj_t* bar = lv_obj_get_child(scr, i);
        if (!_isTopBar(bar)) continue;
        uint32_t bc = lv_obj_get_child_cnt(bar);
        for (uint32_t j = 0; j < bc; j++) {
            lv_obj_t* b = lv_obj_get_child(bar, j);
            if (!lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN) && _isBackBtn(b))
                lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

bool clickTopBarBack()
{
    lv_obj_t* scr = lv_scr_act();
    uint32_t cnt = lv_obj_get_child_cnt(scr);
    for (uint32_t i = 0; i < cnt && i < 4; i++) {
        lv_obj_t* bar = lv_obj_get_child(scr, i);
        if (!_isTopBar(bar)) continue;
        uint32_t bc = lv_obj_get_child_cnt(bar);
        for (uint32_t j = 0; j < bc; j++) {
            lv_obj_t* b = lv_obj_get_child(bar, j);
            if (_isBackBtn(b) && _hasHandlers(b)) {
                lv_event_send(b, LV_EVENT_CLICKED, nullptr);
                return true;
            }
        }
    }
    return false;
}

}}}  // namespace ops::ui::keynav
