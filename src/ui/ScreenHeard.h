// Saitama — ScreenHeard.h
// Copyright 2026 Saitama — MIT License
//
// "Heard" screen — lists every station MeshCore has heard, with RSSI and
// last-heard time.  Tapping a row offers to save the station to Contacts.

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenHeard {
public:
    static void show();

    // Called by UIScreen::tick when the peer list changes.
    // Rebuilds the screen immediately if it is currently visible.
    static void onPeersUpdated();

    // Public so _buildSaveDialog (free function in .cpp) can register them.
    static void _onSaveConfirm    (lv_event_t* e);  // user_data = overlay obj
    static void _onSaveRepConfirm (lv_event_t* e);  // user_data = overlay obj
    static void _onSaveCancel     (lv_event_t* e);  // user_data = overlay obj

private:
    static lv_obj_t* _screen;
    static bool      _isVisible;

    static void _build();
    static void _onHomeClick   (lv_event_t* e);
    static void _onRowClick    (lv_event_t* e);  // user_data = peer index (int)
    static void _onKey         (lv_event_t* e);
};

}}  // namespace ops::ui
