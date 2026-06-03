// Saitama — ScreenChanScan.h
// Copyright 2026 Saitama — MIT License

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

// LoRa CAD channel scanner.
// Centres on the active mesh frequency with 4 channels above and below
// at a selectable step (±100/200/500 kHz). Trackball ←→ changes step.
// Hidden command: /chscan  (not in /help)
class ScreenChanScan {
public:
    static constexpr int MAX_CHAN   = 9;
    static constexpr int CENTER_ROW = MAX_CHAN / 2;  // row 4

    static void show();
    static bool isActive();
    static void update();              // called every tick from UIScreen::tick()
    static void navigate(int dx, int dy);  // dx=step switch

private:

    static lv_obj_t* _screen;
    static lv_obj_t* _regionLbl;
    static lv_obj_t* _chanFreqLbl[MAX_CHAN];
    static lv_obj_t* _chanBar[MAX_CHAN];
    static lv_obj_t* _chanPctLbl[MAX_CHAN];
    static lv_obj_t* _chanStatLbl[MAX_CHAN];
    static lv_obj_t* _meshInfoLbl;

    static void _buildScreen();
    static void _switchStep(int dir);
    static void _updateRows();

    static void _onKey(lv_event_t* e);
    static void _onHome(lv_event_t* e);
};

}}  // namespace ops::ui
