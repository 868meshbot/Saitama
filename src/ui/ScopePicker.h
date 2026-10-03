// Saitama — ScopePicker.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Region-scope picking shared by Settings > Channels, Chat's Add Channel and
// Chat's Configure Scope: a dropdown of "Default (X)" + the saved regions,
// and a + button that saves a new region (ops::regions) and selects it.

#pragma once
#include <lvgl.h>
#include <cstddef>

namespace ops { namespace ui { namespace scopepick {

// Checks a typed region name. On success fills out with the plain name ("" for
// empty or "*" = unscoped) and returns nullptr; otherwise returns why not.
const char* nameError(const char* txt, char* out, size_t outMax);

// Fills dd: "Default (X)" (= empty scope: follows Settings > Region Scope 1),
// every saved region, and `current` if it isn't saved — selecting `current`.
void fill(lv_obj_t* dd, const char* current);

// The choice in dd as a scope: "" for Default, else the region name.
void selected(lv_obj_t* dd, char* out, size_t outMax);

// Builds a [dropdown][+] row in parent, filled for `current`. With inGroup,
// both go in the default input group (keyboard/trackball). Returns the
// dropdown; addBtnOut (optional) receives the + button.
lv_obj_t* createRow(lv_obj_t* parent, const char* current, bool inGroup,
                    lv_obj_t** addBtnOut = nullptr);

// Opens the "New Scope" box over the current screen; on Add the name is
// saved and selected in dd.
void openNewScope(lv_obj_t* dd);

}}}  // namespace ops::ui::scopepick
