// Saitama — cardputer/KeyNav.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Keyboard navigation for boards without a touchscreen. Many shared screens
// build list rows and popup buttons for touch only (outside the LVGL input
// group). KeyNav moves a highlight over every clickable object of the top
// layer — the newest full-screen overlay, else the screen — in reading
// order; Enter clicks it. When the focused group widget sits in that layer
// and is not a text box, the normal group navigation is left alone.

#pragma once

namespace ops { namespace ui { namespace keynav {

// Arrow keys. True when KeyNav moved the highlight (the caller then skips
// the group navigation).
bool handleMove(int dx, int dy);

// Tab: the next text box or clickable object of the top layer, in reading
// order, wrapping. A text box gets the group focus (typing goes there).
bool handleTab();

// Enter on a drop-down: opens a single-column button list of its options
// (Enter picks, Esc cancels). False when obj is not a drop-down.
bool openDropdown(struct _lv_obj_t* obj);

// Enter / G0. True when a highlighted object was clicked.
bool handleEnter();

// Esc. True when it closed the top overlay (its Close/Cancel button, or an
// Esc key event sent to it). With backspace = true (Backspace pressed) it
// only acts when the focused widget is not inside that overlay, so
// Backspace still deletes text in a dialog's text box.
bool handleEsc(bool backspace = false);

// Called every UI frame: a popup box taller than the screen is cut to the
// screen height and made scrollable (the highlight scrolls it).
void fitTopLayer();

// Called every UI frame: hides the Home and Close buttons of the screen's
// title bar (Esc does that, and the bar is narrow).
void trimTopBars();

// Esc on a screen: clicks the (hidden) Home or Close button of its title
// bar. True when one was found.
bool clickTopBarBack();

// Typing drops the highlight so Enter goes back to the focused text box.
void clear();

}}}  // namespace ops::ui::keynav
