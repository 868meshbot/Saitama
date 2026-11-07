// Saitama — cardputer/MapLite.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Hooks between UIScreen and the Cardputer map (ScreenMapLite.cpp). The map
// draws JPEG tiles straight to the display, outside LVGL, so it needs the
// TFT and has to know when LVGL has painted over it (a popup, the
// screensaver coming back) to draw itself again.

#pragma once

class TFT_eSPI;

namespace ops { namespace ui {

TFT_eSPI& directTft();      // UIScreen.cpp's display driver

namespace maplite {
void noteLvglFlush();       // LVGL just flushed an area (called from flush_cb)
}

}}  // namespace ops::ui
