// Saitama — Regions.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include <cstddef>

namespace ops {
namespace regions {

// ── Saved-regions catalog ────────────────────────────────────────────
// Region scope names seen or used on this node — discovered from nearby
// repeaters, or saved as a default/channel scope — offered as suggestions
// wherever a scope is picked. Plain names ("AU"); not secret, NVS only.

static constexpr int    MAX_REGIONS = 16;
static constexpr size_t NAME_LEN    = 16;   // incl. NUL, matches Config::scopeTag

void init();
int  count();
const char* get(int i);        // nullptr if out of range
// Adds a name (a leading '#' is dropped; "*", "$..." and empties ignored).
// Oldest entry drops out when full. Returns true if the list changed.
bool add(const char* name);
// Adds every name in a comma-separated list. Returns how many were new.
int  addList(const char* csv);

}  // namespace regions
}  // namespace ops
