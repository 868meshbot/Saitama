// Saitama — Sound.h
// Copyright 2026 Saitama — MIT License

#pragma once

namespace ops {
namespace sound {

    // Initialise I2S and pre-compute the ping waveform.
    // Call once in setup() after Board::init().
    void init();

    // Play the notification sound selected in cfg.notifySoundChoice.
    // Gated by cfg.speakerEnabled (master) and cfg.notifySound.
    // Returns immediately — audio drains via DMA.
    void playNotification();

    // Play the raw default ping (A5, 150 ms). Use playNotification() at call sites.
    void playPing();

    // Play the boot startup jingle (DMG-style rising G-major arpeggio).
    // Gated by cfg.speakerEnabled only — not notifySound.
    // Blocks ~200 ms while queuing samples, then returns; audio finishes in DMA.
    // Call after config::init() and before ui::init().
    void playStartupJingle();

}  // namespace sound
}  // namespace oms
