// Saitama — ScreenPower.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenPower.h"
#include "../hardware/Board.h"
#include "../mesh/MeshService.h"
#include "../utils/Config.h"
#include "../utils/GpsMgr.h"
#include "../utils/Log.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <lvgl.h>

namespace ops {
namespace ui {

// ── timescale lookup tables ──────────────────────────────────────────
static constexpr int TS_COUNT = 4;
static constexpr int  TS_SAMPLE_MS[TS_COUNT] = { 5000, 10000, 30000, 60000 };
static constexpr const char* TS_LABEL[TS_COUNT] = { "5 MIN", "10 MIN", "30 MIN", "1 HR" };

// ── statics ─────────────────────────────────────────────────────────
lv_obj_t *ScreenPower::_screen      = nullptr;
lv_obj_t *ScreenPower::_body        = nullptr;
lv_obj_t *ScreenPower::_chart       = nullptr;
lv_chart_series_t *ScreenPower::_battSer = nullptr;
lv_obj_t *ScreenPower::_chartHdrLbl = nullptr;
lv_obj_t *ScreenPower::_loraLbl     = nullptr;
lv_obj_t *ScreenPower::_bleLbl      = nullptr;
lv_obj_t *ScreenPower::_displayLbl  = nullptr;
lv_obj_t *ScreenPower::_gpsLbl      = nullptr;
lv_obj_t *ScreenPower::_cpuLbl      = nullptr;
lv_obj_t *ScreenPower::_timeLbl     = nullptr;
lv_obj_t *ScreenPower::_totalLbl    = nullptr;
uint32_t  ScreenPower::_lastSampleMs = 0;
uint32_t  ScreenPower::_prevTxMs    = 0;
uint32_t  ScreenPower::_prevRxMs    = 0;
int       ScreenPower::_tsIdx       = 0;

// ── helpers ─────────────────────────────────────────────────────────
static lv_obj_t *_statLbl(lv_obj_t *parent, int x, int y, int w) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_width(l, w);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(l, theme::TEXT, 0);
  lv_label_set_text(l, "--");
  return l;
}

static lv_obj_t *_axisLbl(lv_obj_t *parent, int y, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_pos(l, 0, y);
  lv_obj_set_width(l, 20);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(l, theme::TEXT_MUTED, 0);
  lv_label_set_text(l, txt);
  return l;
}

// ── callbacks ────────────────────────────────────────────────────────
void ScreenPower::_onHome(lv_event_t * /*e*/) { ScreenLauncher::show(); }

void ScreenPower::_onKey(lv_event_t* e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
        ScreenLauncher::show();
    } else if (key == (uint32_t)'t') {
        _cycleTimescale();
    }
}

// ── _cycleTimescale() ────────────────────────────────────────────────
void ScreenPower::_cycleTimescale() {
  _tsIdx = (_tsIdx + 1) % TS_COUNT;

  if (_chartHdrLbl) {
    char buf[40];
    snprintf(buf, sizeof(buf), "BATTERY %%  -  LAST %s", TS_LABEL[_tsIdx]);
    lv_label_set_text(_chartHdrLbl, buf);
  }

  // Refill the circular buffer so the new timescale starts with a flat line
  // at the current battery level rather than stale data from the old scale.
  if (_chart && _battSer) {
    int batt = Board::instance().batteryPercent();
    for (int i = 0; i < NUM_PTS; i++)
      lv_chart_set_next_value(_chart, _battSer, (lv_coord_t)batt);
    lv_chart_refresh(_chart);
  }

  _lastSampleMs = 0;  // force immediate sample on next tick
}

// ── _build() ─────────────────────────────────────────────────────────
void ScreenPower::_build() {
  static constexpr int TOP_H  = 28;
  static constexpr int PAD    = 4;
  static constexpr int CHART_H = 108;
  // Y-axis label column is 22 px wide; chart is offset by that amount.
  static constexpr int AXIS_W  = 22;
  static constexpr int CHART_W = OPS_SCREEN_W - PAD * 2 - AXIS_W; // 290 px
  static constexpr int CHART_X = AXIS_W;
  // Two equal columns inside body content area
  static constexpr int COL_W  = 152;
  static constexpr int COL2_X = 160;
  // Stats section starts below chart header (14 px) + chart (108 px) + gap (4 px)
  static constexpr int SY = 126;
  static constexpr int RH = 14;  // row height for stat rows

  // Chart data area starts CHART_H * (1 - frac) px from chart top.
  // With PAD_VER=2 and 1px border the data area ~= CHART_H - 6 px.
  // Place labels so they visually align with the 4 guide lines (25/50/75/100%).
  static constexpr int CHART_TOP = 14;  // y of chart in body
  static constexpr int DA_TOP    = CHART_TOP + 3;  // data area top (border+pad)
  static constexpr int DA_H      = CHART_H - 6;    // data area height ≈ 102 px

  // ── screen ───────────────────────────────────────────────────────
  _screen = lv_obj_create(nullptr);
  lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
  lv_obj_set_style_bg_color(_screen, theme::BG, 0);
  lv_obj_set_style_pad_all(_screen, 0, 0);
  lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

  // ── top bar ──────────────────────────────────────────────────────
  lv_obj_t *bar = lv_obj_create(_screen);
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

  lv_obj_t *homeBtn = lv_btn_create(bar);
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
  lv_obj_t *homeLbl = lv_label_create(homeBtn);
  lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
  lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
  lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
  lv_obj_center(homeLbl);

  lv_obj_t *titleLbl = lv_label_create(bar);
  lv_label_set_text(titleLbl, "Power Monitor");
  lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
  lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

  // ── body (focusable so 't'/backspace → key events work) ─────────
  _body = lv_obj_create(_screen);
  lv_obj_set_size(_body, OPS_SCREEN_W, OPS_SCREEN_H - TOP_H);
  lv_obj_set_pos(_body, 0, TOP_H);
  lv_obj_set_style_bg_color(_body, theme::BG, 0);
  lv_obj_set_style_border_width(_body, 0, 0);
  lv_obj_set_style_pad_all(_body, PAD, 0);
  lv_obj_clear_flag(_body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(_body, _onKey, LV_EVENT_KEY, nullptr);
  lv_group_add_obj(lv_group_get_default(), _body);
  lv_group_focus_obj(_body);

  // ── chart header ─────────────────────────────────────────────────
  _chartHdrLbl = lv_label_create(_body);
  {
    char hdrBuf[40];
    snprintf(hdrBuf, sizeof(hdrBuf), "BATTERY %%  -  LAST %s", TS_LABEL[_tsIdx]);
    lv_label_set_text(_chartHdrLbl, hdrBuf);
  }
  lv_obj_set_style_text_font(_chartHdrLbl, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(_chartHdrLbl, theme::TEXT_MUTED, 0);
  lv_obj_set_pos(_chartHdrLbl, AXIS_W, 0);

  // ── Y-axis scale labels ──────────────────────────────────────────
  // Positions chosen to align with the 4 horizontal guide lines.
  _axisLbl(_body, DA_TOP - 3,                   "100");
  _axisLbl(_body, DA_TOP + (int)(DA_H * 0.25f) - 5, "75");
  _axisLbl(_body, DA_TOP + (int)(DA_H * 0.50f) - 5, "50");
  _axisLbl(_body, DA_TOP + (int)(DA_H * 0.75f) - 5, "25");
  _axisLbl(_body, DA_TOP + DA_H - 10,            "0");

  // ── battery % line chart ─────────────────────────────────────────
  _chart = lv_chart_create(_body);
  lv_obj_set_size(_chart, CHART_W, CHART_H);
  lv_obj_set_pos(_chart, CHART_X, CHART_TOP);
  lv_obj_set_style_bg_color(_chart, theme::BG_CARD, 0);
  lv_obj_set_style_bg_color(_chart, theme::BG_CARD, LV_PART_MAIN);
  lv_obj_set_style_border_color(_chart, theme::BORDER, 0);
  lv_obj_set_style_border_width(_chart, 1, 0);
  lv_obj_set_style_pad_hor(_chart, 2, 0);
  lv_obj_set_style_pad_ver(_chart, 2, 0);
  lv_obj_set_style_line_color(_chart, theme::BORDER, LV_PART_MAIN);
  lv_chart_set_type(_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(_chart, NUM_PTS);
  lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_div_line_count(_chart, 4, 0);
  lv_obj_set_style_size(_chart, 0, 0, LV_PART_INDICATOR);  // no point dots
  lv_obj_set_style_line_width(_chart, 2, LV_PART_ITEMS);

  _battSer = lv_chart_add_series(_chart, theme::ACCENT, LV_CHART_AXIS_PRIMARY_Y);

  // Pre-fill with current battery level so line isn't empty on open
  int initBatt = Board::instance().batteryPercent();
  for (int i = 0; i < NUM_PTS; i++)
    lv_chart_set_next_value(_chart, _battSer, (lv_coord_t)initBatt);

  // ── stats section ─────────────────────────────────────────────────
  lv_obj_t *statsHdr = lv_label_create(_body);
  lv_label_set_text(statsHdr, "EST. CURRENT DRAW  [t]=timescale");
  lv_obj_set_style_text_font(statsHdr, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(statsHdr, theme::TEXT_MUTED, 0);
  lv_obj_set_pos(statsHdr, 0, SY);

  // Row 1: LoRa | BLE
  _loraLbl    = _statLbl(_body, 0,      SY + RH,     COL_W);
  _bleLbl     = _statLbl(_body, COL2_X, SY + RH,     COL_W);
  // Row 2: Display | GPS
  _displayLbl = _statLbl(_body, 0,      SY + RH * 2, COL_W);
  _gpsLbl     = _statLbl(_body, COL2_X, SY + RH * 2, COL_W);
  // Row 3: CPU | Est. time remaining
  _cpuLbl     = _statLbl(_body, 0,      SY + RH * 3, COL_W);
  _timeLbl    = _statLbl(_body, COL2_X, SY + RH * 3, COL_W);
  // Row 4: Total (full width, accented)
  _totalLbl   = _statLbl(_body, 0,      SY + RH * 4, OPS_SCREEN_W - PAD * 2);
  lv_obj_set_style_text_color(_totalLbl, theme::ACCENT, 0);

  _sample();
}

// ── _sample() ────────────────────────────────────────────────────────
void ScreenPower::_sample() {
  auto &board = Board::instance();
  const auto &cfg = ops::config::get();
  auto stats = ops::MeshService::instance().radioStats();
  char buf[52];

  // ── Battery → chart ──────────────────────────────────────────────
  int batt = board.batteryPercent();
  if (_chart && _battSer)
    lv_chart_set_next_value(_chart, _battSer, (lv_coord_t)batt);

  // ── LoRa ─────────────────────────────────────────────────────────
  uint32_t txMs = (uint32_t)stats.airtimeTxMs;
  uint32_t rxMs = (uint32_t)stats.airtimeRxMs;
  uint32_t dTx  = (txMs >= _prevTxMs) ? txMs - _prevTxMs : 0;
  uint32_t dRx  = (rxMs >= _prevRxMs) ? rxMs - _prevRxMs : 0;
  _prevTxMs = txMs;
  _prevRxMs = rxMs;

  uint32_t sampleWindow = (uint32_t)TS_SAMPLE_MS[_tsIdx];
  float txF = (dTx < sampleWindow) ? (float)dTx / (float)sampleWindow : 1.0f;
  float rxF = (dRx < sampleWindow) ? (float)dRx / (float)sampleWindow : 1.0f;
  if (txF + rxF > 1.0f) rxF = 1.0f - txF;

  // SX1262 current estimates:
  //   TX: 30 mA @ 10 dBm → ~112 mA @ 22 dBm
  //   RX power-saving gain: ~4.6 mA  |  RX boosted gain: ~5.3 mA (+0.7 mA)
  //   Duty cycle idle: ~2.3 mA avg (50% duty)  |  Continuous standby: ~4.6 mA
  int8_t txdBm  = (cfg.radioTX > 0) ? cfg.radioTX : 17;
  float  txMA   = 30.0f + (float)(txdBm - 10) * 7.5f;
  float  rxMA   = cfg.rxBoost ? 5.3f : 4.6f;
  float  idleMA = stats.loraDutyCycleActive ? 2.3f : rxMA;
  float  loraMA = txF * txMA + rxF * rxMA + (1.0f - txF - rxF) * idleMA;

  if (_loraLbl) {
    int txPct = (int)(txF * 100.0f + 0.5f);
    // Build a compact flags string: "DC" and/or "RXB"
    char flags[8] = "";
    if (stats.loraDutyCycleActive && cfg.rxBoost) snprintf(flags, sizeof(flags), " DC+RXB");
    else if (stats.loraDutyCycleActive)            snprintf(flags, sizeof(flags), " DC");
    else if (cfg.rxBoost)                          snprintf(flags, sizeof(flags), " RXB");
    snprintf(buf, sizeof(buf), "LoRa%s: %.1fmA Tx%d%%", flags, loraMA, txPct);
    lv_label_set_text(_loraLbl, buf);
  }

  // ── BLE ──────────────────────────────────────────────────────────
  float bleMA = cfg.bluetoothEnabled ? 12.0f : 0.0f;
  if (_bleLbl) {
    snprintf(buf, sizeof(buf), "BLE: %.0fmA", bleMA);
    lv_label_set_text(_bleLbl, buf);
  }

  // ── Display backlight ────────────────────────────────────────────
  // TFT backlight: ~30 mA at full brightness, proportional to ledc duty
  float dispMA = (float)cfg.brightness * 30.0f / 255.0f;
  if (_displayLbl) {
    int pct = cfg.brightness * 100 / 255;
    snprintf(buf, sizeof(buf), "Display: %.0fmA %d%%", dispMA, pct);
    lv_label_set_text(_displayLbl, buf);
  }

  // ── GPS ──────────────────────────────────────────────────────────
  // L76K: ~20 mA active/searching, <0.001 mA in PMTK standby
  float gpsMA = (float)ops::GpsMgr::instance().estimatedCurrentMA();
  if (_gpsLbl) {
    static const char *gmName[] = {"off", "inter", "on"};
    const char *st = gmName[cfg.gpsMode < 3 ? cfg.gpsMode : 2];
    snprintf(buf, sizeof(buf), "GPS: %.0fmA (%s)", gpsMA, st);
    lv_label_set_text(_gpsLbl, buf);
  }

  // ── CPU (ESP32-S3, scales with governor) ─────────────────────────
  // 40 MHz ≈ 15 mA, 80 MHz ≈ 25 mA, 240 MHz ≈ 50 mA (measured estimates)
  static const uint32_t kGovFreqActive[4] = { 40, 80, 240, 240 };
  static const char*    kGovNames[4]      = { "PowerSave", "Medium", "Normal", "Turbo" };
  uint8_t gov = cfg.cpuGovernor < 4 ? cfg.cpuGovernor : 2;
  uint32_t cpuMHz = kGovFreqActive[gov];
  float CPU_MA = (cpuMHz <= 40) ? 15.0f : (cpuMHz <= 80) ? 25.0f : 50.0f;
  if (_cpuLbl) {
    snprintf(buf, sizeof(buf), "CPU: %.0fmA %uMHz (%s)", CPU_MA, cpuMHz, kGovNames[gov]);
    lv_label_set_text(_cpuLbl, buf);
  }

  // ── Total ────────────────────────────────────────────────────────
  float totalMA = loraMA + bleMA + dispMA + gpsMA + CPU_MA;
  if (_totalLbl) {
    snprintf(buf, sizeof(buf), "Total est: %.0f mA  (~%.1f mAh/hr)", totalMA, totalMA);
    lv_label_set_text(_totalLbl, buf);
  }

  // ── Estimated time remaining (2000 mAh battery) ─────────────────
  if (_timeLbl) {
    if (totalMA < 1.0f) {
      lv_label_set_text(_timeLbl, "Time: --");
    } else {
      float remaining_mAh = (float)batt * 20.0f;  // 2000 mAh * pct/100
      int totalMins = (int)(remaining_mAh / totalMA * 60.0f);
      int hrs  = totalMins / 60;
      int mins = totalMins % 60;
      if (hrs > 0)
        snprintf(buf, sizeof(buf), "~%dh %dm left", hrs, mins);
      else
        snprintf(buf, sizeof(buf), "~%dm left", mins);
      lv_label_set_text(_timeLbl, buf);
    }
  }

  _lastSampleMs = millis();
}

// ── public API ───────────────────────────────────────────────────────
bool ScreenPower::isActive() { return _screen && lv_scr_act() == _screen; }

void ScreenPower::show() {
  if (!_screen)
    _build();
  lv_scr_load(_screen);
  if (_body)
    lv_group_focus_obj(_body);
  _lastSampleMs = 0;  // force immediate sample on next tick
}

void ScreenPower::tick() {
  if (!isActive()) return;
  if (millis() - _lastSampleMs >= (uint32_t)TS_SAMPLE_MS[_tsIdx])
    _sample();
}

} // namespace ui
} // namespace ops
