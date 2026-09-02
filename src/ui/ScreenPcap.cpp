// Saitama — ScreenPcap.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Layout (320 × 240):
//
//   ┌────────────────────────────────────────┐  y=0
//   │ [⌂]  Packet Capture                    │  28 px  top bar
//   ├────────────────────────────────────────┤  y=28
//   │  Frequency        869.525 MHz          │
//   │  Packets recorded 128                  │
//   │  File              /pcap/20260829_...  │
//   │  Status            Capturing           │
//   │                                        │
//   │           [  Stop Capture  ]           │
//   ├────────────────────────────────────────┤
//   │ raw frames → /pcap/*.pcap              │  14 px  status
//   └────────────────────────────────────────┘  y=240
//
// A capture keeps recording in the background once started — leaving this
// screen does not stop it. tick() is called unconditionally from
// UIScreen::tick() to drain MeshService's raw-frame queue to SD.

#include "ScreenPcap.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "Emoji.h"
#include "../mesh/MeshService.h"
#include "../utils/SDCard.h"
#include "../utils/Config.h"
#include "../utils/Log.h"
#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include <time.h>

namespace ops { namespace ui {

static constexpr int TOP_H  = 28;
static constexpr int STAT_H = 14;

lv_obj_t* ScreenPcap::_screen     = nullptr;
lv_obj_t* ScreenPcap::_freqLbl    = nullptr;
lv_obj_t* ScreenPcap::_countLbl   = nullptr;
lv_obj_t* ScreenPcap::_fileLbl    = nullptr;
lv_obj_t* ScreenPcap::_statusLbl  = nullptr;
lv_obj_t* ScreenPcap::_toggleBtn  = nullptr;
lv_obj_t* ScreenPcap::_toggleLbl  = nullptr;

static bool     s_capturing   = false;
static char     s_filePath[64] = {};
static uint32_t s_packetCount  = 0;
static bool     s_writeError   = false;

// ── _startCapture() ──────────────────────────────────────────────────
void ScreenPcap::_startCapture()
{
    if (!ops::sdcard::isMounted()) {
        s_writeError = true;
        return;
    }
    time_t now = ops::config::localEpoch();
    struct tm t;
    gmtime_r(&now, &t);
    snprintf(s_filePath, sizeof(s_filePath), "/pcap/%04d%02d%02d_%02d%02d%02d.pcap",
              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);

    if (!ops::sdcard::pcapCreate(s_filePath)) {
        s_writeError = true;
        s_filePath[0] = '\0';
        return;
    }
    s_writeError  = false;
    s_packetCount = 0;
    s_capturing   = true;
    ops::MeshService::instance().setPcapCapture(true);
    OPS_LOG("Pcap", "Capture started: %s", s_filePath);
}

// ── _stopCapture() ───────────────────────────────────────────────────
void ScreenPcap::_stopCapture()
{
    s_capturing = false;
    ops::MeshService::instance().setPcapCapture(false);
    OPS_LOG("Pcap", "Capture stopped: %s (%u pkts)", s_filePath, (unsigned)s_packetCount);
}

// ── tick() ────────────────────────────────────────────────────────────
// Runs every frame regardless of the active screen — a capture recorded
// here keeps going while the user is off doing something else.
void ScreenPcap::tick()
{
    ops::CapturedPacket pkt;
    while (ops::MeshService::instance().dequeueCapturedPacket(pkt)) {
        if (!s_capturing) continue;  // drain stragglers after Stop, don't write them
        if (!ops::sdcard::pcapAppend(s_filePath, pkt.timestamp, pkt.usec, pkt.data, pkt.len)) {
            s_writeError = true;
            _stopCapture();
            continue;
        }
        s_packetCount++;
    }

    if (isActive()) _refresh();
}

// ── _refresh() ────────────────────────────────────────────────────────
void ScreenPcap::_refresh()
{
    if (!_screen) return;

    char buf[48];
    snprintf(buf, sizeof(buf), "%.3f MHz", (double)ops::MeshService::instance().getFreqMHz());
    lv_label_set_text(_freqLbl, buf);

    snprintf(buf, sizeof(buf), "%u", (unsigned)s_packetCount);
    lv_label_set_text(_countLbl, buf);

    lv_label_set_text(_fileLbl, s_filePath[0] ? s_filePath : "--");

    if (s_writeError) {
        lv_label_set_text(_statusLbl, "SD write failed");
        lv_obj_set_style_text_color(_statusLbl, theme::RED, 0);
    } else if (s_capturing) {
        lv_label_set_text(_statusLbl, "Capturing");
        lv_obj_set_style_text_color(_statusLbl, theme::GREEN, 0);
    } else {
        lv_label_set_text(_statusLbl, "Idle");
        lv_obj_set_style_text_color(_statusLbl, theme::TEXT_MUTED, 0);
    }

    lv_label_set_text(_toggleLbl, s_capturing ? LV_SYMBOL_STOP " Stop Capture"
                                               : LV_SYMBOL_PLAY " Start Capture");
    lv_obj_set_style_bg_color(_toggleBtn, s_capturing ? theme::RED : theme::PRIMARY, 0);
}

// ── isActive() ────────────────────────────────────────────────────────
bool ScreenPcap::isActive() { return _screen && lv_scr_act() == _screen; }

// ── callbacks ─────────────────────────────────────────────────────────
void ScreenPcap::_onHome(lv_event_t*) { ScreenLauncher::show(); }

void ScreenPcap::_onKey(lv_event_t* e)
{
    uint32_t* key = (uint32_t*)lv_event_get_param(e);
    if (key && *key == LV_KEY_ESC) ScreenLauncher::show();
}

void ScreenPcap::_onToggle(lv_event_t*)
{
    if (s_capturing) _stopCapture();
    else             _startCapture();
    ScreenPcap::_refresh();
}

// ── _build() ──────────────────────────────────────────────────────────
void ScreenPcap::_build()
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
    // Magnifier emoji (U+1F50D) via the imgfont, matching the launcher tile.
    lv_label_set_text(titleLbl, "\xF0\x9F\x94\x8D Packet Capture");
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl,
        ops::emoji::emojiFont(&lv_font_montserrat_12), 0);

    // ── Info rows ─────────────────────────────────────────────────────
    static constexpr int ROW_H  = 22;
    static constexpr int COL_KEY = 8;
    static constexpr int COL_VAL = 100;

    auto addRow = [&](int idx, const char* key) -> lv_obj_t* {
        int y = TOP_H + 6 + idx * ROW_H;
        lv_obj_t* keyLbl = lv_label_create(_screen);
        lv_label_set_text(keyLbl, key);
        lv_obj_set_style_text_color(keyLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(keyLbl, theme::bodyFont10(), 0);
        lv_obj_set_pos(keyLbl, COL_KEY, y);

        lv_obj_t* valLbl = lv_label_create(_screen);
        lv_obj_set_style_text_color(valLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(valLbl, theme::bodyFont10(), 0);
        lv_obj_set_pos(valLbl, COL_VAL, y);
        lv_obj_set_width(valLbl, OPS_SCREEN_W - COL_VAL - 8);
        lv_label_set_long_mode(valLbl, LV_LABEL_LONG_CLIP);
        return valLbl;
    };

    _freqLbl   = addRow(0, "Frequency");
    _countLbl  = addRow(1, "Packets recorded");
    _fileLbl   = addRow(2, "File");
    _statusLbl = addRow(3, "Status");

    // ── Toggle button ─────────────────────────────────────────────────
    _toggleBtn = lv_btn_create(_screen);
    lv_obj_set_size(_toggleBtn, 180, 34);
    lv_obj_align(_toggleBtn, LV_ALIGN_TOP_MID, 0, TOP_H + 6 + 4 * ROW_H + 10);
    lv_obj_set_style_bg_color(_toggleBtn, theme::PRIMARY, 0);
    lv_obj_set_style_bg_color(_toggleBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(_toggleBtn, theme::BG_CARD, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(_toggleBtn, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(_toggleBtn, 1, 0);
    lv_obj_set_style_radius(_toggleBtn, 6, 0);
    lv_obj_set_style_shadow_width(_toggleBtn, 0, 0);
    lv_obj_add_event_cb(_toggleBtn, _onToggle, LV_EVENT_CLICKED, nullptr);
    _toggleLbl = lv_label_create(_toggleBtn);
    lv_label_set_text(_toggleLbl, LV_SYMBOL_PLAY " Start Capture");
    lv_obj_set_style_text_color(_toggleLbl, theme::TEXT, 0);
    // Montserrat, NOT bodyFont12(): the extended-Latin face carries no
    // FontAwesome glyphs, so LV_SYMBOL_PLAY/STOP would render as tofu there.
    lv_obj_set_style_text_font(_toggleLbl, &lv_font_montserrat_12, 0);
    lv_obj_center(_toggleLbl);
    lv_group_add_obj(lv_group_get_default(), _toggleBtn);
    lv_group_focus_obj(_toggleBtn);

    // ── Bottom status bar ─────────────────────────────────────────────
    lv_obj_t* footer = lv_label_create(_screen);
    lv_obj_set_pos(footer, 4, OPS_SCREEN_H - STAT_H + 1);
    lv_obj_set_style_text_color(footer, lv_color_make(70, 70, 70), 0);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_10, 0);
    lv_label_set_text(footer, "raw frames -> /pcap/*.pcap  (keeps recording off-screen)");

    lv_obj_add_event_cb(_toggleBtn, _onKey, LV_EVENT_KEY, nullptr);
}

// ── show() ────────────────────────────────────────────────────────────
void ScreenPcap::show()
{
    if (!_screen) _build();
    if (!_screen) return;
    _refresh();
    lv_scr_load(_screen);
}

}}  // namespace ops::ui
