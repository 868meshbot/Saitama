// Saitama — ScreenPlaceholder.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Generic stub screen used for app tiles that are not yet implemented.
// Shows the app name and a Home button in the top bar.

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenPlaceholder {
public:
    // Show a stub screen with the given title.
    // Always recreates so the title is correct (different apps share this).
    static void show(const char* title);

    // Called from UIScreen::tick() — auto-refreshes the GPS screen every 3 s.
    static void tick();

private:
    static lv_obj_t* _screen;
    static void      _onHomeClick(lv_event_t* e);
};

}}  // namespace ops::ui
