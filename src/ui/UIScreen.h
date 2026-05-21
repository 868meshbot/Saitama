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

// Launch the 2-point touch calibration routine.
// Creates an LVGL overlay; tap the crosshair twice; coefficients are saved to Config.
// Backspace/ESC cancels. Called from the /touch-cali terminal command.
void startTouchCalibration();

}}  // namespace ops::ui