// Saitama — ScreenPcap.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Packet capture — records raw over-the-air mesh frames to a standard
// libpcap file on the SD card (/pcap/<timestamp>.pcap). Once started, the
// capture keeps running in the background regardless of which screen is
// active, the same way message history logging does.

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenPcap {
public:
    static void show();
    static bool isActive();

    // Drains newly-captured raw frames to the open pcap file and updates the
    // live packet count. Called every frame from UIScreen::tick(), independent
    // of the active screen, so a capture keeps recording in the background.
    static void tick();

private:
    static lv_obj_t* _screen;
    static lv_obj_t* _freqLbl;
    static lv_obj_t* _countLbl;
    static lv_obj_t* _fileLbl;
    static lv_obj_t* _statusLbl;
    static lv_obj_t* _toggleBtn;
    static lv_obj_t* _toggleLbl;

    static void _build();
    static void _refresh();
    static void _startCapture();
    static void _stopCapture();

    static void _onHome  (lv_event_t* e);
    static void _onKey   (lv_event_t* e);
    static void _onToggle(lv_event_t* e);
};

}}  // namespace ops::ui
