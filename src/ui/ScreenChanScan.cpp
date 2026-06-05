// Saitama — ScreenChanScan.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Hardware CAD channel scanner centred on the active mesh frequency.
// Runs SX1262 SetCAD on each channel per sweep, tracks 50-sweep rolling
// hit rate and displays activity level as a live bar.
//
// Layout (320 × 240):
//
//   ┌────────────────────────────────────────┐  y=0
//   │ [⌂]  Channel Scanner        ±200kHz   │  28 px  top bar
//   ├────────────────────────────────────────┤  y=28
//   │ 868.725  ░░░░░░░░░░░░░░░░░░   0%  low  │  22 px  row 0  (−4 step)
//   │ 868.925  ░░░░░░░░░░░░░░░░░░   0%  low  │         row 1
//   │ 869.125  ░░░░░░░░░░░░░░░░░░   0%  low  │         row 2
//   │ 869.325  ░░░░░░░░░░░░░░░░░░   0%  low  │         row 3
//   │ 869.525* ████████░░░░░░░░░░  40%  HIGH │         row 4  ← mesh (center)
//   │ 869.725  ░░░░░░░░░░░░░░░░░░   0%  low  │         row 5
//   │ 869.925  ░░░░░░░░░░░░░░░░░░   0%  low  │         row 6
//   │ 870.125  ░░░░░░░░░░░░░░░░░░   0%  low  │         row 7
//   │ 870.325  ░░░░░░░░░░░░░░░░░░   0%  low  │         row 8  (+4 step)
//   ├────────────────────────────────────────┤  y=226
//   │ ←→ step   * = mesh ch   CAD SF8/BW62.5│  14 px  status
//   └────────────────────────────────────────┘  y=240

#include "ScreenChanScan.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../mesh/MeshService.h"
#include "../utils/Log.h"
#include <lvgl.h>
#include <cstdio>
#include <cstring>

namespace ops { namespace ui {

// ── Step options ──────────────────────────────────────────────────────────────

static const float kSteps[]      = { 0.100f, 0.200f, 0.500f };
static const char* kStepLabels[] = { "\xc2\xb1""100kHz", "\xc2\xb1""200kHz", "\xc2\xb1""500kHz" };
static constexpr int STEP_COUNT  = 3;
static constexpr int ROLL        = 50;

// ── Layout ────────────────────────────────────────────────────────────────────
static constexpr int TOP_H  = 28;
static constexpr int ROW_H  = 22;   // 9 × 22 = 198 px (28 + 198 + 14 = 240)
static constexpr int STAT_H = 14;
static constexpr int BAR_W  = 130;

// ── State ─────────────────────────────────────────────────────────────────────
static int   s_stepIdx             = 1;   // default ±200 kHz
static float s_stepMHz             = 0.200f;
static bool  s_hits[ScreenChanScan::MAX_CHAN][ROLL] = {};
static int   s_rollPos             = 0;
static int   s_sweeps              = 0;

// ── Static member defs ────────────────────────────────────────────────────────
lv_obj_t* ScreenChanScan::_screen               = nullptr;
lv_obj_t* ScreenChanScan::_regionLbl            = nullptr;
lv_obj_t* ScreenChanScan::_chanFreqLbl[MAX_CHAN] = {};
lv_obj_t* ScreenChanScan::_chanBar[MAX_CHAN]     = {};
lv_obj_t* ScreenChanScan::_chanPctLbl[MAX_CHAN]  = {};
lv_obj_t* ScreenChanScan::_chanStatLbl[MAX_CHAN] = {};
lv_obj_t* ScreenChanScan::_meshInfoLbl          = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────────

static float _meshFreq() { return ops::MeshService::instance().getFreqMHz(); }

static int _hitPct(int row)
{
    int n = (s_sweeps < ROLL) ? s_sweeps : ROLL;
    if (n == 0) return 0;
    int cnt = 0;
    for (int i = 0; i < n; i++) cnt += s_hits[row][i] ? 1 : 0;
    return cnt * 100 / n;
}

// ── _updateRows() ─────────────────────────────────────────────────────────────
void ScreenChanScan::_updateRows()
{
    if (!_screen) return;
    float meshFreq = _meshFreq();
    for (int i = 0; i < MAX_CHAN; i++) {
        float freq = meshFreq + (i - CENTER_ROW) * s_stepMHz;
        char freqBuf[12];
        if (i == CENTER_ROW) snprintf(freqBuf, sizeof(freqBuf), "%.3f*", (double)freq);
        else                  snprintf(freqBuf, sizeof(freqBuf), "%.3f",  (double)freq);
        lv_label_set_text(_chanFreqLbl[i], freqBuf);

        int pct = _hitPct(i);
        lv_bar_set_value(_chanBar[i], pct, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%3d%%", pct);
        lv_label_set_text(_chanPctLbl[i], buf);

        const char*  status;
        lv_color_t   sc;
        if      (pct >= 30) { status = "HIGH"; sc = lv_color_make(255, 100,  0); }
        else if (pct >= 10) { status = "med";  sc = lv_color_make(220, 200,  0); }
        else                { status = "low";  sc = lv_color_make( 70, 120, 70); }
        lv_label_set_text(_chanStatLbl[i], status);
        lv_obj_set_style_text_color(_chanStatLbl[i], sc, 0);
    }
}

// ── _switchStep() ─────────────────────────────────────────────────────────────
void ScreenChanScan::_switchStep(int dir)
{
    s_stepIdx = (s_stepIdx + STEP_COUNT + dir) % STEP_COUNT;
    s_stepMHz = kSteps[s_stepIdx];
    memset(s_hits, 0, sizeof(s_hits));
    s_rollPos = 0;
    s_sweeps  = 0;
    if (_regionLbl) lv_label_set_text(_regionLbl, kStepLabels[s_stepIdx]);
    _updateRows();
}

// ── navigate() ────────────────────────────────────────────────────────────────
void ScreenChanScan::navigate(int dx, int /*dy*/)
{
    if (dx != 0) _switchStep(dx > 0 ? 1 : -1);
}

// ── update() ──────────────────────────────────────────────────────────────────
void ScreenChanScan::update()
{
    if (!_screen || lv_scr_act() != _screen) return;

    float meshFreq = _meshFreq();
    float freqs[MAX_CHAN];
    for (int i = 0; i < MAX_CHAN; i++) freqs[i] = meshFreq + (i - CENTER_ROW) * s_stepMHz;

    bool results[MAX_CHAN] = {};
    if (!ops::MeshService::instance().cadSweepChannels(freqs, MAX_CHAN, results))
        return;

    for (int i = 0; i < MAX_CHAN; i++) s_hits[i][s_rollPos] = results[i];
    s_rollPos = (s_rollPos + 1) % ROLL;
    if (s_sweeps < ROLL) s_sweeps++;
    _updateRows();
}

// ── isActive() ────────────────────────────────────────────────────────────────
bool ScreenChanScan::isActive() { return _screen && lv_scr_act() == _screen; }

// ── callbacks ─────────────────────────────────────────────────────────────────
void ScreenChanScan::_onKey(lv_event_t* e)
{
    uint32_t* key = (uint32_t*)lv_event_get_param(e);
    if (key && *key == LV_KEY_ESC) ScreenLauncher::show();
}
void ScreenChanScan::_onHome(lv_event_t*) { ScreenLauncher::show(); }

// ── _buildScreen() ────────────────────────────────────────────────────────────
void ScreenChanScan::_buildScreen()
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
    lv_label_set_text(titleLbl, LV_SYMBOL_WIFI " Chan Scan");
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_flex_grow(titleLbl, 1);

    _regionLbl = lv_label_create(bar);
    lv_label_set_text(_regionLbl, kStepLabels[s_stepIdx]);
    lv_obj_set_style_text_color(_regionLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(_regionLbl, &lv_font_montserrat_10, 0);

    // ── Channel rows ──────────────────────────────────────────────────────────
    static constexpr int COL_FREQ = 4;
    static constexpr int COL_BAR  = 68;
    static constexpr int COL_PCT  = COL_BAR + BAR_W + 4;
    static constexpr int COL_STAT = COL_PCT + 34;

    float meshFreq = _meshFreq();
    for (int i = 0; i < MAX_CHAN; i++) {
        int y = TOP_H + i * ROW_H + 1;
        bool isCenter = (i == CENTER_ROW);

        _chanFreqLbl[i] = lv_label_create(_screen);
        lv_obj_set_style_text_font(_chanFreqLbl[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(_chanFreqLbl[i],
            isCenter ? theme::ACCENT : theme::TEXT, 0);
        lv_obj_set_pos(_chanFreqLbl[i], COL_FREQ, y + 5);
        {
            char buf[12];
            float freq = meshFreq + (i - CENTER_ROW) * s_stepMHz;
            if (isCenter) snprintf(buf, sizeof(buf), "%.3f*", (double)freq);
            else          snprintf(buf, sizeof(buf), "%.3f",  (double)freq);
            lv_label_set_text(_chanFreqLbl[i], buf);
        }

        _chanBar[i] = lv_bar_create(_screen);
        lv_obj_set_size(_chanBar[i], BAR_W, ROW_H - 6);
        lv_obj_set_pos(_chanBar[i], COL_BAR, y + 3);
        lv_bar_set_range(_chanBar[i], 0, 100);
        lv_bar_set_value(_chanBar[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(_chanBar[i], 3, 0);
        lv_obj_set_style_radius(_chanBar[i], 3, LV_PART_INDICATOR);
        lv_group_remove_obj(_chanBar[i]);
        if (isCenter) {
            lv_obj_set_style_bg_color(_chanBar[i], lv_color_make(0, 30, 50), 0);
            lv_obj_set_style_bg_color(_chanBar[i], theme::ACCENT, LV_PART_INDICATOR);
        } else {
            lv_obj_set_style_bg_color(_chanBar[i], lv_color_make(25, 40, 25), 0);
            lv_obj_set_style_bg_color(_chanBar[i], lv_color_make(40, 160, 40), LV_PART_INDICATOR);
        }

        _chanPctLbl[i] = lv_label_create(_screen);
        lv_obj_set_style_text_font(_chanPctLbl[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(_chanPctLbl[i], theme::TEXT_MUTED, 0);
        lv_obj_set_pos(_chanPctLbl[i], COL_PCT, y + 5);
        lv_label_set_text(_chanPctLbl[i], "  0%");

        _chanStatLbl[i] = lv_label_create(_screen);
        lv_obj_set_style_text_font(_chanStatLbl[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(_chanStatLbl[i], lv_color_make(70, 120, 70), 0);
        lv_obj_set_pos(_chanStatLbl[i], COL_STAT, y + 5);
        lv_label_set_text(_chanStatLbl[i], "low");
    }

    // ── Bottom status bar ─────────────────────────────────────────────────────
    _meshInfoLbl = lv_label_create(_screen);
    lv_obj_set_pos(_meshInfoLbl, 4, OPS_SCREEN_H - STAT_H + 1);
    lv_obj_set_style_text_color(_meshInfoLbl, lv_color_make(70, 70, 70), 0);
    lv_obj_set_style_text_font(_meshInfoLbl, &lv_font_montserrat_10, 0);
    lv_label_set_text(_meshInfoLbl, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " step   * = mesh ch   CAD SF8/BW62.5");

    // Focus object
    lv_obj_t* foc = lv_obj_create(_screen);
    lv_obj_set_size(foc, 1, 1);
    lv_obj_set_pos(foc, 0, 0);
    lv_obj_set_style_bg_opa(foc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(foc, 0, 0);
    lv_obj_add_event_cb(foc, _onKey, LV_EVENT_KEY, nullptr);
    lv_group_add_obj(lv_group_get_default(), foc);
    lv_group_focus_obj(foc);
}

// ── show() ────────────────────────────────────────────────────────────────────
void ScreenChanScan::show()
{
    if (!_screen) _buildScreen();
    if (!_screen) return;

    s_stepIdx = 1;
    s_stepMHz = kSteps[1];
    memset(s_hits, 0, sizeof(s_hits));
    s_rollPos = 0;
    s_sweeps  = 0;
    if (_regionLbl) lv_label_set_text(_regionLbl, kStepLabels[1]);
    _updateRows();
    lv_scr_load(_screen);
}

}}  // namespace ops::ui
