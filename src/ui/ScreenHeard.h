// Saitama — ScreenHeard.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// "Heard" screen — lists every station MeshCore has heard, with signal bars
// and last-heard time. Tapping a row shows its details (name, identity, RSSI,
// and a repeater's owner) and offers to save or update it.

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenHeard {
public:
    static void show();

    // Called by UIScreen::tick when the peer list changes.
    // Rebuilds the screen immediately if it is currently visible.
    static void onPeersUpdated();

    // Public so _buildPeerDialog (free function in .cpp) can register them.
    static void _onSaveConfirm    (lv_event_t* e);  // user_data = overlay obj
    static void _onSaveRepConfirm (lv_event_t* e);  // user_data = overlay obj
    static void _onSaveCancel     (lv_event_t* e);  // user_data = overlay obj
    static void _onClearList      (lv_event_t* e);  // top-bar Clear List button

    static void _onHomeClick   (lv_event_t* e);  // Home and the close X

private:
    static lv_obj_t* _screen;
    static bool      _isVisible;

    static void _build();
    static void _onRowClick    (lv_event_t* e);  // user_data = peer index (int)
    static void _onKey         (lv_event_t* e);
};

}}  // namespace ops::ui
