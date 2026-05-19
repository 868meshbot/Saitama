// Saitama — Keymap.h
// Copyright 2026 Saitama — MIT License

#pragma once
#include <cstdint>

namespace ops {
namespace keymap {

static constexpr uint8_t LAYOUT_EN    = 0;  // English QWERTY (default)
static constexpr uint8_t LAYOUT_FR    = 1;  // French AZERTY
static constexpr uint8_t LAYOUT_DE    = 2;  // German QWERTZ
static constexpr uint8_t LAYOUT_COUNT = 3;

// Newline-separated names for the settings dropdown.
extern const char* kLayoutNames;

// Result of a single translate() call.
struct KeyOut {
    char ch;      // character to emit (0 = nothing to send)
    bool replace; // true: send LV_KEY_BACKSPACE before ch (cycling replaced previous char)
};

// Translate one raw BBQ10 keypress.
// - Applies positional remap (AZERTY/QWERTZ) then cycles through accented variants
//   when the same key is pressed repeatedly within CYCLE_MS.
// - Call with the current millis() timestamp so cycle timeouts work correctly.
// - Only remaps letter keys; digits, symbols, control chars pass through unchanged.
KeyOut translate(char raw, uint8_t layout, uint32_t nowMs);

}  // namespace keymap
}  // namespace ops
