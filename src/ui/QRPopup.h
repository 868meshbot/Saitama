// Saitama — QRPopup.h
// Copyright 2026 Saitama — GPL-3.0-or-later
#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

// Show a dim-overlay QR popup on the active screen.
// data is encoded at QR version 6 (ECC_LOW, up to 134 bytes).
// title: label shown above the QR (max ~40 chars).
// Tap overlay or press Close to dismiss.
void showQrPopup(const char* title, const char* data);

}}  // namespace ops::ui
