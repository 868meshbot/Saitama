// Saitama — Theme.h
// Copyright 2026 Saitama — MIT License
//
// Dark theme inspired by MeshOS aesthetics but cleaner.
// All colour constants live here so the UI stays consistent.

#pragma once

#include <lvgl.h>

namespace ops { namespace theme {

// ── Colour palette ──────────────────────────────────────────────────
static constexpr lv_color_t BG          = LV_COLOR_MAKE(13, 17, 23);    // #0d1117
static constexpr lv_color_t BG_CARD     = LV_COLOR_MAKE(22, 27, 34);   // #161b22
static constexpr lv_color_t TEXT        = LV_COLOR_MAKE(230, 237, 243); // #e6edf3
static constexpr lv_color_t TEXT_MUTED  = LV_COLOR_MAKE(139, 148, 158); // #8b949e
static constexpr lv_color_t ACCENT      = LV_COLOR_MAKE(88, 166, 255);  // #58a6ff
static constexpr lv_color_t PRIMARY     = LV_COLOR_MAKE(0, 51, 170);    // #0033AA
static constexpr lv_color_t GREEN       = LV_COLOR_MAKE(63, 185, 80);   // #3fb950
static constexpr lv_color_t RED         = LV_COLOR_MAKE(248, 81, 73);   // #f85149
static constexpr lv_color_t ORANGE      = LV_COLOR_MAKE(210, 153, 34);  // #d29922
static constexpr lv_color_t BORDER     = LV_COLOR_MAKE(48, 54, 61);    // #30363d

// ── Apply theme to display ──────────────────────────────────────────
void apply(lv_disp_t* disp);

}}  // namespace ops::theme