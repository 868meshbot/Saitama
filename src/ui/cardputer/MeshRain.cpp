// Saitama — cardputer/MeshRain.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "MeshRain.h"
#include "../BootAnim.h"
#include "../Theme.h"
#include "../../mesh/MeshService.h"
#include "../../utils/Config.h"
#include "../../utils/Contacts.h"
#include "../Emoji.h"
#include <lvgl.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

namespace ops {

namespace {

constexpr int CHAN_DROPS = 2;
constexpr int USER_DROPS = 3;
constexpr int DROPS      = CHAN_DROPS + USER_DROPS;   // channels first
constexpr int LINKS      = 2;
constexpr int TICK_MS    = 50;
constexpr int TOP_Y      = 0;                         // names start at the top
constexpr int CLOCK_TICKS = 2;                       // clock falls 1 px every 2 ticks
constexpr uint32_t LINK_MS = 15000;

struct Drop {
    lv_obj_t* lbl;
    char      name[32];
    int16_t   y;          // x is fixed per fall
    uint8_t   speed;      // px per tick
    bool      live;
};

struct Link {
    lv_obj_t*  line;
    lv_point_t pts[2];
    int8_t     a, b;      // drop indices
    uint32_t   startMs;
};

lv_obj_t*   s_parent  = nullptr;
lv_obj_t*   s_clock   = nullptr;
lv_timer_t* s_timer   = nullptr;
Drop        s_drops[DROPS];
Link        s_links[LINKS];
uint32_t    s_clockTick = 0;
int16_t     s_clockY    = 0;
uint8_t     s_clockStep = 0;

bool _isChan(int i) { return i < CHAN_DROPS; }

// Random station heard this session (chat nodes; not blocked, not on screen).
bool _pickUser(char* out, size_t max)
{
    auto& svc = MeshService::instance();
    int n = svc.peerCount();
    if (n <= 0) return false;
    int start = (int)lv_rand(0, n - 1);
    for (int k = 0; k < n; k++) {
        PeerInfo p;
        if (!svc.getPeer((start + k) % n, p)) continue;
        if (p.type != 1 || !p.name[0] || contacts::isBlockedName(p.name)) continue;
        bool shown = false;
        for (int i = CHAN_DROPS; i < DROPS; i++)
            if (s_drops[i].live && strcmp(s_drops[i].name, p.name) == 0) shown = true;
        if (shown) continue;
        strncpy(out, p.name, max - 1);
        out[max - 1] = '\0';
        return true;
    }
    return false;
}

// Random configured channel not already falling.
bool _pickChannel(char* out, size_t max)
{
    const auto& cfg = config::get();
    int start = (int)lv_rand(0, 9);
    for (int k = 0; k < 10; k++) {
        const auto& ch = cfg.channels[(start + k) % 10];
        if (!ch.name[0]) continue;
        char tag[34];
        snprintf(tag, sizeof(tag), "#%s", ch.name);
        bool shown = false;
        for (int i = 0; i < CHAN_DROPS; i++)
            if (s_drops[i].live && strcmp(s_drops[i].name, tag) == 0) shown = true;
        if (shown) continue;
        strncpy(out, tag, max - 1);
        out[max - 1] = '\0';
        return true;
    }
    return false;
}

void _place(int i, const char* name, int16_t y)
{
    Drop& d = s_drops[i];
    strncpy(d.name, name, sizeof(d.name) - 1);
    d.name[sizeof(d.name) - 1] = '\0';
    char shown[32];
    strncpy(shown, d.name, sizeof(shown));
    theme::sanitizeText(shown);
    lv_label_set_text(d.lbl, shown);
    lv_obj_update_layout(d.lbl);
    int w = lv_obj_get_width(d.lbl);
    int maxX = OPS_SCREEN_W - w - 2;
    lv_obj_set_pos(d.lbl, (lv_coord_t)lv_rand(2, maxX > 2 ? maxX : 2), y);
    d.y     = y;
    d.speed = (uint8_t)lv_rand(1, 2);
    d.live  = true;
    lv_obj_clear_flag(d.lbl, LV_OBJ_FLAG_HIDDEN);
}

// New name for drop i of the same kind; hidden if there is none.
void _respawn(int i, bool stagger)
{
    char name[34];
    bool ok = _isChan(i) ? _pickChannel(name, sizeof(name)) : _pickUser(name, sizeof(name));
    if (!ok) {
        s_drops[i].live = false;
        lv_obj_add_flag(s_drops[i].lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int16_t y = stagger ? (int16_t)lv_rand(TOP_Y, OPS_SCREEN_H - 20) : (int16_t)TOP_Y;
    _place(i, name, y);
}

void _updateClock()
{
    if (!s_clock) return;
    time_t now = config::localEpoch();
    char buf[24];
    if (now < 1700000000) {
        snprintf(buf, sizeof(buf), "--:--");
    } else {
        struct tm t;
        gmtime_r(&now, &t);
        // U+1F319 moon in night mode, as beside the launcher clock.
        snprintf(buf, sizeof(buf), "%02d:%02d%s", t.tm_hour, t.tm_min,
                 config::nightModeActive() ? " \xF0\x9F\x8C\x99" : "");
    }
    lv_label_set_text(s_clock, buf);
}

// The clock falls too, slowly, from a new random x each pass, so nothing
// stays lit in one place (screen burn).
void _placeClock(int16_t y)
{
    lv_obj_update_layout(s_clock);
    int w = lv_obj_get_width(s_clock);
    int maxX = OPS_SCREEN_W - w - 2;
    lv_obj_set_pos(s_clock, (lv_coord_t)lv_rand(2, maxX > 2 ? maxX : 2), y);
    s_clockY = y;
}

lv_point_t _centre(int i)
{
    lv_area_t a;
    lv_obj_get_coords(s_drops[i].lbl, &a);
    lv_point_t p = { (lv_coord_t)((a.x1 + a.x2) / 2), (lv_coord_t)((a.y1 + a.y2) / 2) };
    return p;
}

void _tick(lv_timer_t*)
{
    for (int i = 0; i < DROPS; i++) {
        Drop& d = s_drops[i];
        if (!d.live) {
            // Something to show now (a station heard since)? Start falling.
            if (lv_rand(0, 40) == 0) _respawn(i, false);
            continue;
        }
        d.y += d.speed;
        if (d.y > OPS_SCREEN_H) { _respawn(i, false); continue; }
        lv_obj_set_y(d.lbl, d.y);
    }

    uint32_t now = lv_tick_get();
    for (Link& l : s_links) {
        if (!l.line) continue;
        uint32_t age = now - l.startMs;
        if (age >= LINK_MS || !s_drops[l.a].live || !s_drops[l.b].live) {
            lv_obj_del(l.line);
            l.line = nullptr;
            continue;
        }
        l.pts[0] = _centre(l.a);
        l.pts[1] = _centre(l.b);
        lv_line_set_points(l.line, l.pts, 2);
        lv_obj_set_style_line_opa(l.line, (lv_opa_t)(LV_OPA_COVER * (LINK_MS - age) / LINK_MS), 0);
    }

    if (++s_clockTick >= 20) {   // once a second
        s_clockTick = 0;
        _updateClock();
    }
    if (s_clock && ++s_clockStep >= CLOCK_TICKS) {
        s_clockStep = 0;
        if (++s_clockY > OPS_SCREEN_H) _placeClock((int16_t)-lv_obj_get_height(s_clock));
        else                           lv_obj_set_y(s_clock, s_clockY);
    }
}

int _findDrop(const char* name, bool chan)
{
    int from = chan ? 0 : CHAN_DROPS, to = chan ? CHAN_DROPS : DROPS;
    for (int i = from; i < to; i++)
        if (s_drops[i].live && strcmp(s_drops[i].name, name) == 0) return i;
    return -1;
}

// Puts name on screen in a drop of its kind, replacing the lowest one.
int _ensureDrop(const char* name, bool chan)
{
    int i = _findDrop(name, chan);
    if (i >= 0) return i;
    int from = chan ? 0 : CHAN_DROPS, to = chan ? CHAN_DROPS : DROPS;
    int pick = from;
    for (int k = from; k < to; k++) {
        if (!s_drops[k].live) { pick = k; break; }
        if (s_drops[k].y > s_drops[pick].y) pick = k;
    }
    _place(pick, name, (int16_t)lv_rand(TOP_Y, OPS_SCREEN_H / 2));
    return pick;
}

}  // namespace

// ── bootanim API (screensaver style 1) ──────────────────────────────
namespace bootanim {

void create(lv_obj_t* parent)
{
    destroy();
    s_parent = parent;

    s_clock = lv_label_create(parent);
    lv_obj_set_style_text_font(s_clock, emoji::emojiFont(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(s_clock, theme::TEXT, 0);
    _updateClock();
    _placeClock((int16_t)lv_rand(0, OPS_SCREEN_H / 2));
    s_clockStep = 0;

    for (int i = 0; i < DROPS; i++) {
        Drop& d = s_drops[i];
        d.lbl = lv_label_create(parent);
        lv_obj_set_style_text_font(d.lbl, theme::bodyFont12(), 0);
        lv_obj_set_style_text_color(d.lbl, _isChan(i) ? theme::ACCENT : theme::GREEN, 0);
        d.live = false;
        lv_obj_add_flag(d.lbl, LV_OBJ_FLAG_HIDDEN);
        _respawn(i, true);
    }
    for (Link& l : s_links) l.line = nullptr;

    s_clockTick = 0;
    s_timer = lv_timer_create(_tick, TICK_MS, nullptr);
}

void destroy()
{
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    // The objects belong to the screensaver screen and go with it.
    s_parent = nullptr;
    s_clock  = nullptr;
    for (Drop& d : s_drops) { d.lbl = nullptr; d.live = false; }
    for (Link& l : s_links) l.line = nullptr;
}

}  // namespace bootanim

namespace meshrain {

void noteMessage(const char* sender, const char* channel)
{
    if (!s_timer || !sender || !sender[0]) return;
    int a = _ensureDrop(sender, false);
    if (!channel || !channel[0]) return;
    char tag[34];
    snprintf(tag, sizeof(tag), "#%s", channel);
    int b = _ensureDrop(tag, true);

    Link* slot = &s_links[0];
    for (Link& l : s_links) {
        if (!l.line) { slot = &l; break; }
        if (l.startMs < slot->startMs) slot = &l;
    }
    if (slot->line) lv_obj_del(slot->line);
    slot->line = lv_line_create(s_parent);
    lv_obj_set_style_line_color(slot->line, theme::ACCENT, 0);
    lv_obj_set_style_line_width(slot->line, 1, 0);
    lv_obj_move_background(slot->line);
    slot->a = (int8_t)a;
    slot->b = (int8_t)b;
    slot->startMs = lv_tick_get();
    slot->pts[0] = _centre(a);
    slot->pts[1] = _centre(b);
    lv_line_set_points(slot->line, slot->pts, 2);
}

}  // namespace meshrain
}  // namespace ops
