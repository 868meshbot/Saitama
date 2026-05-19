// Saitama — Keymap.cpp
// Copyright 2026 Saitama — MIT License
//
// T9-style accent cycling: press a letter key repeatedly within CYCLE_MS to
// step through accented variants. Each press after the first sends a backspace
// to erase the previous char before inserting the next. On the final variant,
// the next press wraps back to the base letter.
//
// Layout position-remap is applied first, so the user always thinks in their
// native layout (French user presses physical-Q, gets logical-A, then à, â…).

#include "Keymap.h"
#include <cstring>

namespace ops {
namespace keymap {

const char* kLayoutNames = "English (Default)\nFrench AZERTY\nGerman QWERTZ";

static constexpr uint32_t CYCLE_MS = 500;  // ms window to accept a follow-up press

// Physical QWERTY → logical letter maps (index = raw_letter - 'a').
static const char kMap[LAYOUT_COUNT][26] = {
    // EN — identity
    { 'a','b','c','d','e','f','g','h','i','j','k','l','m',
      'n','o','p','q','r','s','t','u','v','w','x','y','z' },
    // FR AZERTY: physical q→a, a→q, w→z, z→w
    { 'q','b','c','d','e','f','g','h','i','j','k','l','m',
      'n','o','p','a','r','s','t','u','v','z','x','y','w' },
    // DE QWERTZ: physical y→z, z→y
    { 'a','b','c','d','e','f','g','h','i','j','k','l','m',
      'n','o','p','q','r','s','t','u','v','w','x','z','y' },
};

struct CycleRow {
    char        base;   // logical base letter (lowercase)
    const char* lo;     // null-terminated cycle string: base + accented variants (lowercase)
    const char* hi;     // null-terminated cycle string: base + accented variants (uppercase)
};

// French AZERTY accent cycles.
static const CycleRow kCycleFR[] = {
    { 'e', "e\xE9\xE8\xEA\xEB", "E\xC9\xC8\xCA\xCB" },  // e é è ê ë
    { 'a', "a\xE0\xE2",         "A\xC0\xC2"          },  // a à â
    { 'u', "u\xF9\xFB\xFC",     "U\xD9\xDB\xDC"      },  // u ù û ü
    { 'i', "i\xEE\xEF",         "I\xCE\xCF"          },  // i î ï
    { 'o', "o\xF4",             "O\xD4"              },  // o ô
    { 'c', "c\xE7",             "C\xC7"              },  // c ç
};
static constexpr int kCycleFRLen = (int)(sizeof(kCycleFR)/sizeof(kCycleFR[0]));

// German QWERTZ accent cycles.
static const CycleRow kCycleDE[] = {
    { 'a', "a\xE4", "A\xC4" },  // a ä
    { 'o', "o\xF6", "O\xD6" },  // o ö
    { 'u', "u\xFC", "U\xDC" },  // u ü
    { 's', "s\xDF", "S\xDF" },  // s ß (no uppercase form)
};
static constexpr int kCycleDELen = (int)(sizeof(kCycleDE)/sizeof(kCycleDE[0]));

// Cycle state.
static char     s_pendingBase  = '\0';
static bool     s_pendingUpper = false;
static int      s_cycleIdx     = 0;
static uint32_t s_cycleMs      = 0;
static uint8_t  s_cycleLayout  = 0xFF;

static void _clearCycle()
{
    s_pendingBase  = '\0';
    s_pendingUpper = false;
    s_cycleIdx     = 0;
    s_cycleMs      = 0;
    s_cycleLayout  = 0xFF;
}

static char _remap(char raw, uint8_t layout)
{
    if (raw >= 'a' && raw <= 'z') return kMap[layout][(uint8_t)(raw - 'a')];
    if (raw >= 'A' && raw <= 'Z')
        return (char)(kMap[layout][(uint8_t)(raw - 'A')] - 'a' + 'A');
    return raw;
}

KeyOut translate(char raw, uint8_t layout, uint32_t nowMs)
{
    // Reject hardware-injected extended bytes (e.g. BBQ10 Shift+0 → 0xE0).
    // Our accented outputs are only ever in the return value, never the input.
    if ((uint8_t)raw > 127) {
        _clearCycle();
        return {'\0', false};
    }

    // EN layout: no remapping or cycling.
    if (layout == LAYOUT_EN || layout >= LAYOUT_COUNT) {
        _clearCycle();
        return {raw, false};
    }

    char mapped  = _remap(raw, layout);
    bool isUpper = (mapped >= 'A' && mapped <= 'Z');
    bool isLower = (mapped >= 'a' && mapped <= 'z');

    // Non-letter: clear cycle state and pass through.
    if (!isLower && !isUpper) {
        _clearCycle();
        return {mapped, false};
    }

    char baseLo = isUpper ? (char)(mapped - 'A' + 'a') : mapped;

    // Look up cycle row.
    const CycleRow* table    = nullptr;
    int             tableLen = 0;
    if (layout == LAYOUT_FR) { table = kCycleFR; tableLen = kCycleFRLen; }
    else if (layout == LAYOUT_DE) { table = kCycleDE; tableLen = kCycleDELen; }

    const CycleRow* row = nullptr;
    for (int i = 0; i < tableLen; i++) {
        if (table[i].base == baseLo) { row = &table[i]; break; }
    }

    // Letter has no accent cycle — return remapped char and clear state.
    if (!row) {
        _clearCycle();
        return {mapped, false};
    }

    const char* cycleStr = isUpper ? row->hi : row->lo;
    int         cycleLen = (int)strlen(cycleStr);

    bool continuing = (s_pendingBase  == baseLo  &&
                       s_cycleLayout  == layout   &&
                       s_pendingUpper == isUpper  &&
                       s_cycleMs      > 0         &&
                       (nowMs - s_cycleMs) < CYCLE_MS);

    if (continuing) {
        s_cycleIdx = (s_cycleIdx + 1) % cycleLen;
        s_cycleMs  = nowMs;
        return {cycleStr[s_cycleIdx], true};  // replace = true: caller sends BACKSPACE first
    } else {
        s_pendingBase  = baseLo;
        s_pendingUpper = isUpper;
        s_cycleIdx     = 0;
        s_cycleMs      = nowMs;
        s_cycleLayout  = layout;
        return {cycleStr[0], false};  // first press, no replace needed
    }
}

}  // namespace keymap
}  // namespace ops
