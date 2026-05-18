// Saitama — Theme.cpp
// Copyright 2026 Saitama — MIT License

#include "Theme.h"

namespace ops { namespace theme {

void apply(lv_disp_t* disp) {
    lv_theme_t* th = lv_theme_default_init(
        disp,
        PRIMARY,       // primary
        ACCENT,        // secondary
        true,          // dark mode
        LV_FONT_DEFAULT
    );
    (void)th;
    // Additional style overrides can be added here
    // e.g. lv_style_set_bg_color for specific widgets
}

}}  // namespace ops::theme