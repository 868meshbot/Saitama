// Saitama — Theme.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Runtime colour palette. Call applyTheme() before ui::init() so that all
// subsequent LVGL style calls read the correct colours.

#pragma once

#include <lvgl.h>
#include <stdint.h>

namespace ops { namespace theme {

// ── Colour palette ──────────────────────────────────────────────────
// Populated by applyTheme(); read-only after that.
extern lv_color_t BG;
extern lv_color_t BG_CARD;
extern lv_color_t TEXT;
extern lv_color_t TEXT_MUTED;
extern lv_color_t ACCENT;
extern lv_color_t PRIMARY;
extern lv_color_t GREEN;
extern lv_color_t RED;
extern lv_color_t ORANGE;
extern lv_color_t BORDER;

// ── Theme selection ─────────────────────────────────────────────────
// 0 = Default (dark blue/github-inspired)
// 1 = Green   (blue and green roles swapped)
// 2 = Dracula (Dracula colour scheme)
static constexpr int THEME_COUNT = 12;
void applyTheme(uint8_t choice);

// ── Apply LVGL default theme to the display ─────────────────────────
void apply(lv_display_t* disp);

// ── Font helpers ────────────────────────────────────────────────────
// Return the appropriate body font for the current fontExtLatin setting.
// Extended variants cover U+0020-U+017E (Basic Latin + Latin-1 Supplement
// + Latin Extended-A), enabling accented chars for most European languages.
const lv_font_t* bodyFont10();  // replaces &lv_font_montserrat_10 in message areas
const lv_font_t* bodyFont12();  // replaces &lv_font_montserrat_12 in message areas

}}  // namespace ops::theme
