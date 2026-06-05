// Saitama — ScreenBoot.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Splash screen shown on power-up. Displays project name, version, and a
// loading spinner, then automatically transitions to ScreenLauncher.

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenBoot {
public:
    // Create and show the boot splash. Call once from UIScreen::init().
    static void show();

private:
    static lv_obj_t* _screen;
    static void      _onTimerDone(lv_timer_t* t);
};

}}  // namespace ops::ui
