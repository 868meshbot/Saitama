// Saitama — Emoji.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once

#include <lvgl.h>

namespace ops {
namespace emoji {

// Call once after lv_init() in UIScreen::init().
void init();

// Returns a per-size imgfont whose internal fallback is set to the named
// Montserrat font.  Use this as the label font when you want emoji support.
// Returns nullptr if imgfont is unavailable; callers fall back to plain font.
const lv_font_t* emojiFont(const lv_font_t* montserratBase);

}  // namespace emoji
}  // namespace ops
