// Saitama — ScreenSigGen.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

// RF Signal Generator screen.
// Outputs CW (continuous carrier) or LoRa infinite preamble on any frequency
// 150–960 MHz at configurable power.  Auto-stops after 5 minutes.
// Hidden command: /siggen  (not in /help)
class ScreenSigGen {
public:
    static void show();
    static bool isActive();
    static void update();              // called every tick from UIScreen::tick()
    static void navigate(int dx, int dy);  // trackball: dx=freq, dy=power

private:
    static lv_obj_t* _screen;
    static lv_obj_t* _freqLbl;    // large frequency display
    static lv_obj_t* _statLbl;    // TX ACTIVE / STANDBY
    static lv_obj_t* _paramLbl;   // "CW  ·  +22 dBm"
    static lv_obj_t* _txBtn;
    static lv_obj_t* _txBtnLbl;
    static lv_obj_t* _cntdownLbl; // auto-stop countdown

    static void _buildScreen();
    static void _refreshLabels();
    static void _startTx();
    static void _stopTx();

    static void _onKey(lv_event_t* e);
    static void _onHome(lv_event_t* e);
    static void _onTxBtn(lv_event_t* e);
};

}}  // namespace ops::ui
