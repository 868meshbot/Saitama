// Saitama — Sound.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include <stdint.h>

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

    // Play a specific sound choice immediately, bypassing the notifySound gate.
    // Only gated by speakerEnabled. Intended for settings previews.
    void playPreview(uint8_t choice);

    // Returns true while DMA is still draining audio (approximately).
    // Used by the CPU governor to avoid lowering APB below 80 MHz mid-playback.
    bool isPlaying();

    // Play a short tone burst of arbitrary pitch and length.
    // Gated by cfg.speakerEnabled only — NOT by notifySound, because this is a
    // deliberate user-driven tool sound rather than a message notification.
    // freqHz is clamped to 200-3500 Hz (Nyquist at the 8 kHz sample rate),
    // durationMs to 10-200 ms. Returns immediately; audio drains via DMA.
    // Added for the foxhunt proximity beeper, which needs variable rate/pitch.
    void playBeep(uint16_t freqHz, uint16_t durationMs);

    // Play the boot startup jingle (DMG-style rising G-major arpeggio).
    // Gated by cfg.speakerEnabled only — not notifySound.
    // Blocks ~200 ms while queuing samples, then returns; audio finishes in DMA.
    // Call after config::init() and before ui::init().
    void playStartupJingle();

}  // namespace sound
}  // namespace ops
