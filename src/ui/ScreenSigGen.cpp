// Saitama — ScreenSigGen.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// RF Signal Generator — uses SX1262 SetTxContinuousWave (CW) and
// SetTxInfinitePreamble (LoRa preamble) modes for antenna testing.
//
// Layout (320 × 240):
//
//   ┌────────────────────────────────────────┐  y=0
//   │ [⌂]  Signal Generator       ⚡ ACTIVE  │  28 px  top bar
//   ├────────────────────────────────────────┤  y=28
//   │                                        │
//   │           868.618 MHz                  │  48 px  large freq
//   │                                        │
//   │           CW  ·  +22 dBm              │  20 px  params
//   │                                        │
//   │  ┌──────────────────────────────────┐  │
//   │  │          START TX                │  │  44 px  button
//   │  └──────────────────────────────────┘  │
//   │                                        │
//   │           Auto-stop: 5:00              │  20 px  countdown
//   │                                        │
//   ├────────────────────────────────────────┤  y=208
//   │  ↑↓ power  ←→ freq  c:CW/preamble  ⌫  │  32 px  hint bar
//   └────────────────────────────────────────┘  y=240

#include "ScreenSigGen.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../mesh/MeshService.h"
#include "../utils/Log.h"
#include <lvgl.h>
#include <cstdio>
#include <cstring>

namespace ops { namespace ui {

static constexpr int TOP_H = 28;
static constexpr int BOT_H = 32;
static constexpr uint32_t TX_TIMEOUT_MS = 5 * 60 * 1000;  // 5 minutes

// ── State ─────────────────────────────────────────────────────────────────────
static float   s_freqMHz    = 868.618f;
static int8_t  s_powerDbm   = 22;
static bool    s_loraMode   = false;  // false=CW, true=LoRa preamble
static bool    s_txActive   = false;
static uint32_t s_txStartMs = 0;

// Freq step sizes (cycle through with trackball clicks)
static constexpr float kFreqSteps[] = { 0.1f, 1.0f, 5.0f };
static int s_freqStepIdx = 0;

// ── Static members ────────────────────────────────────────────────────────────
lv_obj_t* ScreenSigGen::_screen     = nullptr;
lv_obj_t* ScreenSigGen::_freqLbl    = nullptr;
lv_obj_t* ScreenSigGen::_statLbl    = nullptr;
lv_obj_t* ScreenSigGen::_paramLbl   = nullptr;
lv_obj_t* ScreenSigGen::_txBtn      = nullptr;
lv_obj_t* ScreenSigGen::_txBtnLbl   = nullptr;
lv_obj_t* ScreenSigGen::_cntdownLbl = nullptr;

// ── helpers ───────────────────────────────────────────────────────────────────

void ScreenSigGen::_startTx()
{
    if (s_txActive) return;
    if (ops::MeshService::instance().sigGenStart(s_freqMHz, s_powerDbm, s_loraMode)) {
        s_txActive  = true;
        s_txStartMs = millis();
        OPS_LOG("SigGen", "TX on %.3f MHz %s %+d dBm",
                (double)s_freqMHz, s_loraMode ? "preamble" : "CW", (int)s_powerDbm);
    }
}

void ScreenSigGen::_stopTx()
{
    if (!s_txActive) return;
    ops::MeshService::instance().sigGenStop();
    s_txActive = false;
    OPS_LOG("SigGen", "TX off");
}

void ScreenSigGen::_refreshLabels()
{
    if (!_screen) return;

    // Frequency
    char buf[24];
    snprintf(buf, sizeof(buf), "%.3f MHz", (double)s_freqMHz);
    lv_label_set_text(_freqLbl, buf);

    // Params
    snprintf(buf, sizeof(buf), "%s  ·  %+d dBm",
             s_loraMode ? "LoRa preamble" : "CW",
             (int)s_powerDbm);
    lv_label_set_text(_paramLbl, buf);

    // TX button label
    lv_label_set_text(_txBtnLbl, s_txActive ? "STOP TX" : "START TX");
    lv_obj_set_style_bg_color(_txBtn,
        s_txActive ? lv_color_make(180, 30, 30) : lv_color_make(30, 140, 30), 0);

    // Status indicator in top bar
    lv_label_set_text(_statLbl, s_txActive ? "TX ACTIVE" : "");
    lv_obj_set_style_text_color(_statLbl, lv_color_make(255, 80, 0), 0);

    // Countdown
    if (s_txActive) {
        uint32_t elapsed = millis() - s_txStartMs;
        uint32_t rem = (TX_TIMEOUT_MS > elapsed) ? (TX_TIMEOUT_MS - elapsed) : 0;
        uint32_t mins = rem / 60000;
        uint32_t secs = (rem % 60000) / 1000;
        snprintf(buf, sizeof(buf), "Auto-stop: %lu:%02lu", mins, secs);
        lv_label_set_text(_cntdownLbl, buf);
        lv_obj_set_style_text_color(_cntdownLbl, lv_color_make(200, 100, 0), 0);
    } else {
        lv_label_set_text(_cntdownLbl, "");
    }
}

// ── navigate() ────────────────────────────────────────────────────────────────
void ScreenSigGen::navigate(int dx, int dy)
{
    bool changed = false;
    if (dx != 0) {
        float step = kFreqSteps[s_freqStepIdx] * (dx > 0 ? 1 : -1);
        s_freqMHz += step;
        if (s_freqMHz < 150.0f) s_freqMHz = 150.0f;
        if (s_freqMHz > 960.0f) s_freqMHz = 960.0f;
        changed = true;
    }
    if (dy != 0) {
        int p = (int)s_powerDbm - (dy > 0 ? -1 : 1);  // up = higher power
        if (p < -9) p = -9;
        if (p > 22) p = 22;
        s_powerDbm = (int8_t)p;
        changed = true;
    }
    if (changed) {
        // If TX is running, restart it with new params
        if (s_txActive) {
            _stopTx();
            _startTx();
        }
        _refreshLabels();
    }
}

// ── update() ──────────────────────────────────────────────────────────────────
void ScreenSigGen::update()
{
    if (!_screen || lv_scr_act() != _screen) return;

    // Auto-stop
    if (s_txActive && (millis() - s_txStartMs >= TX_TIMEOUT_MS)) {
        _stopTx();
        lv_label_set_text(_cntdownLbl, "Auto-stopped (5 min limit)");
        lv_obj_set_style_text_color(_cntdownLbl, lv_color_make(200, 60, 0), 0);
    }

    // Refresh countdown every second
    static uint32_t s_lastRefresh = 0;
    if (millis() - s_lastRefresh >= 500UL) {
        s_lastRefresh = millis();
        _refreshLabels();
    }
}

// ── isActive() ────────────────────────────────────────────────────────────────
bool ScreenSigGen::isActive()
{
    return _screen && lv_scr_act() == _screen;
}

// ── callbacks ─────────────────────────────────────────────────────────────────
void ScreenSigGen::_onTxBtn(lv_event_t*)
{
    if (s_txActive) _stopTx();
    else            _startTx();
    _refreshLabels();
}

void ScreenSigGen::_onKey(lv_event_t* e)
{
    uint32_t* key = (uint32_t*)lv_event_get_param(e);
    if (!key) return;

    if (*key == LV_KEY_ESC) {
        _stopTx();
        ScreenLauncher::show();
        return;
    }
    if (*key == 'c' || *key == 'C') {
        s_loraMode = !s_loraMode;
        if (s_txActive) { _stopTx(); _startTx(); }
        _refreshLabels();
        return;
    }
    // Cycle frequency step size with 's'
    if (*key == 's' || *key == 'S') {
        s_freqStepIdx = (s_freqStepIdx + 1) % 3;
        _refreshLabels();
        return;
    }
    // Enter/space toggles TX
    if (*key == LV_KEY_ENTER || *key == ' ') {
        _onTxBtn(nullptr);
    }
}

void ScreenSigGen::_onHome(lv_event_t*)
{
    _stopTx();
    ScreenLauncher::show();
}

// ── _buildScreen() ────────────────────────────────────────────────────────────
void ScreenSigGen::_buildScreen()
{
    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ───────────────────────────────────────────────────────────────
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
    lv_obj_add_event_cb(homeBtn, _onHome, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    lv_obj_t* titleLbl = lv_label_create(bar);
    lv_label_set_text(titleLbl, LV_SYMBOL_CHARGE " Signal Gen");
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_flex_grow(titleLbl, 1);

    _statLbl = lv_label_create(bar);
    lv_label_set_text(_statLbl, "");
    lv_obj_set_style_text_font(_statLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(_statLbl, lv_color_make(255, 80, 0), 0);

    // ── Content area ──────────────────────────────────────────────────────────
    static constexpr int CONTENT_Y = TOP_H + 4;
    static constexpr int CONTENT_H = OPS_SCREEN_H - TOP_H - BOT_H;

    // Large frequency label
    _freqLbl = lv_label_create(_screen);
    lv_label_set_text(_freqLbl, "868.618 MHz");
    lv_obj_set_style_text_color(_freqLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(_freqLbl, &lv_font_montserrat_36, 0);
    lv_obj_align(_freqLbl, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 10);

    // Mode / power params
    _paramLbl = lv_label_create(_screen);
    lv_label_set_text(_paramLbl, "CW  ·  +22 dBm");
    lv_obj_set_style_text_color(_paramLbl, lv_color_make(140, 180, 140), 0);
    lv_obj_set_style_text_font(_paramLbl, &lv_font_montserrat_14, 0);
    lv_obj_align(_paramLbl, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 58);

    // TX button
    _txBtn = lv_btn_create(_screen);
    lv_obj_set_size(_txBtn, OPS_SCREEN_W - 40, 44);
    lv_obj_align(_txBtn, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 88);
    lv_obj_set_style_bg_color(_txBtn, lv_color_make(30, 140, 30), 0);
    lv_obj_set_style_bg_color(_txBtn, lv_color_make(60, 180, 60), LV_STATE_PRESSED);
    lv_obj_set_style_radius(_txBtn, 6, 0);
    lv_obj_set_style_shadow_width(_txBtn, 0, 0);
    lv_obj_set_style_border_width(_txBtn, 0, 0);
    lv_obj_add_event_cb(_txBtn, _onTxBtn, LV_EVENT_CLICKED, nullptr);
    lv_group_remove_obj(_txBtn);

    _txBtnLbl = lv_label_create(_txBtn);
    lv_label_set_text(_txBtnLbl, "START TX");
    lv_obj_set_style_text_color(_txBtnLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(_txBtnLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(_txBtnLbl);

    // Countdown
    _cntdownLbl = lv_label_create(_screen);
    lv_label_set_text(_cntdownLbl, "");
    lv_obj_set_style_text_font(_cntdownLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(_cntdownLbl, lv_color_make(200, 100, 0), 0);
    lv_obj_align(_cntdownLbl, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 142);

    // ── Bottom hint bar ───────────────────────────────────────────────────────
    lv_obj_t* bot = lv_obj_create(_screen);
    lv_obj_set_size(bot, OPS_SCREEN_W, BOT_H);
    lv_obj_set_pos(bot, 0, OPS_SCREEN_H - BOT_H);
    lv_obj_set_style_bg_color(bot, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_radius(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 2, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* hint = lv_label_create(bot);
    lv_label_set_text(hint, "\xe2\x86\x91\xe2\x86\x93power  \xe2\x86\x90\xe2\x86\x92freq  c:mode  s:step  \xe2\x8c\xab exit");
    lv_obj_set_style_text_color(hint, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_align(hint, LV_ALIGN_LEFT_MID, 2, 0);

    // Key handler — canvas-style focus on an invisible overlay obj
    lv_obj_t* focusObj = lv_obj_create(_screen);
    lv_obj_set_size(focusObj, 1, 1);
    lv_obj_set_pos(focusObj, 0, 0);
    lv_obj_set_style_bg_opa(focusObj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(focusObj, 0, 0);
    lv_obj_add_event_cb(focusObj, _onKey, LV_EVENT_KEY, nullptr);
    lv_group_add_obj(lv_group_get_default(), focusObj);
    lv_group_focus_obj(focusObj);
}

// ── show() ────────────────────────────────────────────────────────────────────
void ScreenSigGen::show()
{
    if (!_screen) _buildScreen();
    if (!_screen) return;

    s_freqMHz   = ops::MeshService::instance().getFreqMHz();
    s_powerDbm  = 22;
    s_loraMode  = false;
    s_freqStepIdx = 0;
    // Don't carry over TX state from previous session
    s_txActive  = false;

    _refreshLabels();
    lv_scr_load(_screen);
}

}}  // namespace ops::ui
