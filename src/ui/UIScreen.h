// Saitama — UIScreen.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Top-level UI controller. Owns the LVGL display driver and
// manages which screen is active.  All screens are defined in
// the ui/ directory; this header provides the init/tick entry
// points called from main.cpp.

#pragma once

#include "../features.h"

namespace ops { namespace ui {

void init();
void tick();

// Arm (true) or clear (false) the user light-sleep wake pins. tick() arms
// them only around each light sleep; setup() clears them first thing, since
// a panic restart leaves the previous run's GPIO setup in place.
void armWakePins(bool on);

// Navigate to the main launcher from any screen (back / home action).
void showLauncher();

// Force the keyboard-backlight auto/manual logic to re-apply on the next tick
// (call after a live brightness preview in Settings so the auto state reasserts).
void resetKbBacklight();

// Apply the CPU frequency for the current governor and display state.
// Call after changing cfg.cpuGovernor so the new setting takes effect immediately.
void applyGovernorNow();

// Activate the screensaver immediately. Pass analog=true for the analog clock face.
// Any input will dismiss it as usual. Called from the /clock terminal command.
void activateScreensaver(bool analog = false);

// Show the Saitama screensaver (BootAnim) now; a trackball click dismisses
// it. Called from the /saitama-scr terminal command. The idle screensaver uses
// it too when Settings > Screensaver is "Saitama".
void activateSaitamaScr();

// Launch the 2-point touch calibration routine.
// Creates an LVGL overlay; tap the crosshair twice; coefficients are saved to Config.
// Backspace/ESC cancels. Called from the /touch-cali terminal command.
void startTouchCalibration();

// Render the current LVGL screen to an image file on the SD card: PNG with
// PSRAM, an uncompressed 24-bit BMP without (the PNG encoder and its RGB copy
// don't fit in internal RAM). path should be an absolute SD path such as
// "/screenshot_001.png" — use SCREENSHOT_EXT. Returns true on success.
#if OPS_HAS_PSRAM
static constexpr const char* SCREENSHOT_EXT = ".png";
#else
static constexpr const char* SCREENSHOT_EXT = ".bmp";
#endif
bool takeScreenshot(const char* path);

// No-PSRAM boards: frees the screens that are rebuilt on every visit
// (Settings, Signal, Contacts, Repeaters, Heard, Regions, Chat) unless shown.
void releaseHiddenScreens();

// A short notice at the bottom of the screen, gone after 3 s.
void showNotice(const char* text);

}}  // namespace ops::ui