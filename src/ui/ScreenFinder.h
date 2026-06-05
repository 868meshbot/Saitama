// Saitama — ScreenFinder.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include <lvgl.h>
#include "../mesh/MeshService.h"

namespace ops { namespace ui {

class ScreenFinder {
public:
    static void show();
    // Called from UIScreen::tick() to push a discover result into the list.
    static void addDiscoverResult(const ops::DiscoverEntry& e);
    // Called from UIScreen::tick() each frame to update the scan status label.
    static void tick();

private:
    static lv_obj_t* _screen;
    static lv_obj_t* _statusLbl;
    static lv_obj_t* _list;
    static lv_obj_t* _scanBtn;
    static lv_obj_t* _scanBtnLbl;

    static void _buildScreen();
    static void _rebuildList();
    static void _onScan(lv_event_t* e);
    static void _onBack(lv_event_t* e);
    static void _onRowClick(lv_event_t* e);
    static void _showActionPopup(int idx);
    static void _onPopupAdd(lv_event_t* e);
    static void _onPopupClose(lv_event_t* e);
};

}}  // namespace ops::ui
