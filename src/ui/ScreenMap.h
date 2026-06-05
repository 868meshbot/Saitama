// Saitama — ScreenMap.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Offline tile map screen.  Displays OSM PNG tiles from SD /maps/osm/{x}/{y}/{z}.png.
// Repeaters are shown as purple dots, chat contacts as blue dots.
// Right strip: zoom +/- controls.  Trackball or touch-drag pans; backspace exits.

#pragma once
#include <lvgl.h>
#include <cstdint>

namespace ops { namespace ui {

class ScreenMap {
public:
    static void show();

    // Called by UIScreen::tick() for trackball pan (pixels).
    static bool isActive();
    static void navigate(int dxPx, int dyPx);

private:
    static lv_obj_t*   _screen;
    static lv_obj_t*   _canvas;
    static lv_color_t* _canvasBuf;
    static lv_obj_t*   _zoomLbl;
    static lv_obj_t*   _mountOverlay;  // SD remount dialog; nullptr when not shown

    static float _centerLat;
    static float _centerLng;
    static int   _zoom;
    static bool  _gpsCentered;   // true once snapped to GPS fix on first show

    static void _build();
    static void _redrawMap();
    static void _updateZoomLabel();
    static void _showMountDialog();

    static void _onHomeClick   (lv_event_t* e);
    static void _onExitClick   (lv_event_t* e);
    static void _onZoomIn      (lv_event_t* e);
    static void _onZoomOut     (lv_event_t* e);
    static void _onKey         (lv_event_t* e);
    static void _onMountClick  (lv_event_t* e);
    static void _onMountCancel (lv_event_t* e);
    static void _onTouch       (lv_event_t* e);  // touch drag → pan
};

}}  // namespace ops::ui
