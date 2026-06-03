// Saitama — ScreenSignal.cpp
// Copyright 2026 Saitama — MIT License
//
// Screen layout (320 × 240):
//
//   ┌──────────────────────────────────────┐  y = 0
//   │ [⌂]        Signal           12:34   │  top bar   28 px
//   ├──────────────────────────────────────┤  y = 28
//   │  SIGNAL QUALITY  ───────────────     │  section   20 px
//   │  RSSI:   -89 dBm                    │  row       18 px  ← colour-coded
//   │  SNR:    +5.2 dB                    │  row       18 px
//   │  Noise:  -110 dBm                   │  row       18 px
//   │  PACKETS ───────────────────────     │  section   20 px
//   │  TX:     42                         │  row       18 px
//   │  RX:     17                         │  row       18 px
//   │  Flood TX:  38                      │  row       18 px
//   │  Flood RX:  15                      │  row       18 px
//   │  Direct TX: 4                       │  row       18 px
//   │  Direct RX: 2                       │  row       18 px
//   │  Errors: 0                          │  row       18 px
//   │  AIRTIME ───────────────────────     │  section   20 px
//   │  TX:     2m34s                      │  row       18 px
//   │  RX:     8m12s                      │  row       18 px
//   │  RADIO ─────────────────────────     │  section   20 px
//   │  Freq:   869.618 MHz                │  row       18 px
//   │  Profile: NAR                       │  row       18 px
//   │  HARDWARE ──────────────────────     │  section   20 px
//   │  Board:  LilyGo T-Deck Plus         │  row       18 px
//   │  FW:     v0.1.0-alpha.1             │  row       18 px
//   │  Heap:   184 KB free                │  row       18 px
//   │  PSRAM:  7.9 MB free                │  row       18 px
//   └──────────────────────────────────────┘
//
//   Total content ≈ 404 px → scrolls ~192 px inside 212 px body.

#include "ScreenSignal.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../mesh/MeshService.h"
#include "../utils/Config.h"
#include "../hardware/Board.h"
#include "../version.h"
#include "../utils/Log.h"

#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include <time.h>
#include <esp_heap_caps.h>

namespace ops { namespace ui {

// ── Constants ─────────────────────────────────────────────────────────
static constexpr int  TOP_H   = 28;
static constexpr int  BODY_H  = OPS_SCREEN_H - TOP_H;   // 212 px
static constexpr int  SEC_H   = 20;    // section header height
static constexpr int  ROW_H   = 18;    // data row height
static constexpr int  KEY_W   = 92;    // key column width (px)

// ── Static state ─────────────────────────────────────────────────────
lv_obj_t*   ScreenSignal::_screen       = nullptr;
lv_obj_t*   ScreenSignal::_body         = nullptr;
lv_timer_t* ScreenSignal::_timer        = nullptr;

lv_obj_t* ScreenSignal::s_rssiLbl      = nullptr;
lv_obj_t* ScreenSignal::s_snrLbl       = nullptr;
lv_obj_t* ScreenSignal::s_noiseLbl     = nullptr;
lv_obj_t* ScreenSignal::s_txLbl        = nullptr;
lv_obj_t* ScreenSignal::s_rxLbl        = nullptr;
lv_obj_t* ScreenSignal::s_floodTxLbl   = nullptr;
lv_obj_t* ScreenSignal::s_floodRxLbl   = nullptr;
lv_obj_t* ScreenSignal::s_directTxLbl  = nullptr;
lv_obj_t* ScreenSignal::s_directRxLbl  = nullptr;
lv_obj_t* ScreenSignal::s_errorLbl     = nullptr;
lv_obj_t* ScreenSignal::s_airtimeTxLbl = nullptr;
lv_obj_t* ScreenSignal::s_airtimeRxLbl = nullptr;
lv_obj_t* ScreenSignal::s_freqLbl      = nullptr;
lv_obj_t* ScreenSignal::s_profileLbl  = nullptr;
lv_obj_t* ScreenSignal::s_heapLbl      = nullptr;
lv_obj_t* ScreenSignal::s_psramLbl    = nullptr;
lv_obj_t* ScreenSignal::s_battLbl     = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────

static void fmtAirtime(uint32_t ms, char* buf, size_t len)
{
    uint32_t s = ms / 1000;
    uint32_t m = s / 60;  s %= 60;
    uint32_t h = m / 60;  m %= 60;
    if (h > 0)       snprintf(buf, len, "%uh%02um", h, m);
    else if (m > 0)  snprintf(buf, len, "%um%02us", m, s);
    else             snprintf(buf, len, "%us", s);
}

static lv_color_t rssiColor(float rssi)
{
    if (rssi == 0.0f)        return theme::TEXT_MUTED;
    if (rssi > -85.0f)       return theme::GREEN;
    if (rssi > -100.0f)      return theme::ORANGE;
    return theme::RED;
}

static lv_color_t snrColor(float snr)
{
    if (snr == 0.0f)   return theme::TEXT_MUTED;
    if (snr > 10.0f)   return theme::GREEN;
    if (snr > 5.0f)    return theme::ORANGE;
    return theme::RED;
}

// ── _addSection() ─────────────────────────────────────────────────────
void ScreenSignal::_addSection(lv_obj_t* parent, const char* title)
{
    lv_obj_t* sec = lv_obj_create(parent);
    lv_obj_set_size(sec, OPS_SCREEN_W, SEC_H);
    lv_obj_set_style_bg_color(sec, theme::BG_CARD, 0);
    lv_obj_set_style_bg_opa(sec, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sec, 0, 0);
    lv_obj_set_style_border_side(sec, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(sec, theme::BORDER, 0);
    lv_obj_set_style_radius(sec, 0, 0);
    lv_obj_set_style_pad_hor(sec, 6, 0);
    lv_obj_set_style_pad_ver(sec, 0, 0);
    lv_obj_clear_flag(sec, LV_OBJ_FLAG_SCROLLABLE);
    lv_group_remove_obj(sec);

    lv_obj_t* lbl = lv_label_create(sec);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
}

// ── _addRow() ─────────────────────────────────────────────────────────
lv_obj_t* ScreenSignal::_addRow(lv_obj_t* parent, const char* key, const char* val,
                                 lv_obj_t** outValLbl)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, OPS_SCREEN_W, ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_group_remove_obj(row);

    lv_obj_t* keyLbl = lv_label_create(row);
    lv_obj_set_size(keyLbl, KEY_W, ROW_H);
    lv_label_set_text(keyLbl, key);
    lv_label_set_long_mode(keyLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(keyLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(keyLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_pad_left(keyLbl, 6, 0);
    lv_obj_set_style_pad_ver(keyLbl, 2, 0);

    lv_obj_t* valLbl = lv_label_create(row);
    lv_obj_set_flex_grow(valLbl, 1);
    lv_obj_set_height(valLbl, ROW_H);
    lv_label_set_text(valLbl, val);
    lv_label_set_long_mode(valLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(valLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(valLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_pad_ver(valLbl, 2, 0);

    if (outValLbl) *outValLbl = valLbl;
    return valLbl;
}

// ── _build() ─────────────────────────────────────────────────────────
void ScreenSignal::_build()
{
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
    lv_obj_set_style_pad_column(bar, 4, 0);
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

    lv_obj_t* titleLbl = lv_label_create(bar);
    lv_obj_set_flex_grow(titleLbl, 1);
    lv_label_set_text(titleLbl, LV_SYMBOL_WIFI "  Signal");
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);

    // ── Scrollable body ───────────────────────────────────────────────
    _body = lv_obj_create(_screen);
    lv_obj_set_size(_body, OPS_SCREEN_W, BODY_H);
    lv_obj_align(_body, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(_body, theme::BG, 0);
    lv_obj_set_style_border_width(_body, 0, 0);
    lv_obj_set_style_radius(_body, 0, 0);
    lv_obj_set_style_pad_all(_body, 0, 0);
    lv_obj_set_style_pad_row(_body, 0, 0);
    lv_obj_set_scroll_dir(_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Body is the focused group object — receives LV_KEY_ESC for back navigation.
    lv_obj_add_event_cb(_body, _onKey, LV_EVENT_KEY, nullptr);

    // ── SIGNAL QUALITY ────────────────────────────────────────────────
    _addSection(_body, "SIGNAL QUALITY");
    _addRow(_body, "RSSI:",     "--",  &s_rssiLbl);
    _addRow(_body, "SNR:",      "--",  &s_snrLbl);
    _addRow(_body, "Noise:",    "--",  &s_noiseLbl);

    // ── PACKETS ───────────────────────────────────────────────────────
    _addSection(_body, "PACKETS");
    _addRow(_body, "TX:",        "0",  &s_txLbl);
    _addRow(_body, "RX:",        "0",  &s_rxLbl);
    _addRow(_body, "Flood TX:",  "0",  &s_floodTxLbl);
    _addRow(_body, "Flood RX:",  "0",  &s_floodRxLbl);
    _addRow(_body, "Direct TX:", "0",  &s_directTxLbl);
    _addRow(_body, "Direct RX:", "0",  &s_directRxLbl);
    _addRow(_body, "Errors:",    "0",  &s_errorLbl);

    // ── AIRTIME ───────────────────────────────────────────────────────
    _addSection(_body, "AIRTIME");
    _addRow(_body, "TX:",  "0s",  &s_airtimeTxLbl);
    _addRow(_body, "RX:",  "0s",  &s_airtimeRxLbl);

    // ── RADIO CONFIG (live — refreshed every 2 s) ────────────────────
    _addSection(_body, "RADIO CONFIG");
    _addRow(_body, "Freq:",    "--", &s_freqLbl);
    _addRow(_body, "Profile:", "--", &s_profileLbl);

    // ── HARDWARE (partly live: heap/PSRAM) ───────────────────────────
    _addSection(_body, "HARDWARE");
    _addRow(_body, "Board:", "LilyGo T-Deck Plus");
    _addRow(_body, "FW:", OPS_VERSION_STRING);
    _addRow(_body, "CPU:", "ESP32-S3  240 MHz");
    _addRow(_body, "Heap:",  "--",  &s_heapLbl);
    _addRow(_body, "PSRAM:", "--",  &s_psramLbl);

    // ── POWER ─────────────────────────────────────────────────────────
    _addSection(_body, "POWER");
    _addRow(_body, "Battery:", "--", &s_battLbl);

    // ── Live-update timer (2 s period) ────────────────────────────────
    if (!_timer)
        _timer = lv_timer_create(_onTimer, 2000, nullptr);

    // Initial population
    _refresh();
}

// ── _refresh() ────────────────────────────────────────────────────────
void ScreenSignal::_refresh()
{
    if (!_screen || lv_scr_act() != _screen) return;

    RadioStats st = MeshService::instance().radioStats();
    char buf[32];

    // RSSI
    if (st.lastRssi != 0.0f)
        snprintf(buf, sizeof(buf), "%.0f dBm", (double)st.lastRssi);
    else
        snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(s_rssiLbl, buf);
    lv_obj_set_style_text_color(s_rssiLbl, rssiColor(st.lastRssi), 0);

    // SNR
    if (st.lastSnr != 0.0f)
        snprintf(buf, sizeof(buf), "%.1f dB", (double)st.lastSnr);
    else
        snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(s_snrLbl, buf);
    lv_obj_set_style_text_color(s_snrLbl, snrColor(st.lastSnr), 0);

    // Noise floor
    snprintf(buf, sizeof(buf), "%d dBm", (int)st.noiseFloor);
    lv_label_set_text(s_noiseLbl, buf);

    // Packet counts
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.packetsSent);
    lv_label_set_text(s_txLbl, buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.packetsRecv);
    lv_label_set_text(s_rxLbl, buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.floodSent);
    lv_label_set_text(s_floodTxLbl, buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.floodRecv);
    lv_label_set_text(s_floodRxLbl, buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.directSent);
    lv_label_set_text(s_directTxLbl, buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.directRecv);
    lv_label_set_text(s_directRxLbl, buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.packetsRecvError);
    lv_label_set_text(s_errorLbl, buf);
    lv_obj_set_style_text_color(s_errorLbl,
        st.packetsRecvError > 0 ? theme::ORANGE : theme::TEXT, 0);

    // Airtime
    fmtAirtime(st.airtimeTxMs, buf, sizeof(buf));
    lv_label_set_text(s_airtimeTxLbl, buf);

    fmtAirtime(st.airtimeRxMs, buf, sizeof(buf));
    lv_label_set_text(s_airtimeRxLbl, buf);

    // Radio config (profile can change in Settings while this screen is cached)
    {
        static const char* kProfileNames[14] = {
            "Australia",     "Australia Vic.", "EU/UK Narrow",  "EU/UK Long Range",
            "EU/UK Medium",  "Czech Narrow",   "EU 433 LR",     "New Zealand",
            "NZ Narrow",     "Portugal 433",   "Portugal 868",  "Switzerland",
            "USA/Canada",    "Vietnam",
        };
        const auto& cfg = ops::config::get();
        uint8_t p = (cfg.radioProfile < 14) ? cfg.radioProfile : 2;
        snprintf(buf, sizeof(buf), "%.3f MHz", (double)MeshService::instance().getFreqMHz());
        lv_label_set_text(s_freqLbl, buf);
        if (cfg.radioCustom)
            snprintf(buf, sizeof(buf), "%.22s+cust", kProfileNames[p]);
        else
            snprintf(buf, sizeof(buf), "%s", kProfileNames[p]);
        lv_label_set_text(s_profileLbl, buf);
    }

    // Heap / PSRAM
    size_t heap  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (heap > 1024)
        snprintf(buf, sizeof(buf), "%u KB free", (unsigned)(heap / 1024));
    else
        snprintf(buf, sizeof(buf), "%u B free", (unsigned)heap);
    lv_label_set_text(s_heapLbl, buf);

    if (psram > 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB free", (double)psram / (1024.0 * 1024.0));
    else if (psram > 1024)
        snprintf(buf, sizeof(buf), "%u KB free", (unsigned)(psram / 1024));
    else
        snprintf(buf, sizeof(buf), "%u B free", (unsigned)psram);
    lv_label_set_text(s_psramLbl, buf);

    // Battery — average 3 reads to reduce ADC noise
    auto& board = Board::instance();
    int battSum = 0;
    for (int i = 0; i < 3; i++) battSum += board.batteryPercent();
    int  battPct = battSum / 3;
    bool charging = board.batteryCharging();
    if (charging)
        snprintf(buf, sizeof(buf), "%d%% " LV_SYMBOL_CHARGE, battPct);
    else
        snprintf(buf, sizeof(buf), "%d%%", battPct);
    lv_label_set_text(s_battLbl, buf);
    lv_color_t bc = charging      ? theme::GREEN  :
                    battPct >= 50  ? theme::GREEN  :
                    battPct >= 20  ? theme::ORANGE :
                                     theme::RED;
    lv_obj_set_style_text_color(s_battLbl, bc, 0);
}

// ── show() ────────────────────────────────────────────────────────────
void ScreenSignal::show()
{
    if (!_screen)
        _build();

    lv_scr_load(_screen);

    // Give focus to the scrollable body so trackball up/down scrolls it
    // and backspace (→ LV_KEY_ESC) triggers _onKey → showLauncher().
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, _body);
        lv_group_focus_obj(_body);
    }

    _refresh();
    OPS_LOG("UI", "Signal shown");
}

// ── Callbacks ─────────────────────────────────────────────────────────
void ScreenSignal::_onHomeClick(lv_event_t* /*e*/)
{
    ScreenLauncher::show();
}

void ScreenSignal::_onKey(lv_event_t* e)
{
    uint32_t key = *(uint32_t*)lv_event_get_param(e);
    if (key == LV_KEY_ESC)
        ScreenLauncher::show();
}

void ScreenSignal::_onTimer(lv_timer_t* /*t*/)
{
    _refresh();
}

}}  // namespace ops::ui
