// Saitama — ScreenSpectrum.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

// Spectrum analyzer screen: sweeps the SX1262 using GetRssiInst across a
// configurable band, displaying a live trace + scrolling waterfall.
// Hidden command: /spectrum  (not in /help)
class ScreenSpectrum {
public:
    static void show();
    static bool isActive();

    // Called every tick by UIScreen::tick() when this screen is active.
    // Performs one full sweep and redraws the canvas.
    static void update();

    // Trackball routing from UIScreen::tick(): dx=pan, dy=zoom.
    static void navigate(int dx, int dy);

private:
    static lv_obj_t*   _screen;
    static lv_obj_t*   _canvas;
    static uint16_t* _canvasBuf;  // RGB565 pixels matching LV_COLOR_FORMAT_NATIVE
    static lv_obj_t*   _infoLbl;
    static lv_obj_t*   _cadLbl;    // top-bar CAD status indicator

    static void _buildScreen();
    static void _redrawCanvas();
    static void _updateInfo();

    static void _onKey(lv_event_t* e);
    static void _onHome(lv_event_t* e);
};

}}  // namespace ops::ui
