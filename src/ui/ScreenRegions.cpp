// Saitama — ScreenRegions.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenRegions.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../mesh/MeshService.h"
#include "../utils/Config.h"
#include "../utils/Log.h"
#include "../utils/Regions.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

namespace ops { namespace ui {

lv_obj_t* ScreenRegions::_screen      = nullptr;
lv_obj_t* ScreenRegions::_body        = nullptr;
lv_obj_t* ScreenRegions::_defaultLbl  = nullptr;
lv_obj_t* ScreenRegions::_statusLbl   = nullptr;
lv_obj_t* ScreenRegions::_repList     = nullptr;
lv_obj_t* ScreenRegions::_chips       = nullptr;
lv_obj_t* ScreenRegions::_discoverBtn = nullptr;

static constexpr int TOP_H   = 28;
static constexpr int MAX_REP = 8;   // matches MeshService's outstanding-request limit

// Last discovery results — kept across visits to the screen.
static ops::RegionReply s_reps[MAX_REP];
static int              s_repCount   = 0;
static bool             s_running    = false;
static bool             s_everRan    = false;
static int              s_asked      = 0;
static uint32_t         s_until      = 0;
// Regions discovered but not saved, offered as "+ NAME" chips.
static constexpr int    MAX_FOUND    = 16;
static char             s_found[MAX_FOUND][ops::regions::NAME_LEN];
static int              s_foundCount = 0;
static bool             s_retried    = false;
// Repeaters that never answered, named once discovery finishes.
static char             s_noReply[MAX_REP][32];
static int              s_noReplyCount = 0;

// ── Small builders ────────────────────────────────────────────────────

static lv_obj_t* _section(lv_obj_t* parent, const char* text)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_top(l, 4, 0);
    return l;
}

static lv_obj_t* _flexBox(lv_obj_t* parent, lv_flex_flow_t flow)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_width(o, lv_pct(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_row(o, 3, 0);
    lv_obj_set_style_pad_column(o, 4, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(o, flow);
    return o;
}

// ── Status / default ──────────────────────────────────────────────────

void ScreenRegions::_setStatus(const char* msg, lv_color_t col)
{
    if (!_statusLbl) return;
    lv_label_set_text(_statusLbl, msg);
    lv_obj_set_style_text_color(_statusLbl, col, 0);
}

void ScreenRegions::_refreshDefault()
{
    if (!_defaultLbl) return;
    const char* sc = ops::config::get().scopeTag;
    char buf[48];
    if (sc[0]) snprintf(buf, sizeof(buf), "Default scope: %s", sc);
    else       snprintf(buf, sizeof(buf), "Default scope: none (unscoped)");
    lv_label_set_text(_defaultLbl, buf);
    lv_obj_set_style_text_color(_defaultLbl, sc[0] ? theme::ACCENT : theme::TEXT_MUTED, 0);
}

// ── Nearby repeaters ──────────────────────────────────────────────────
// One row per reply: name, the regions it floods for, and reply SNR.

void ScreenRegions::_rebuildRepeaters()
{
    if (!_repList) return;
    lv_obj_clean(_repList);

    if (s_repCount == 0 && (s_running || s_noReplyCount == 0)) {
        lv_obj_t* l = lv_label_create(_repList);
        lv_label_set_text(l, s_everRan ? (s_running ? "Waiting for replies..." : "No replies.")
                                       : "Press Discover to ask repeaters in direct range.");
        lv_obj_set_style_text_color(l, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        return;
    }

    for (int i = 0; i < s_repCount; i++) {
        const ops::RegionReply& r = s_reps[i];
        lv_obj_t* row = _flexBox(_repList, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, theme::BG_CARD, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* nm = lv_label_create(row);
        lv_label_set_text(nm, r.repeater);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(nm, 104);
        lv_obj_set_style_text_color(nm, theme::TEXT, 0);
        lv_obj_set_style_text_font(nm, theme::bodyFont12(), 0);

        // "AU,NSW,*" → "AU | NSW | *" — ASCII only: montserrat has no "·".
        char regs[160] = {};
        if (!r.regions[0]) {
            snprintf(regs, sizeof(regs), "(no regions)");
        } else {
            const char* p = r.regions;
            while (*p && strlen(regs) < sizeof(regs) - 6) {
                if (*p == ',') {
                    if (p[1] && regs[0]) strncat(regs, " | ", sizeof(regs) - strlen(regs) - 1);
                } else {
                    size_t n = strlen(regs); regs[n] = *p; regs[n + 1] = '\0';
                }
                p++;
            }
        }
        lv_obj_t* rg = lv_label_create(row);
        lv_label_set_text(rg, regs);
        lv_label_set_long_mode(rg, LV_LABEL_LONG_WRAP);
        lv_obj_set_flex_grow(rg, 1);
        lv_obj_set_style_text_color(rg, r.regions[0] ? theme::ACCENT : theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(rg, &lv_font_montserrat_12, 0);

        char snr[16];
        snprintf(snr, sizeof(snr), "%+.1f dB", (double)r.snr);
        lv_obj_t* sl = lv_label_create(row);
        lv_label_set_text(sl, snr);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(sl, r.snr >= 5.0f ? theme::GREEN
                                      : r.snr >= -2.5f ? theme::ORANGE : theme::RED, 0);
    }

    if (s_running) return;
    for (int i = 0; i < s_noReplyCount; i++) {
        lv_obj_t* row = _flexBox(_repList, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, theme::BG_CARD, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* nm = lv_label_create(row);
        lv_label_set_text(nm, s_noReply[i]);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(nm, 104);
        lv_obj_set_style_text_color(nm, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(nm, theme::bodyFont12(), 0);

        lv_obj_t* l = lv_label_create(row);
        lv_label_set_text(l, "no reply");
        lv_obj_set_style_text_color(l, theme::ORANGE, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    }
}

// ── Saved-regions chips ───────────────────────────────────────────────
// "Off" plus every saved name; the current default is filled in. Tapping
// one makes it the default scope. user_data = catalog index + 1 (0 = Off).
// Then a dashed "+ NAME" chip for every discovered region not saved yet;
// tapping one saves it. user_data = s_found index.

static bool _isSaved(const char* name)
{
    for (int i = 0; i < ops::regions::count(); i++)
        if (strcmp(ops::regions::get(i), name) == 0) return true;
    return false;
}

// Collects the discovered region names that aren't saved ("*" isn't a region).
static void _collectFound()
{
    s_foundCount = 0;
    for (int r = 0; r < s_repCount; r++) {
        const char* p = s_reps[r].regions;
        while (*p) {
            const char* end = strchr(p, ',');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            while (n && *p == '#') { p++; n--; }
            if (n > 0 && n < ops::regions::NAME_LEN && !(n == 1 && *p == '*')) {
                char nm[ops::regions::NAME_LEN];
                memcpy(nm, p, n);
                nm[n] = '\0';
                bool dup = _isSaved(nm);
                for (int k = 0; k < s_foundCount && !dup; k++)
                    if (strcmp(s_found[k], nm) == 0) dup = true;
                if (!dup && s_foundCount < MAX_FOUND)
                    memcpy(s_found[s_foundCount++], nm, n + 1);
            }
            if (!end) break;
            p = end + 1;
        }
    }
}

void ScreenRegions::_rebuildChips()
{
    if (!_chips) return;
    lv_obj_clean(_chips);
    const char* cur = ops::config::get().scopeTag;
    lv_group_t* g = lv_group_get_default();

    for (int i = -1; i < ops::regions::count(); i++) {
        const char* name = (i < 0) ? "Off" : ops::regions::get(i);
        bool active = (i < 0) ? !cur[0] : strcmp(cur, name) == 0;

        lv_obj_t* b = lv_btn_create(_chips);
        lv_obj_set_height(b, 24);
        lv_obj_set_style_pad_hor(b, 10, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, active ? theme::ACCENT : theme::BORDER, 0);
        lv_obj_set_style_border_width(b, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(b, theme::TEXT, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(b, active ? theme::ACCENT : theme::BG_CARD, 0);
        lv_obj_add_event_cb(b, _onChipClick, LV_EVENT_CLICKED, (void*)(intptr_t)(i + 1));
        lv_obj_add_event_cb(b, _onKey,       LV_EVENT_KEY,     nullptr);
        if (g) lv_group_add_obj(g, b);

        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, name);
        lv_obj_set_style_text_color(l, active ? theme::BG : theme::TEXT, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_center(l);
    }

    _collectFound();
    for (int i = 0; i < s_foundCount; i++) {
        lv_obj_t* b = lv_btn_create(_chips);
        lv_obj_set_height(b, 24);
        lv_obj_set_style_pad_hor(b, 10, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(b, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, theme::TEXT_MUTED, 0);
        lv_obj_set_style_border_width(b, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(b, theme::TEXT, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(b, _onAddChipClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(b, _onKey,          LV_EVENT_KEY,     nullptr);
        if (g) lv_group_add_obj(g, b);

        char txt[ops::regions::NAME_LEN + 4];
        snprintf(txt, sizeof(txt), LV_SYMBOL_PLUS " %s", s_found[i]);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_center(l);
    }

    if (ops::regions::count() == 0 && s_foundCount == 0) {
        lv_obj_t* l = lv_label_create(_chips);
        lv_label_set_text(l, "Saved regions appear here. Discover offers new ones to add.");
        lv_obj_set_style_text_color(l, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    }
}

// ── Events ────────────────────────────────────────────────────────────

void ScreenRegions::_onHomeClick(lv_event_t* /*e*/) { ScreenLauncher::show(); }

void ScreenRegions::_onKey(lv_event_t* e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC) ScreenLauncher::show();
}

void ScreenRegions::_onChipClick(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e) - 1;
    const char* name = (idx < 0) ? "" : ops::regions::get(idx);
    if (!name) return;

    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    strncpy(cfg.scopeTag, name, sizeof(cfg.scopeTag) - 1);
    cfg.scopeTag[sizeof(cfg.scopeTag) - 1] = '\0';
    ops::config::save();
    OPS_LOG("Regions", "Default scope: %s", name[0] ? name : "(none)");

    char msg[48];
    if (name[0]) snprintf(msg, sizeof(msg), "Default scope set to %s.", name);
    else         snprintf(msg, sizeof(msg), "Default scope off - floods unscoped.");
    _setStatus(msg, theme::GREEN);
    _refreshDefault();
    // Rebuilding deletes the chip whose handler is running — defer it.
    lv_async_call([](void*) { _rebuildChips(); }, nullptr);
}

void ScreenRegions::_onAddChipClick(lv_event_t* e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_foundCount) return;
    char msg[64];
    if (ops::regions::count() >= ops::regions::MAX_REGIONS) {
        snprintf(msg, sizeof(msg), "Saved regions are full (9). Remove one in Settings.");
        _setStatus(msg, theme::ORANGE);
        return;
    }
    ops::regions::add(s_found[i]);
    snprintf(msg, sizeof(msg), "Saved %s. Tap it to make it the default.", s_found[i]);
    _setStatus(msg, theme::GREEN);
    // Rebuilding deletes the chip whose handler is running — defer it.
    lv_async_call([](void*) { _rebuildChips(); }, nullptr);
}

void ScreenRegions::_onDiscoverClick(lv_event_t* /*e*/)
{
    if (s_running) return;
    auto& mesh = ops::MeshService::instance();
    int n = mesh.discoverRegions();
    s_everRan  = true;
    s_repCount = 0;
    s_noReplyCount = 0;
    s_retried  = false;
    s_asked    = n;
    s_running  = true;
    // Repeaters found by the scan are asked as they answer, so wait out the
    // scan and then long enough for the last one asked to reply.
    uint32_t scan = mesh.regionScanMs() + mesh.regionWaitMs(1);
    uint32_t wait = mesh.regionWaitMs(n);
    s_until = millis() + (scan > wait ? scan : wait);
    _setStatus("Looking for repeaters in direct range...", theme::ACCENT);
    _rebuildRepeaters();
}

// ── tick() ────────────────────────────────────────────────────────────

void ScreenRegions::tick()
{
    if (!s_running) return;

    auto& mesh = ops::MeshService::instance();
    bool changed = false;
    int asked = mesh.regionAskedCount();
    if (asked != s_asked) {
        // The scan found another repeater and asked it — give it time to answer.
        s_asked = asked;
        uint32_t until = millis() + mesh.regionWaitMs(1);
        if ((int32_t)(until - s_until) > 0) s_until = until;
        changed = true;
    }
    ops::RegionReply r;
    while (ops::MeshService::instance().pollRegionReply(r)) {
        if (s_repCount < MAX_REP) s_reps[s_repCount++] = r;
        changed = true;
    }
    bool timedOut = millis() > s_until;
    bool retrying = false;
    if (s_repCount < s_asked && timedOut && !s_retried) {
        // One more try for the quiet ones — a missed packet or a collision
        // between replies is the common reason. Only once: repeaters allow
        // 4 anonymous requests per 3 minutes.
        s_retried = true;
        int k = ops::MeshService::instance().retryRegions();
        if (k > 0) {
            s_until  = millis() + ops::MeshService::instance().regionWaitMs(k);
            timedOut = false;
            retrying = true;
            OPS_LOG("Regions", "Retrying %d repeater(s)", k);
        }
    }
    bool done = (s_asked > 0 && s_repCount >= s_asked && !mesh.regionScanning()) || timedOut;
    if (done) {
        s_running = false;
        s_noReplyCount = ops::MeshService::instance().regionUnanswered(s_noReply, MAX_REP);
    }

    // Only touch widgets while the screen is showing.
    if (!_screen || lv_scr_act() != _screen) return;
    if (changed && !done && s_asked > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Asking %d repeater(s)...", s_asked);
        _setStatus(msg, theme::ACCENT);
    }
    if (changed || done) {
        _rebuildRepeaters();
        _rebuildChips();   // replies add names to the catalog
    }
    if (retrying) {
        char msg[48];
        snprintf(msg, sizeof(msg), "%d of %d replied - asking the rest again...",
                 s_repCount, s_asked);
        _setStatus(msg, theme::ACCENT);
    }
    if (done) {
        char msg[72];
        if (s_asked == 0)
            snprintf(msg, sizeof(msg), "No repeaters in direct range answered the scan.");
        else if (s_repCount == 0)
            snprintf(msg, sizeof(msg), "Asked %d repeater(s); none replied.", s_asked);
        else
            snprintf(msg, sizeof(msg), "%d of %d replied.  * = also accepts unscoped.",
                     s_repCount, s_asked);
        _setStatus(msg, s_repCount ? theme::GREEN : theme::ORANGE);
    }
}

// ── _build() ──────────────────────────────────────────────────────────

void ScreenRegions::_build()
{
    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ──
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

    lv_obj_t* home = lv_btn_create(bar);
    lv_group_remove_obj(home);
    lv_obj_set_height(home, TOP_H - 6);
    lv_obj_set_style_bg_color(home, theme::BG, 0);
    lv_obj_set_style_bg_color(home, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(home, theme::BORDER, 0);
    lv_obj_set_style_border_width(home, 1, 0);
    lv_obj_set_style_radius(home, 4, 0);
    lv_obj_set_style_shadow_width(home, 0, 0);
    lv_obj_set_style_pad_hor(home, 5, 0);
    lv_obj_add_event_cb(home, _onHomeClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* homeLbl = lv_label_create(home);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, "Regions");
    lv_obj_set_style_text_color(title, theme::TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_flex_grow(title, 1);

    _discoverBtn = lv_btn_create(bar);
    lv_obj_set_height(_discoverBtn, TOP_H - 6);
    lv_obj_set_style_bg_color(_discoverBtn, theme::PRIMARY, 0);
    lv_obj_set_style_bg_color(_discoverBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(_discoverBtn, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(_discoverBtn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(_discoverBtn, 4, 0);
    lv_obj_set_style_shadow_width(_discoverBtn, 0, 0);
    lv_obj_set_style_pad_hor(_discoverBtn, 8, 0);
    lv_obj_add_event_cb(_discoverBtn, _onDiscoverClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(_discoverBtn, _onKey,           LV_EVENT_KEY,     nullptr);
    lv_obj_t* dl = lv_label_create(_discoverBtn);
    lv_label_set_text(dl, LV_SYMBOL_GPS " Discover");
    lv_obj_set_style_text_color(dl, theme::TEXT, 0);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_12, 0);
    lv_obj_center(dl);

    // ── Body: scrolling column ──
    _body = lv_obj_create(_screen);
    lv_obj_set_size(_body, OPS_SCREEN_W, OPS_SCREEN_H - TOP_H);
    lv_obj_align(_body, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(_body, theme::BG, 0);
    lv_obj_set_style_border_width(_body, 0, 0);
    lv_obj_set_style_radius(_body, 0, 0);
    lv_obj_set_style_pad_hor(_body, 6, 0);
    lv_obj_set_style_pad_ver(_body, 4, 0);
    lv_obj_set_style_pad_row(_body, 4, 0);
    lv_obj_set_scrollbar_mode(_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(_body, _onKey, LV_EVENT_KEY, nullptr);

    _defaultLbl = lv_label_create(_body);
    lv_obj_set_style_text_font(_defaultLbl, &lv_font_montserrat_14, 0);
    _refreshDefault();

    _statusLbl = lv_label_create(_body);
    lv_obj_set_width(_statusLbl, lv_pct(100));
    lv_label_set_long_mode(_statusLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_statusLbl, &lv_font_montserrat_10, 0);
    _setStatus("Tap a saved region to make it the default.", theme::TEXT_MUTED);

    _section(_body, "Nearby repeaters");
    _repList = _flexBox(_body, LV_FLEX_FLOW_COLUMN);

    _section(_body, "Saved regions - tap to use, + to save");
    _chips = _flexBox(_body, LV_FLEX_FLOW_ROW_WRAP);

    lv_group_t* g = lv_group_get_default();
    if (g) lv_group_add_obj(g, _discoverBtn);

    _rebuildRepeaters();
    _rebuildChips();
    if (g) lv_group_focus_obj(_discoverBtn);

    lv_scr_load(_screen);
}

void ScreenRegions::show()
{
    lv_obj_t* old = _screen;
    _screen = _body = _defaultLbl = _statusLbl = _repList = _chips = _discoverBtn = nullptr;
    _build();
    if (old) lv_obj_del(old);
}

}}  // namespace ops::ui
