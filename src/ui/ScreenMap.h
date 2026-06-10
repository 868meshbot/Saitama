// Saitama — ScreenMap.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Offline tile map screen — NavBoxLib backend (navboxlib branch).
// NavBoxLib manages a TILECACHE_SIZE tile cache as LVGL image objects and a
// MarkerLayer for contact/repeater/self dots.  Tiles are loaded from SD at
// /maps/osm/{z}/{x}/{y}.png (standard OSM slippy-map layout).
// Trackball and touch-drag pan; +/- buttons zoom; backspace exits.

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
    static lv_obj_t* _screen;
    static lv_obj_t* _zoomLbl;       // unused in NavBoxLib layout; kept for compat
    static lv_obj_t* _mountOverlay;  // SD remount dialog; nullptr when not shown

    static float _centerLat;
    static float _centerLng;
    static int   _zoom;
    static bool  _gpsCentered;   // true once snapped to GPS fix on first show

    static void _build();
    static void _refreshMarkers();    // rebuild NavBoxLib MarkerLayer from live data
    static void _updateZoomLabel();   // no-op; kept for call-site compat
    static void _showMountDialog();

    static void _onHomeClick   (lv_event_t* e);
    static void _onExitClick   (lv_event_t* e);
    static void _onZoomIn      (lv_event_t* e);
    static void _onZoomOut     (lv_event_t* e);
    static void _onKey         (lv_event_t* e);
    static void _onMountClick  (lv_event_t* e);
    static void _onMountCancel (lv_event_t* e);
    static void _onTouch       (lv_event_t* e);
};

}}  // namespace ops::ui
