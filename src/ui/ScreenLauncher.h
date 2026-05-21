// Saitama — ScreenLauncher.h
// Copyright 2026 Saitama — MIT License
//
// Main app launcher screen.
//
// Layout (320 x 240 landscape):
//
//  ┌───────────────────────────────────────┐  y = 0
//  │ [⌂]                        12:34     │  top bar   28 px (channels from config)
//  ├───────────────────────────────────────┤  y = 28
//  │  Chat   Contacts  Repeat.  Finder    │
//  │  Heard    Map    Advertise Settings  │  grid      188 px
//  │  Trace  Terminal   GPS     Signal    │
//  ├───────────────────────────────────────┤  y = 216
//  │ OPS-0001                     🔋 87%  │  bottom bar 24 px
//  └───────────────────────────────────────┘  y = 240

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenLauncher {
public:
    // Show (or re-show) the launcher. Safe to call repeatedly.
    static void show();

    // 2-D trackball navigation across the 12 app tiles.
    // dx/dy are -1, 0, or +1.  Going up from row 0 selects the Home button.
    static void navigate(int dx, int dy);

    // Fire a click on the currently highlighted tile / Home button.
    static void confirmSelect();

    // True when the launcher screen is the active LVGL screen.
    static bool isActive();

    // Called from UIScreen::tick() — refreshes the clock label.
    static void refreshClock();

    // Called from UIScreen::tick() — refreshes the battery label.
    static void refreshBattery(int percent, bool charging = false);

    // Called from UIScreen::tick() — refreshes GPS icon and satellite count.
    // gpsMode: 0=off (red), 1=intermittent (orange), 2=on (green/muted by fix).
    static void refreshStatus(uint8_t gpsMode, bool hasFix, int satellites);

    // Called from UIScreen::tick() — refreshes LoRa radio status indicator.
    static void refreshRadio(bool initialized, bool active);

    // Called from UIScreen::tick() when contact unread state changes.
    static void refreshUnreadDot();

    // Called from UIScreen::tick() to update the speaker mute/unmute icon.
    static void refreshSpeaker(bool enabled);

    // Called from UIScreen::tick() when peerSerial changes while the
    // advertise screen is active — refreshes the repeater response list.
    static void onAdvertPeersUpdated();

private:
    static lv_obj_t* _screen;
    static lv_obj_t* _timeLbl;     // right end of top bar
    static lv_obj_t* _battLbl;     // right end of bottom bar
    static lv_obj_t* _satLbl;      // GPS satellite count (hidden when GPS off)
    static lv_obj_t* _radioLbl;    // LoRa radio status (hidden until initialized)
    static lv_obj_t* _speakerLbl;  // mute/volume icon next to GPS

    static void _buildTopBar   (lv_obj_t* parent);
    static void _buildGrid     (lv_obj_t* parent);
    static void _buildBottomBar(lv_obj_t* parent);

    static void _onIconClick   (lv_event_t* e);  // grid tile pressed
};

}}  // namespace ops::ui
