// Saitama — RepeaterAdminTools.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Full-screen windows opened from the repeater admin panel (admin login
// needed on the repeater): Change Password, Access Control (the repeater's
// ACL) and Regions (its flood-scope region table). Each is built over the
// active screen and talks to the repeater through admin CLI commands; the
// replies arrive through ScreenRepeaters::onContactResponse() → onResponse().

#pragma once
#include <cstdint>

namespace ops { namespace ui { namespace repadmin {

void openPassword(const uint8_t prefix4[4], const char* name);
void openAccess  (const uint8_t prefix4[4], const char* name);
void openRegions (const uint8_t prefix4[4], const char* name);

// One reply line with the "[Name] " prefix removed. True when a window used it.
bool onResponse(const char* text);

// The admin panel (and with it any open window) is being deleted.
void forget();

}}}  // namespace ops::ui::repadmin
