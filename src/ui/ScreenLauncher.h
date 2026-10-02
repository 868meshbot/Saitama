// Saitama — ScreenLauncher.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Main app launcher screen: three pages, swiped (or trackballed past an
// edge) left/right. Page 0 is home: stations heard this session (newest 20)
// fall like rain behind four buttons. Pages 1 and 2 are the classic grids.
//
// Layout (320 x 240 landscape), home page:
//
//  ┌───────────────────────────────────────┐  y = 0
//  │ [⌂]                        12:34     │  top bar   28 px
//  ├───────────────────────────────────────┤  y = 28
//  │   G4ABC          M0XYZ               │
//  │          2E0QQ           G7RPT       │  rain      144 px
//  │  M7KEV        G4ABC                  │
//  │ [Chat] [Contacts] [Map] [Settings]   │  buttons    44 px
//  ├───────────────────────────────────────┤  y = 216
//  │ OPS-0001        ● ○ ○        🔋 87%  │  bottom bar 24 px
//  └───────────────────────────────────────┘  y = 240

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenLauncher {
public:
    // Show (or re-show) the launcher. Safe to call repeatedly.
    static void show();

    // 2-D trackball navigation: across the home buttons or a grid's tiles;
    // left/right past the edge moves to the neighbouring page.
    // dx/dy are -1, 0, or +1.  On a grid, up from row 0 selects the Home button.
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

    // Called from UIScreen::tick() to update the Bluetooth status icon.
    static void refreshBluetooth(bool enabled);

    // Called from UIScreen::tick() for each channel message: the sender joins
    // the home page rain (channel packets carry no key, so names only), the
    // channel's activity is noted, and a line is drawn sender → channel (and
    // sender → user for an "@[Name]" reply).
    static void noteChannelMessage(const char* sender, const char* channel,
                                   const char* text, float rssi, uint8_t hops);

    // Called from UIScreen::tick() for each DM: line sender → this node.
    static void noteDirectMessage(const char* sender);

    // Called by Chat after sending: line this node → channel slot chSlot
    // (and → the user an "@[Name]" reply is for), or → dmTo for a DM
    // (chSlot = -1).
    static void noteOwnMessage(int chSlot, const char* dmTo, const char* text);

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
    static lv_obj_t* _btLbl;       // Bluetooth status icon in top bar

    static void _buildTopBar   (lv_obj_t* parent);
    static void _buildGrid     (lv_obj_t* parent);
    static void _buildBottomBar(lv_obj_t* parent);

    static void _onIconClick   (lv_event_t* e);  // grid tile pressed
};

}}  // namespace ops::ui
