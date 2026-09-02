// Saitama — ScreenFoxhunt.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// BLE foxhunt — radio direction finding by ear.
//
// Scans for BLE advertisers, lets the user pick one, then beeps faster and
// higher-pitched as the target's RSSI rises, so the device can be walked to
// its source without watching the screen.
//
// The proximity-feedback idea and the shape of the RSSI -> beep-rate mapping
// come from OUI-SPY Foxhunter (https://github.com/colonelpanichacks/ouispy-foxhunter),
// adapted here for the T-Deck's I2S speaker and LVGL UI rather than a PWM
// piezo and a WiFi config portal. See ScreenFoxhunt.cpp for what changed and
// why.

#pragma once
#include <lvgl.h>
#include <stdint.h>

namespace ops { namespace ui {

class ScreenFoxhunt {
public:
    static void show();
    static bool isActive();

    // Drains newly-seen BLE advertisements into the UI and drives the
    // proximity beeper. Called every frame from UIScreen::tick().
    static void tick();

    // Trackball up/down moves the device-list selection.
    static void navigate(int dx, int dy);
    // Centre press / enter selects the highlighted device.
    static void confirmSelect();

    // Stops scanning and silences the beeper. Called when leaving the screen
    // so BLE scanning does not keep running in the background.
    static void stop();

private:
    static lv_obj_t* _screen;
    static lv_obj_t* _title;
    static lv_obj_t* _list;        // MODE_LIST container
    static lv_obj_t* _hunt;        // MODE_HUNT container
    static lv_obj_t* _rssiLbl;
    static lv_obj_t* _bar;
    static lv_obj_t* _targetLbl;
    static lv_obj_t* _stateLbl;
    static lv_obj_t* _addrLbl;   // address-kind warning on the hunt screen
    static lv_obj_t* _rateLbl;
    static lv_obj_t* _hintLbl;

    static void _build();
    static void _rebuildList();
    static void _refreshListValues();

    // Rename dialog — opened with 'r' while hunting a target.
    static void _showRenameDialog(int devIdx);
    // 'g' — request a GATT read of the GAP Device Name. Non-blocking: the
    // connect runs on its own task (see the note in the .cpp — doing it on the
    // loop task tripped the 5 s task watchdog). _probeTick() drives it.
    static void _probeName(int devIdx);
    static void _probeTick();
    static void _forgetDevice(int devIdx);

    // Persistent user-assigned names, /ops/foxnames.json. Most BLE devices
    // broadcast no name at all, so a local alias is the only way to label them.
    static void _aliasLoad();
    static void _aliasSave();
    static void _aliasSet(const char* mac, const char* name);

    static void _onRenameSave  (lv_event_t* e);
    static void _onRenameCancel(lv_event_t* e);
    static void _onRenameKey   (lv_event_t* e);
    static void _enterHunt(int devIdx);
    static void _enterList();
    static void _refreshHunt();
    static void _updateHighlight();

    static void _onHome  (lv_event_t* e);
    static void _onKey   (lv_event_t* e);
    static void _onRowClick(lv_event_t* e);
    static void _onBack  (lv_event_t* e);
};

}}  // namespace ops::ui
