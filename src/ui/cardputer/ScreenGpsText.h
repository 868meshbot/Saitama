// Saitama — cardputer/ScreenGpsText.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// GPS status as text (the T-Deck's skyplot does not fit 240x135): mode, fix,
// satellites, HDOP, position, altitude, UTC time and clock sync. Refreshes
// every second while shown. Esc / Backspace returns to the launcher.

#pragma once

namespace ops { namespace ui {

class ScreenGpsText {
public:
    static void show();
};

}}  // namespace ops::ui
