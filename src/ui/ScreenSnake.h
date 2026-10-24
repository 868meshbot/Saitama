// Saitama — ScreenSnake.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Snake game screen.
// Trackball, screen swipes or WASD/arrows to steer; trackball click,
// Space or Enter to pause; N=new game; Bksp=launcher.
// Best score persisted in NVS namespace "games", key "snake_b".

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenSnake {
public:
    static void show();
    static bool isActive();
    static void navigate(int dx, int dy);  // called by UIScreen::tick() for trackball
    static void press();                   // trackball click: start / pause / resume

private:
    static lv_obj_t* _screen;

    static void _build();
    static void _onKey      (lv_event_t* e);
    static void _onTouch    (lv_event_t* e);
    static void _onDraw     (lv_event_t* e);
    static void _onTick     (lv_timer_t* t);
    static void _onHomeClick(lv_event_t* e);
    static void _onNewClick (lv_event_t* e);
};

}}  // namespace ops::ui
