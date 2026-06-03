// Saitama — ScreenSignal.h
// Copyright 2026 Saitama — MIT License
//
// Radio signal & statistics screen.
// Displays live RSSI/SNR/noise, packet counts (flood/direct split),
// TX/RX airtime, radio config, and hardware info.
// Refreshes every 2 s.  Backspace returns to the launcher.

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenSignal {
public:
    static void show();

private:
    static lv_obj_t*   _screen;
    static lv_obj_t*   _body;       // scrollable content area
    static lv_timer_t* _timer;

    // Live-update labels (value column only)
    static lv_obj_t* s_rssiLbl;
    static lv_obj_t* s_snrLbl;
    static lv_obj_t* s_noiseLbl;
    static lv_obj_t* s_txLbl;
    static lv_obj_t* s_rxLbl;
    static lv_obj_t* s_floodTxLbl;
    static lv_obj_t* s_floodRxLbl;
    static lv_obj_t* s_directTxLbl;
    static lv_obj_t* s_directRxLbl;
    static lv_obj_t* s_errorLbl;
    static lv_obj_t* s_airtimeTxLbl;
    static lv_obj_t* s_airtimeRxLbl;
    static lv_obj_t* s_freqLbl;
    static lv_obj_t* s_profileLbl;
    static lv_obj_t* s_heapLbl;
    static lv_obj_t* s_psramLbl;
    static lv_obj_t* s_battLbl;

    static void _build();
    static void _refresh();

    // Widget-factory helpers
    static void      _addSection(lv_obj_t* parent, const char* title);
    static lv_obj_t* _addRow(lv_obj_t* parent, const char* key, const char* val,
                             lv_obj_t** outValLbl = nullptr);

    // Event/timer callbacks
    static void _onHomeClick(lv_event_t* e);
    static void _onKey      (lv_event_t* e);
    static void _onTimer    (lv_timer_t* t);
};

}}  // namespace ops::ui
