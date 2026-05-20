// Saitama — ScreenPower.h
// Copyright 2026 Saitama — MIT License
//
// Power monitor — hidden from launcher, launched via /power in terminal.
// Graphs battery % over time; shows estimated component current draws.
// Press 't' to cycle the chart timescale (5m / 10m / 30m / 1h).

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenPower {
public:
    static void show();
    static bool isActive();
    static void tick();  // called from UIScreen::tick()

private:
    static lv_obj_t*          _screen;
    static lv_obj_t*          _body;
    static lv_obj_t*          _chart;
    static lv_chart_series_t* _battSer;
    static lv_obj_t*          _chartHdrLbl;
    static lv_obj_t*          _loraLbl;
    static lv_obj_t*          _bleLbl;
    static lv_obj_t*          _displayLbl;
    static lv_obj_t*          _gpsLbl;
    static lv_obj_t*          _cpuLbl;
    static lv_obj_t*          _timeLbl;   // estimated time to power off
    static lv_obj_t*          _totalLbl;

    static uint32_t _lastSampleMs;
    static uint32_t _prevTxMs;
    static uint32_t _prevRxMs;

    static constexpr int NUM_PTS = 60;

    // Timescale state — index into TS_SAMPLE_MS[] / TS_LABEL[]
    static int _tsIdx;

    static void _build();
    static void _sample();
    static void _cycleTimescale();
    static void _onHome(lv_event_t* e);
    static void _onKey (lv_event_t* e);
};

}}  // namespace ops::ui
