// Saitama — MP3Player.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once

namespace ops {
namespace mp3player {

    enum class State { Idle, Playing, Paused, Stopping, Done, Error };

    // Start playing an MP3 file from the given SD-absolute path.
    // Spawns a FreeRTOS task on Core 0.  Returns false if already playing or
    // file not found.
    bool play(const char* sdPath);

    // Pause / resume — only valid while Playing/Paused.
    void pause();
    void resume();

    // Signal the task to stop; non-blocking — state becomes Idle shortly after.
    void stop();

    State       state();
    const char* filename();   // basename of current/last file
    float       progress();   // 0.0–1.0 based on byte offset

}  // namespace mp3player
}  // namespace ops
