// Saitama — Emoji.cpp
// Copyright 2026 Saitama — MIT License
//
// LVGL imgfont adapter for emoji rendering.
//
// Design constraint: LVGL 8 declares its built-in fonts as `const lv_font_t`
// which on ESP32 lands in read-only flash (RODATA).  Writing the `fallback`
// field via const_cast causes a LoadStoreError hard fault.
//
// Solution: keep Montserrat in flash untouched.  When a label needs emoji,
// call emojiFont(base) to get a heap-allocated lv_font_t whose internal
// `fallback` points to the Montserrat base.  LVGL checks the imgfont first
// (emoji), then falls through to Montserrat for normal glyphs.

#include "Emoji.h"
#include "emoji/emoji_data.h"

#include <lvgl.h>
#include <cstring>

namespace ops {
namespace emoji {

// imgfont callback: called by LVGL for every codepoint lookup.
// Writes an lv_img_dsc_t into imgSrc and returns true if an emoji matched.
static bool _pathCb(const lv_font_t* /*font*/, void* imgSrc, uint16_t len,
                    uint32_t unicode, uint32_t /*unicodeNext*/)
{
    if (unicode == 0xFE0F || unicode == 0x200D) return false;  // variation/ZWJ

    for (int i = 0; i < kOpsEmojiCount; i++) {
        if (kOpsEmoji[i].codepoint == unicode) {
            if (len < sizeof(lv_img_dsc_t)) return false;
            memcpy(imgSrc, kOpsEmoji[i].img, sizeof(lv_img_dsc_t));
            return true;
        }
    }
    return false;
}

void init()
{
    // Nothing to do at global init time.
    // emojiFont() creates fonts on first call per size.
}

const lv_font_t* emojiFont(const lv_font_t* montserratBase)
{
    if (!montserratBase) return nullptr;

    // Create a heap-allocated imgfont (writable) and chain Montserrat as its
    // fallback.  LVGL resolves: imgfont (emoji codepoints) → montserrat (ASCII).
    lv_font_t* f = lv_imgfont_create(montserratBase->line_height, _pathCb);
    if (!f) return montserratBase;  // graceful degradation

    f->fallback = montserratBase;  // safe: f is on the heap, not flash
    return f;
}

}  // namespace emoji
}  // namespace ops
