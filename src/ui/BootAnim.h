// Saitama — BootAnim.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Animated boot-screen concept: a sparse mesh of nodes that drift slowly,
// with a stylised "S" packet routing node to node along the links.
// Used as the Saitama screensaver (Settings > Screensaver, or /saitama-scr in
// the terminal); not yet the boot screen.

#pragma once
#include <lvgl.h>

namespace ops {
namespace bootanim {

// Builds the animation on parent (expects a full-screen, black object) and
// starts its LVGL timer. Call destroy() before deleting parent.
void create(lv_obj_t* parent);

// Stops the timer and forgets the objects (they die with their parent).
void destroy();

}  // namespace bootanim
}  // namespace ops
