// Saitama — ScreenZeroXZero.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// 0x0 — Tic-Tac-Toe game screen.
// Player is X; device plays O with minimax + 20% blunder.
// Touch: tap a cell to play.  Keyboard: N=new game, M=toggle AI/2P, Bksp=back.
// Scores persisted to NVS namespace "games".

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenZeroXZero {
public:
    static void show();

private:
    static lv_obj_t* _screen;

    static void _build();

    static void _onCellClick(lv_event_t* e);
    static void _onKey      (lv_event_t* e);
    static void _onHomeClick(lv_event_t* e);
    static void _onModeClick(lv_event_t* e);
    static void _onNewClick (lv_event_t* e);
};

}}  // namespace ops::ui
