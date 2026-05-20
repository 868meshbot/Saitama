// Saitama — UIScreen.h
// Copyright 2026 Saitama — MIT License
//
// Top-level UI controller. Owns the LVGL display driver and
// manages which screen is active.  All screens are defined in
// the ui/ directory; this header provides the init/tick entry
// points called from main.cpp.

#pragma once

namespace ops { namespace ui {

void init();
void tick();

// Navigate to the main launcher from any screen (back / home action).
void showLauncher();

// Apply the CPU frequency for the current governor and display state.
// Call after changing cfg.cpuGovernor so the new setting takes effect immediately.
void applyGovernorNow();

}}  // namespace ops::ui