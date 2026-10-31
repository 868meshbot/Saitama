// Saitama — cardputer/MeshRain.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// The Cardputer's "Mesh rain" screensaver (screensaver style 1). It
// implements the bootanim API (BootAnim.h), so UIScreen starts and stops it
// exactly like the T-Deck's Saitama animation. At most 2 channel names and
// 3 station names fall at once, each picked at random; a new message pulls
// its sender into view and joins it to the channel with a fading line.

#pragma once

namespace ops { namespace meshrain {

// From the launcher's message hooks. channel = nullptr for a DM.
void noteMessage(const char* sender, const char* channel);

}}  // namespace ops::meshrain
