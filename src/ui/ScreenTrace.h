// Saitama — ScreenTrace.h
// Copyright 2026 Saitama — MIT License
//
// Trace-route screen.  Lets the user pick any known contact or repeater
// (alphabetically sorted) and fire a MeshCore TRACE (0x09) packet along
// the stored direct path.  Results are shown when onTraceRecv fires on
// this device (either as destination or after the final-hop retransmission).

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenTrace {
public:
    static void show();

    // Called from UIScreen::tick() each frame.  Polls MeshService for a
    // completed TraceResult and refreshes the result area.
    static void tick();

private:
    static lv_obj_t* _screen;
    static lv_obj_t* _dropdown;
    static lv_obj_t* _traceBtn;
    static lv_obj_t* _statusLbl;
    static lv_obj_t* _hopList;

    static void _build();
    static void _rebuildHopList();
    static void _setStatus(const char* msg, lv_color_t col);

    static void _onHomeClick   (lv_event_t* e);
    static void _onDropChange  (lv_event_t* e);
    static void _onTraceClick  (lv_event_t* e);
};

}}  // namespace ops::ui
