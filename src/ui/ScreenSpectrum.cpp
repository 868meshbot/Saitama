// Saitama — ScreenSpectrum.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Spectrum analyzer using the SX1262's GetRssiInst command.
// Layout (320 × 240):
//
//   ┌────────────────────────────────────────┐  y=0
//   │ [⌂]  Spectrum           869.6 ±16MHz   │  28px  top bar
//   ├───────────────────────────────┬────────┤  y=28
//   │                               │ ─50    │
//   │  waterfall  (50 rows × 2px)   │ ~~~~~  │  100px
//   │                               │ ─125   │
//   ├───────────────────────────────┼────────┤  y=128
//   │                               │ ─50    │
//   │  live trace + peak hold       │ ─90    │  80px
//   │                               │ ─125   │
//   ├───────────────────────────────┴────────┤  y=208
//   │  100kHz  29.6MHz  869.618MHz  gain:NORM│  32px  bottom bar
//   │  NF -118.5 dBm    CH -96.0 dBm         │        (2 lines)
//   └────────────────────────────────────────┘  y=240
//
//  Right 24 px of canvas = scale strip (colour ramp for WF; dBm ticks for trace)
//
//  dB scale: auto by default — the waterfall colours and the trace both span
//  from just under the noise floor to just over the strongest signal (40-90 dB,
//  5 dB steps), so the height isn't spent on -140..-120 where nothing is.
//  'a' switches to the fixed -140..-50 dBm scale and back.

#include "ScreenSpectrum.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../mesh/MeshService.h"
#include "../utils/Config.h"
#include "../utils/Log.h"
#include <lvgl.h>
#include <cstring>
#include <cmath>
#include <esp_heap_caps.h>

namespace ops { namespace ui {

// ── Layout ───────────────────────────────────────────────────────────────────
static constexpr int TOP_H    = 28;
static constexpr int CANVAS_W = 320;
static constexpr int SCALE_W  = 24;          // right-hand scale strip
static constexpr int SPEC_W   = CANVAS_W - SCALE_W;  // 296 — spectrum columns
static constexpr int WF_H     = 100;         // waterfall section height in canvas
static constexpr int WF_ROWS  = 50;          // rows in history (each 2 px tall)
static constexpr int TRACE_H  = 80;          // trace section height in canvas
static constexpr int CANVAS_H = WF_H + TRACE_H;  // 180
static constexpr int BOT_H    = 32;

// ── RSSI range ───────────────────────────────────────────────────────────────
// Fixed scale (also the limits of the auto scale).
static constexpr float RSSI_MIN = -140.0f;
static constexpr float RSSI_MAX =  -50.0f;
// Auto scale: span limits and how far the smoothed level must move past the
// shown edge before the edge steps (stops the labels flickering).
static constexpr float AUTO_MIN_SPAN = 40.0f;
static constexpr float AUTO_MAX_SPAN = 90.0f;
static constexpr float AUTO_HYST     = 7.5f;

// Scale in use — what the waterfall colours and trace bars map to.
static float s_lo = RSSI_MIN;
static float s_hi = RSSI_MAX;
static bool  s_autoScale = true;
static float s_noiseEma  = 0.0f;   // smoothed noise floor (20th percentile)
static float s_maxEma    = 0.0f;   // smoothed strongest bin
static bool  s_emaValid  = false;
static lv_obj_t* s_liveLbl = nullptr;   // bottom bar line 2: noise floor + channel

// ── Zoom levels ───────────────────────────────────────────────────────────────
struct ZoomLevel { float stepKHz; const char* label; };
static constexpr ZoomLevel kZooms[] = {
    { 25.0f,  "25kHz"  },   // span ≈  7.4 MHz
    { 100.0f, "100kHz" },   // span ≈ 29.6 MHz  (default)
    { 500.0f, "500kHz" },   // span ≈ 148 MHz
};
static constexpr int ZOOM_COUNT = 3;

// ── State ─────────────────────────────────────────────────────────────────────
// PSRAM-backed buffers — allocated on first show() to free ~17 KB of internal
// DRAM. s_wf is a flattened WF_ROWS×SPEC_W array; access with s_wf[row*SPEC_W+col].
static float*  s_rssi = nullptr;
static float*  s_peak = nullptr;
static int8_t* s_wf   = nullptr;
static int    s_wfRow      = 0;
static float  s_center     = 869.0f;
static int    s_zoom       = 1;
static bool   s_gainBoost  = false;
static bool   s_cadDetected = false;   // result of last hardware CAD check
static float  s_meshFreqMHz = 869.0f; // current mesh channel frequency

// ── Static members ────────────────────────────────────────────────────────────
lv_obj_t*   ScreenSpectrum::_screen    = nullptr;
lv_obj_t*   ScreenSpectrum::_canvas    = nullptr;
lv_color_t* ScreenSpectrum::_canvasBuf = nullptr;
lv_obj_t*   ScreenSpectrum::_infoLbl   = nullptr;
lv_obj_t*   ScreenSpectrum::_cadLbl    = nullptr;

// ── Color helpers ─────────────────────────────────────────────────────────────

// Heat map: bottom of the scale → black, top → yellow
static inline lv_color_t _rssiToWfColor(float rssi)
{
    float norm = (rssi - s_lo) / (s_hi - s_lo);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    uint16_t v = (uint16_t)(norm * 255.0f);

    uint8_t r, g, b;
    if (v < 64) {
        r = 0;  g = 0;  b = (uint8_t)(v * 4);
    } else if (v < 128) {
        r = 0;  g = (uint8_t)((v - 64) * 4);  b = 255;
    } else if (v < 192) {
        r = 0;  g = 255;  b = (uint8_t)(255 - (v - 128) * 4);
    } else {
        r = (uint8_t)((v - 192) * 4);  g = 255;  b = 0;
    }
    return lv_color_make(r, g, b);
}

// Pixels from bottom of trace area (0=floor, TRACE_H-1=ceiling)
static inline int _rssiToBar(float rssi)
{
    if (rssi < s_lo) rssi = s_lo;
    if (rssi > s_hi) rssi = s_hi;
    return (int)((rssi - s_lo) / (s_hi - s_lo) * (TRACE_H - 1));
}

// Tracks the noise floor and strongest bin of each sweep (shown live in the
// bottom bar), and in auto mode moves the scale towards them. Noise floor =
// 20th percentile of the bins (a few strong signals don't lift it); top =
// strongest bin. Both are smoothed, rises faster than falls; the scale edges
// snap to 5 dB.
static void _updateAutoScale()
{
    // Histogram in the radio's 0.5 dB steps, -160..-0.5 dBm — no sort, no
    // big buffer.
    static constexpr int H_MIN2 = -320, H_N = 320;   // half-dB units
    uint16_t hist[H_N] = {};
    float maxR = -200.0f;
    for (int i = 0; i < SPEC_W; i++) {
        float r = s_rssi[i];
        if (r > maxR) maxR = r;
        int b = (int)floorf(r * 2.0f) - H_MIN2;
        if (b < 0) b = 0;
        if (b >= H_N) b = H_N - 1;
        hist[b]++;
    }
    int want = SPEC_W / 5, acc = 0, nb = 0;
    for (; nb < H_N; nb++) { acc += hist[nb]; if (acc >= want) break; }
    float noise = (float)(nb + H_MIN2) * 0.5f;

    if (!s_emaValid) {
        s_noiseEma = noise;
        s_maxEma   = maxR;
        s_emaValid = true;
        s_lo = 1.0f;   // force the first snap below
    } else {
        s_noiseEma += (noise - s_noiseEma) * 0.15f;
        s_maxEma   += (maxR - s_maxEma) * (maxR > s_maxEma ? 0.5f : 0.05f);
    }

    if (!s_autoScale) { s_lo = RSSI_MIN; s_hi = RSSI_MAX; return; }

    float wantLo = floorf((s_noiseEma - 5.0f) / 5.0f) * 5.0f;
    float wantHi = ceilf((s_maxEma + 5.0f) / 5.0f) * 5.0f;
    if (wantLo < RSSI_MIN) wantLo = RSSI_MIN;
    if (wantHi > RSSI_MAX) wantHi = RSSI_MAX;
    if (wantHi - wantLo < AUTO_MIN_SPAN) wantHi = wantLo + AUTO_MIN_SPAN;
    if (wantHi > RSSI_MAX) { wantHi = RSSI_MAX; wantLo = RSSI_MAX - AUTO_MIN_SPAN; }
    if (wantHi - wantLo > AUTO_MAX_SPAN) wantLo = wantHi - AUTO_MAX_SPAN;

    // Hysteresis: only step an edge when the smoothed level is well past it.
    if (s_lo > 0.0f || fabsf((s_noiseEma - 5.0f) - s_lo) >= AUTO_HYST) s_lo = wantLo;
    if (s_lo > 0.0f || fabsf((s_maxEma + 5.0f) - s_hi) >= AUTO_HYST
                    || s_hi - s_lo < AUTO_MIN_SPAN) s_hi = wantHi;
    if (s_hi - s_lo < AUTO_MIN_SPAN) s_hi = s_lo + AUTO_MIN_SPAN;
    if (s_hi - s_lo > AUTO_MAX_SPAN) s_lo = s_hi - AUTO_MAX_SPAN;
}

// ── _redrawCanvas() ───────────────────────────────────────────────────────────
void ScreenSpectrum::_redrawCanvas()
{
    if (!_canvas || !_canvasBuf) return;

    static const lv_color_t kBg   = lv_color_make(0, 0, 0);
    static const lv_color_t kGrid = lv_color_make(28, 28, 28);
    static const lv_color_t kPeak = lv_color_make(255, 210, 0);
    static const lv_color_t kScaleBg = lv_color_make(10, 10, 10);
    static const lv_color_t kDiv    = lv_color_make(50, 50, 50);

    // Trace grid lines on round dBm values inside the current scale:
    // every 10 dB for spans up to 60 dB, else every 20.
    static constexpr int GRID_MAX = 5;
    float gridDbm[GRID_MAX];
    int   gridY[GRID_MAX];
    int   nGrid = 0;
    {
        float step = (s_hi - s_lo) <= 60.0f ? 10.0f : 20.0f;
        for (float d = ceilf((s_lo + 1.0f) / step) * step;
             d < s_hi && nGrid < GRID_MAX; d += step) {
            gridDbm[nGrid] = d;
            gridY[nGrid]   = WF_H + (TRACE_H - 1 - _rssiToBar(d));
            nGrid++;
        }
    }

    // ── Waterfall ─────────────────────────────────────────────────────────────
    for (int row = 0; row < WF_ROWS; row++) {
        int histIdx  = (s_wfRow + row) % WF_ROWS;
        int cy0      = row * 2;
        for (int x = 0; x < SPEC_W; x++) {
            lv_color_t c = _rssiToWfColor((float)s_wf[histIdx * SPEC_W + x]);
            _canvasBuf[ cy0      * CANVAS_W + x] = c;
            _canvasBuf[(cy0 + 1) * CANVAS_W + x] = c;
        }
        // Scale strip: vertical colour-ramp (maps row top → RSSI)
        float rowRssi = s_hi - (float)cy0 / (WF_H - 1) * (s_hi - s_lo);
        lv_color_t sc = _rssiToWfColor(rowRssi);
        for (int x = SPEC_W; x < CANVAS_W; x++) {
            _canvasBuf[ cy0      * CANVAS_W + x] = sc;
            _canvasBuf[(cy0 + 1) * CANVAS_W + x] = sc;
        }
    }
    // 1-px divider between spectrum and scale strip (waterfall rows)
    for (int cy = 0; cy < WF_H; cy++)
        _canvasBuf[cy * CANVAS_W + SPEC_W - 1] = kDiv;

    // ── Trace ─────────────────────────────────────────────────────────────────
    for (int x = 0; x < SPEC_W; x++) {
        int barH  = _rssiToBar(s_rssi[x]);
        int peakH = _rssiToBar(s_peak[x]);
        int barTop  = WF_H + (TRACE_H - 1 - barH);
        int peakTop = WF_H + (TRACE_H - 1 - peakH);

        for (int ty = WF_H; ty < CANVAS_H; ty++) {
            bool isGrid = false;
            for (int g = 0; g < nGrid; g++) if (ty == gridY[g]) isGrid = true;
            bool inBar  = (ty >= barTop);
            bool isPeak = (ty == peakTop);

            lv_color_t c;
            if (isPeak) {
                c = kPeak;
            } else if (inBar) {
                int relY   = ty - barTop;
                uint8_t br = (uint8_t)(210 - relY * 150 / TRACE_H);
                if (br < 40) br = 40;
                c = lv_color_make(0, br, 20);
            } else if (isGrid) {
                c = kGrid;
            } else {
                c = kBg;
            }
            _canvasBuf[ty * CANVAS_W + x] = c;
        }
    }
    // Scale strip: dark background in trace section + divider line
    for (int ty = WF_H; ty < CANVAS_H; ty++) {
        for (int x = SPEC_W; x < CANVAS_W; x++)
            _canvasBuf[ty * CANVAS_W + x] = kScaleBg;
    }
    for (int ty = WF_H; ty < CANVAS_H; ty++)
        _canvasBuf[ty * CANVAS_W + SPEC_W - 1] = kDiv;

    // ── Scale labels (drawn on top of canvas buffer) ───────────────────────────
    lv_draw_label_dsc_t ldsc;
    lv_draw_label_dsc_init(&ldsc);
    ldsc.font  = &lv_font_montserrat_10;

    char hiTxt[8], loTxt[8];
    snprintf(hiTxt, sizeof(hiTxt), "%d", (int)s_hi);
    snprintf(loTxt, sizeof(loTxt), "%d", (int)s_lo);

    // Waterfall labels: top and bottom of the colour scale
    ldsc.color = lv_color_make(220, 220, 220);
    lv_canvas_draw_text(_canvas, SPEC_W + 1, 2,            SCALE_W - 2, &ldsc, hiTxt);
    lv_canvas_draw_text(_canvas, SPEC_W + 1, WF_H - 13,   SCALE_W - 2, &ldsc, loTxt);

    // Trace dBm ticks: top, the grid lines (unless crowding an end label), bottom
    ldsc.color = lv_color_make(140, 140, 140);
    int traceTop    = WF_H;            // canvas Y for s_hi
    int traceBottom = CANVAS_H - 1;    // canvas Y for s_lo
    lv_canvas_draw_text(_canvas, SPEC_W + 1, traceTop,         SCALE_W - 2, &ldsc, hiTxt);
    for (int g = 0; g < nGrid; g++) {
        if (gridY[g] - 5 < traceTop + 12 || gridY[g] - 5 > traceBottom - 24) continue;
        char t[8];
        snprintf(t, sizeof(t), "%d", (int)gridDbm[g]);
        lv_canvas_draw_text(_canvas, SPEC_W + 1, gridY[g] - 5, SCALE_W - 2, &ldsc, t);
    }
    lv_canvas_draw_text(_canvas, SPEC_W + 1, traceBottom - 12, SCALE_W - 2, &ldsc, loTxt);

    // ── Mesh channel vertical marker (drawn last so it sits on top) ───────────
    {
        float stepMHz  = kZooms[s_zoom].stepKHz / 1000.0f;
        float viewStart = s_center - (SPEC_W / 2.0f) * stepMHz;
        int meshCol = (int)((s_meshFreqMHz - viewStart) / stepMHz + 0.5f);
        if (meshCol >= 1 && meshCol < SPEC_W - 1) {
            // CAD active → bright orange; quiet → dim green
            lv_color_t mc = s_cadDetected
                ? lv_color_make(255, 100,  0)
                : lv_color_make(  0, 100,  0);
            for (int cy = 0; cy < CANVAS_H; cy++)
                _canvasBuf[cy * CANVAS_W + meshCol] = mc;
        }
    }

    lv_obj_invalidate(_canvas);
}

// ── _updateInfo() ─────────────────────────────────────────────────────────────
void ScreenSpectrum::_updateInfo()
{
    if (!_infoLbl) return;
    float stepKHz = kZooms[s_zoom].stepKHz;
    float spanMHz = (float)SPEC_W * stepKHz / 1000.0f;
    char buf[80];
    snprintf(buf, sizeof(buf), "%s  %.1fMHz  %.3fMHz  gain:%s  dB:%s",
             kZooms[s_zoom].label,
             (double)spanMHz,
             (double)s_center,
             s_gainBoost ? "+3dB" : "NORM",
             s_autoScale ? "AUTO" : "FIX");
    lv_label_set_text(_infoLbl, buf);
}

// ── navigate() ────────────────────────────────────────────────────────────────
void ScreenSpectrum::navigate(int dx, int dy)
{
    if (dy != 0) {
        int nz = s_zoom - (dy > 0 ? 1 : -1);
        s_zoom = nz < 0 ? 0 : nz >= ZOOM_COUNT ? ZOOM_COUNT - 1 : nz;
        for (int i = 0; i < SPEC_W; i++) s_peak[i] = RSSI_MIN;
        _updateInfo();
    }
    if (dx != 0) {
        float stepMHz = kZooms[s_zoom].stepKHz / 1000.0f;
        float span    = (float)SPEC_W * stepMHz;
        s_center += (float)(dx > 0 ? 1 : -1) * span * 0.25f;
        if (s_center < 150.0f) s_center = 150.0f;
        if (s_center > 960.0f) s_center = 960.0f;
        _updateInfo();
    }
}

// ── update() ──────────────────────────────────────────────────────────────────
void ScreenSpectrum::update()
{
    if (!_screen || lv_scr_act() != _screen) return;

    float stepMHz  = kZooms[s_zoom].stepKHz / 1000.0f;
    float startMHz = s_center - (SPEC_W / 2) * stepMHz;

    ops::MeshService::instance().spectrumScan(startMHz, stepMHz, SPEC_W, s_rssi);

    // Hardware CAD at the mesh channel — adds ~8 ms, identifies LoRa vs noise
    s_meshFreqMHz = ops::MeshService::instance().getFreqMHz();
    s_cadDetected = ops::MeshService::instance().cadCheck();
    if (_cadLbl) {
        lv_obj_set_style_text_color(_cadLbl,
            s_cadDetected ? lv_color_make(255, 140, 0) : lv_color_make(60, 110, 60), 0);
        lv_label_set_text(_cadLbl, s_cadDetected ? "ACTIVE" : "QUIET");
    }

    for (int i = 0; i < SPEC_W; i++)
        if (s_rssi[i] > s_peak[i]) s_peak[i] = s_rssi[i];

    for (int x = 0; x < SPEC_W; x++) {
        float r = s_rssi[x];
        if (r < -128.0f) r = -128.0f;
        if (r >   -1.0f) r =   -1.0f;
        s_wf[s_wfRow * SPEC_W + x] = (int8_t)r;
    }
    s_wfRow = (s_wfRow + 1) % WF_ROWS;

    _updateAutoScale();
    _redrawCanvas();

    // Live readout: smoothed noise floor (drops 1-3 dB with 'g' boost) and
    // the strongest of the three bins around the mesh channel.
    if (s_liveLbl) {
        char ch[12] = "--";
        int meshCol = (int)((s_meshFreqMHz - startMHz) / stepMHz + 0.5f);
        if (meshCol >= 0 && meshCol < SPEC_W) {
            float m = s_rssi[meshCol];
            if (meshCol > 0          && s_rssi[meshCol - 1] > m) m = s_rssi[meshCol - 1];
            if (meshCol < SPEC_W - 1 && s_rssi[meshCol + 1] > m) m = s_rssi[meshCol + 1];
            snprintf(ch, sizeof(ch), "%.1f", (double)m);
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "NF %.1f dBm    CH %s dBm",
                 (double)s_noiseEma, ch);
        lv_label_set_text(s_liveLbl, buf);
    }
}

// ── isActive() ────────────────────────────────────────────────────────────────
bool ScreenSpectrum::isActive()
{
    return _screen && (lv_scr_act() == _screen);
}

// ── _onKey() ──────────────────────────────────────────────────────────────────
void ScreenSpectrum::_onKey(lv_event_t* e)
{
    uint32_t* key = (uint32_t*)lv_event_get_param(e);
    if (!key) return;

    if (*key == LV_KEY_ESC || *key == 'q' || *key == 'Q') {
        // Restore configured RX boost before exiting
        ops::MeshService::instance().setRxBoost(ops::config::get().rxBoost);
        ScreenLauncher::show();
        return;
    }

    if (*key == 'g' || *key == 'G') {
        s_gainBoost = !s_gainBoost;
        ops::MeshService::instance().setRxBoost(s_gainBoost);
        // No scale re-fit here: the floor only moves 1-3 dB, and snapping
        // the scale to it would hide the very change being toggled.
        _updateInfo();
    }

    if (*key == 'a' || *key == 'A') {
        s_autoScale = !s_autoScale;
        s_emaValid  = false;
        _updateAutoScale();
        _redrawCanvas();
        _updateInfo();
    }
}

void ScreenSpectrum::_onHome(lv_event_t* e)
{
    ops::MeshService::instance().setRxBoost(ops::config::get().rxBoost);
    ScreenLauncher::show();
}

// ── _buildScreen() ────────────────────────────────────────────────────────────
void ScreenSpectrum::_buildScreen()
{
    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar (matches MP3/Terminal chrome) ─────────────────────────────────
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

    // Title grows to fill space, pushing CAD label to the right
    lv_obj_t* titleLbl = lv_label_create(bar);
    lv_label_set_text(titleLbl, LV_SYMBOL_WIFI " Spectrum");
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_flex_grow(titleLbl, 1);

    // CAD status indicator — sits at far right of flex row
    _cadLbl = lv_label_create(bar);
    lv_label_set_text(_cadLbl, "");
    lv_obj_set_style_text_color(_cadLbl, lv_color_make(60, 110, 60), 0);
    lv_obj_set_style_text_font(_cadLbl, &lv_font_montserrat_10, 0);

    // ── Canvas ────────────────────────────────────────────────────────────────
    _canvasBuf = (lv_color_t*)ps_malloc((size_t)CANVAS_W * CANVAS_H * sizeof(lv_color_t));
    if (!_canvasBuf) { OPS_LOG("Spectrum", "ps_malloc canvas failed"); return; }
    memset(_canvasBuf, 0, (size_t)CANVAS_W * CANVAS_H * sizeof(lv_color_t));

    _canvas = lv_canvas_create(_screen);
    lv_canvas_set_buffer(_canvas, _canvasBuf, CANVAS_W, CANVAS_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(_canvas, 0, TOP_H);
    lv_group_remove_obj(_canvas);

    lv_obj_add_event_cb(_canvas, _onKey, LV_EVENT_KEY, nullptr);
    lv_group_add_obj(lv_group_get_default(), _canvas);
    lv_group_focus_obj(_canvas);

    // ── Bottom info bar ───────────────────────────────────────────────────────
    lv_obj_t* bot = lv_obj_create(_screen);
    lv_obj_set_size(bot, OPS_SCREEN_W, BOT_H);
    lv_obj_set_pos(bot, 0, OPS_SCREEN_H - BOT_H);
    lv_obj_set_style_bg_color(bot, theme::PRIMARY, 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_radius(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 2, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);

    _infoLbl = lv_label_create(bot);
    lv_obj_set_style_text_color(_infoLbl, lv_color_make(160, 160, 160), 0);
    lv_obj_set_style_text_font(_infoLbl, &lv_font_montserrat_10, 0);
    lv_obj_align(_infoLbl, LV_ALIGN_TOP_LEFT, 2, 0);

    s_liveLbl = lv_label_create(bot);
    lv_label_set_text(s_liveLbl, "NF --    CH --");
    lv_obj_set_style_text_color(s_liveLbl, lv_color_make(120, 200, 230), 0);
    lv_obj_set_style_text_font(s_liveLbl, &lv_font_montserrat_10, 0);
    lv_obj_align(s_liveLbl, LV_ALIGN_BOTTOM_LEFT, 2, 0);
}

// ── show() ────────────────────────────────────────────────────────────────────
void ScreenSpectrum::show()
{
    if (!s_rssi) {
        s_rssi = (float*)ps_malloc(SPEC_W * sizeof(float));
        s_peak = (float*)ps_malloc(SPEC_W * sizeof(float));
        s_wf   = (int8_t*)ps_malloc((size_t)WF_ROWS * SPEC_W * sizeof(int8_t));
        if (!s_rssi || !s_peak || !s_wf) { OPS_LOG("Spectrum", "ps_malloc failed"); return; }
    }
    if (!_screen) _buildScreen();
    if (!_screen || !_canvas) { OPS_LOG("Spectrum", "build failed"); return; }

    s_center      = ops::MeshService::instance().getFreqMHz();
    s_meshFreqMHz = s_center;
    s_zoom        = 1;
    s_gainBoost   = false;
    s_cadDetected = false;
    s_emaValid    = false;   // re-fit the auto scale from the first sweep
    ops::MeshService::instance().setRxBoost(false);  // start with normal gain

    if (_cadLbl) {
        lv_obj_set_style_text_color(_cadLbl, lv_color_make(60, 110, 60), 0);
        lv_label_set_text(_cadLbl, "");
    }

    for (int i = 0; i < SPEC_W; i++) { s_rssi[i] = RSSI_MIN; s_peak[i] = RSSI_MIN; }
    memset(s_wf, -128, (size_t)WF_ROWS * SPEC_W * sizeof(int8_t));
    s_wfRow = 0;

    _updateInfo();
    lv_scr_load(_screen);
}

}}  // namespace ops::ui
