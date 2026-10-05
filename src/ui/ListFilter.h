// Saitama — ListFilter.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Type-to-filter box for list screens (Contacts, Repeaters): a textarea at
// the right of the top bar that hides list rows not matching its text.
// Rows opt in with setRowKey(); their match text is supplied by the screen.

#pragma once
#include <lvgl.h>
#include <cstddef>

namespace ops { namespace ui { namespace listfilter {

struct State {
    lv_obj_t* ta   = nullptr;
    lv_obj_t* list = nullptr;
    char      text[24] = {};   // kept across list rebuilds
    // Fills out with the searchable text for row key `key` (name, id…).
    void (*rowText)(int key, char* out, size_t outMax) = nullptr;
    void (*onEsc)() = nullptr;  // Esc / Backspace on an empty box
};

// Adds the box to the end of `bar` (a flex row), focused for typing.
lv_obj_t* create(lv_obj_t* bar, State& st);
// Tags a list row with its key (>= 0); untagged children are never hidden.
void setRowKey(lv_obj_t* row, int key);
// Shows/hides st.list's rows for the current text.
void apply(State& st);

}}}  // namespace ops::ui::listfilter
