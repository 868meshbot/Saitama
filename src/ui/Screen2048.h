// Saitama — Screen2048.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// 2048 sliding-tile puzzle screen.
// WASD/trackball to slide; N=new game; Bksp=launcher.
// Best score persisted in NVS namespace "games", key "2048_b".

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class Screen2048 {
public:
    static void show();
    static bool isActive();
    static void navigate(int dx, int dy);  // called by UIScreen::tick() for trackball

private:
    static lv_obj_t* _screen;

    static void _build();
    static void _onKey      (lv_event_t* e);
    static void _onHomeClick(lv_event_t* e);
    static void _onNewClick (lv_event_t* e);
    static void _onKeepClick(lv_event_t* e);
};

}}  // namespace ops::ui
