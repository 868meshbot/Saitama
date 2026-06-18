// Saitama — Emoji.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// LVGL imgfont adapter for emoji rendering.
//
// Constraint: on this build the Montserrat font descriptors live in flash
// (DROM, 0x3C……) which is read-only — writing their `fallback` field via
// const_cast faults or corrupts the flash cache.  So we never touch them.
//
// Solution: emojiFont(base) returns a heap-allocated *copy* of the Montserrat
// struct (writable RAM; its glyph data still points at flash, which is fine to
// read) with `fallback` set to a shared emoji imgfont.  The real Montserrat is
// the primary font (robust glyph rendering); the imgfont only supplies emoji:
//
//     montserrat copy (ASCII/Latin) → emoji imgfont → placeholder □
//
// Copies are cached per base, so we allocate at most once per Montserrat size.

#include "Emoji.h"
#include "emoji/emoji_data.h"

#include <lvgl.h>
#include <cstring>

namespace ops {
namespace emoji {

// imgfont callback: called by LVGL for codepoints not in the parent font.
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

static lv_font_t* s_emoji = nullptr;  // shared emoji imgfont (fallback)

// Cache of base→wrapped copies so each Montserrat size is copied only once.
static const int        kCacheMax       = 6;
static const lv_font_t* s_base[kCacheMax]    = {0};
static lv_font_t*       s_wrapped[kCacheMax] = {0};

void init()
{
    // Build the shared emoji imgfont once.  emojiFont() chains it as a fallback.
    s_emoji = lv_imgfont_create(16, _pathCb);
    if (s_emoji) s_emoji->base_line = 0;
}

const lv_font_t* emojiFont(const lv_font_t* montserratBase)
{
    if (!montserratBase) return nullptr;
    if (!s_emoji)        return montserratBase;  // imgfont unavailable

    for (int i = 0; i < kCacheMax; i++) {
        if (s_base[i] == montserratBase) return s_wrapped[i];
    }

    // Heap copy of the Montserrat struct so we can set its fallback in RAM.
    // The copied dsc/glyph pointers still reference flash — read-only access,
    // which is safe; we only write the struct's own fallback field.
    lv_font_t* copy = (lv_font_t*)lv_malloc(sizeof(lv_font_t));
    if (!copy) return montserratBase;  // graceful degradation
    memcpy(copy, montserratBase, sizeof(lv_font_t));
    copy->fallback = s_emoji;

    for (int i = 0; i < kCacheMax; i++) {
        if (!s_base[i]) { s_base[i] = montserratBase; s_wrapped[i] = copy; break; }
    }
    return copy;
}

}  // namespace emoji
}  // namespace ops
