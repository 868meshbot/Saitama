// Saitama — Regions.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include <cstddef>

namespace ops {
namespace regions {

// ── Saved-regions catalog ────────────────────────────────────────────
// Region scope names the user saved — Settings > Region Scope slots 2-10
// (slot 1 is the default scope, Config::scopeTag), the + next to a channel's
// scope, or a "+ NAME" chip for a discovered region on the Regions screen.
// Nothing is added automatically. Offered wherever a scope is picked.
// Plain names ("AU"); not secret, NVS only. No gaps: indices 0..count()-1.

static constexpr int    MAX_REGIONS = 9;
static constexpr size_t NAME_LEN    = 16;   // incl. NUL, matches Config::scopeTag

void init();
int  count();
const char* get(int i);        // nullptr if out of range
// Adds a name (a leading '#' is dropped; "*", "$..." and empties ignored).
// Ignored when full, so discovery never pushes out a name the user saved.
// Returns true if the list changed.
bool add(const char* name);
// Replaces entry i (i == count() appends). False if the name is invalid,
// already saved elsewhere, or i is out of range / the list is full.
bool set(int i, const char* name);
// Removes entry i; later entries move up.
bool remove(int i);
// Adds every name in a comma-separated list. Returns how many were new.
int  addList(const char* csv);

}  // namespace regions
}  // namespace ops
