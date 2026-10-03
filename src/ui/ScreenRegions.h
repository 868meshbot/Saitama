// Saitama — ScreenRegions.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Region scopes: the default scope this node floods with, a Discover sweep
// that asks repeaters in direct range which regions they flood for, and the
// saved-regions catalog (tap a region to make it the default).

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenRegions {
public:
    static void show();

    // Called from UIScreen::tick() each frame: collects discovery replies.
    static void tick();

private:
    static lv_obj_t* _screen;
    static lv_obj_t* _body;
    static lv_obj_t* _defaultLbl;
    static lv_obj_t* _statusLbl;
    static lv_obj_t* _repList;
    static lv_obj_t* _chips;
    static lv_obj_t* _discoverBtn;

    static void _build();
    static void _rebuildRepeaters();
    static void _rebuildChips();
    static void _refreshDefault();
    static void _setStatus(const char* msg, lv_color_t col);

    static void _onHomeClick    (lv_event_t* e);
    static void _onDiscoverClick(lv_event_t* e);
    static void _onChipClick    (lv_event_t* e);
    static void _onAddChipClick (lv_event_t* e);   // "+ NAME": save a discovered region
    static void _onKey          (lv_event_t* e);
};

}}  // namespace ops::ui
