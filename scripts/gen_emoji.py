#!/usr/bin/env python3
"""
Generate LVGL C image arrays for a curated emoji set from jdecked/twemoji 16.0.1
(community-maintained fork of Twitter Twemoji; Unicode Emoji 16.0).

Output: one .c file per emoji + emoji_data.h in src/ui/emoji/
Format: LV_IMG_CF_TRUE_COLOR_ALPHA  →  [RGB565_lo, RGB565_hi, alpha] per pixel

Run from the project root:  python3 scripts/gen_emoji.py
"""

import os
import struct
import urllib.request
from PIL import Image
import io

SIZE = 16  # pixels (fits within montserrat_14 line_height=18)

# (symbol_name, primary_codepoint, filename_on_CDN, search_keywords)
#
# For multi-codepoint emoji (flags, sequences) the primary_codepoint is the
# FIRST codepoint LVGL will look up.  The remainder of the sequence is ignored
# by the imgfont system — the second codepoint renders as nothing from the
# fallback font, which is acceptable at this screen size.
EMOJI = [
    # ── Face Smiling ──────────────────────────────────────────────────────────
    ("grinning",      0x1F600, "1f600", "grinning happy smile"),
    ("big_smile",     0x1F603, "1f603", "big smile happy grin"),
    ("grin",          0x1F604, "1f604", "grin happy smile eyes"),
    ("beam",          0x1F601, "1f601", "beaming smile happy"),
    ("squint",        0x1F606, "1f606", "squinting laugh happy"),
    ("sweat_smile",   0x1F605, "1f605", "sweat smile nervous happy"),
    ("rofl",          0x1F923, "1f923", "rofl rolling floor laughing"),
    ("joy",           0x1F602, "1f602", "joy laugh cry tears happy"),
    ("smile",         0x1F642, "1f642", "smile slightly happy"),
    ("upside_down",   0x1F643, "1f643", "upside down smile silly"),
    ("wink",          0x1F609, "1f609", "wink flirty smile"),
    ("blush",         0x1F60A, "1f60a", "blush smile happy warm"),
    ("halo",          0x1F607, "1f607", "halo angel innocent smile"),
    # ── Face Affection ───────────────────────────────────────────────────────
    ("heart_face",    0x1F970, "1f970", "heart face love smiling"),
    ("heart_eyes",    0x1F60D, "1f60d", "heart eyes love smiling"),
    ("star_struck",   0x1F929, "1f929", "star struck eyes amazed wow"),
    ("kiss_heart",    0x1F618, "1f618", "kiss heart love"),
    ("kiss",          0x1F617, "1f617", "kiss"),
    ("kiss_closed",   0x1F61A, "1f61a", "kiss closed eyes"),
    ("smile_tear",    0x1F972, "1f972", "smile tear grateful happy sad"),
    # ── Face Tongue ──────────────────────────────────────────────────────────
    ("yum",           0x1F60B, "1f60b", "yum tasty tongue food"),
    ("tongue",        0x1F61B, "1f61b", "tongue silly playful"),
    ("wink_tongue",   0x1F61C, "1f61c", "wink tongue cheeky"),
    ("crazy",         0x1F92A, "1f92a", "crazy zany wild"),
    ("squint_tongue", 0x1F61D, "1f61d", "squint tongue silly"),
    ("money_mouth",   0x1F911, "1f911", "money mouth rich greedy"),
    # ── Face Hand ────────────────────────────────────────────────────────────
    ("hug",           0x1F917, "1f917", "hug hugging warm"),
    ("hand_mouth",    0x1F92D, "1f92d", "hand over mouth surprised"),
    ("shush",         0x1F92B, "1f92b", "shush quiet secret"),
    ("think",         0x1F914, "1f914", "thinking hmm ponder"),
    # ── Face Neutral / Skeptical ─────────────────────────────────────────────
    ("zipper",        0x1F910, "1f910", "zipper mouth quiet secret"),
    ("woozy",         0x1F974, "1f974", "woozy drunk dizzy"),
    ("neutral",       0x1F610, "1f610", "neutral flat blank"),
    ("expressionless",0x1F611, "1f611", "expressionless blank meh"),
    ("no_mouth",      0x1F636, "1f636", "no mouth silent blank"),
    ("smirk",         0x1F60F, "1f60f", "smirk smug confident"),
    ("unamused",      0x1F612, "1f612", "unamused annoyed unimpressed"),
    ("eye_roll",      0x1F644, "1f644", "eye roll bored annoyed"),
    ("grimace",       0x1F62C, "1f62c", "grimace awkward nervous teeth"),
    ("lying",         0x1F925, "1f925", "lying pinocchio nose"),
    # ── Face Sleepy ──────────────────────────────────────────────────────────
    ("sleeping",      0x1F634, "1f634", "sleeping tired zzz"),
    ("drool",         0x1F924, "1f924", "drooling sleep hungry"),
    ("sleepy",        0x1F62A, "1f62a", "sleepy tired yawn"),
    # ── Face Unwell ──────────────────────────────────────────────────────────
    ("sick",          0x1F912, "1f912", "sick thermometer ill fever"),
    ("hurt",          0x1F915, "1f915", "hurt injured bandage"),
    ("nausea",        0x1F922, "1f922", "nausea sick green ill"),
    ("vomit",         0x1F92E, "1f92e", "vomit sick ill"),
    ("sneeze",        0x1F927, "1f927", "sneeze sick cold"),
    ("hot",           0x1F975, "1f975", "hot sweating overheated"),
    ("cold",          0x1F976, "1f976", "cold freezing ice"),
    ("dizzy_face",    0x1F635, "1f635", "dizzy face spiral woozy"),
    ("exploding",     0x1F92F, "1f92f", "exploding head mind blown"),
    ("yawn",          0x1F971, "1f971", "yawn tired bored"),
    # ── Face Hat ─────────────────────────────────────────────────────────────
    ("cowboy",        0x1F920, "1f920", "cowboy hat western"),
    ("party",         0x1F973, "1f973", "party celebrate hat"),
    ("disguise",      0x1F978, "1f978", "disguise incognito glasses"),
    # ── Face Glasses ─────────────────────────────────────────────────────────
    ("sunglasses",    0x1F60E, "1f60e", "sunglasses cool shades"),
    ("nerd",          0x1F913, "1f913", "nerd glasses smart"),
    ("monocle",       0x1F9D0, "1f9d0", "monocle thinking smart"),
    # ── Face Concerned ───────────────────────────────────────────────────────
    ("confused",      0x1F615, "1f615", "confused lost uncertain"),
    ("worried",       0x1F61F, "1f61f", "worried anxious nervous"),
    ("frown",         0x1F641, "1f641", "frown sad unhappy"),
    ("open_mouth",    0x1F62E, "1f62e", "open mouth surprised"),
    ("hushed",        0x1F62F, "1f62f", "hushed surprised quiet"),
    ("astonished",    0x1F632, "1f632", "astonished shocked wow"),
    ("flushed",       0x1F633, "1f633", "flushed embarrassed red"),
    ("plead",         0x1F97A, "1f97a", "pleading eyes puppy beg"),
    ("fearful",       0x1F628, "1f628", "fearful scared afraid"),
    ("anxious_sweat", 0x1F630, "1f630", "anxious sweat nervous"),
    ("sad_relief",    0x1F625, "1f625", "sad relieved sweat"),
    ("cry",           0x1F622, "1f622", "cry tear sad"),
    ("sob",           0x1F62D, "1f62d", "sob cry tears"),
    ("scream",        0x1F631, "1f631", "scream scared horror"),
    ("confounded",    0x1F616, "1f616", "confounded stressed fail"),
    ("persevere",     0x1F623, "1f623", "persevering struggle"),
    ("disappoint",    0x1F61E, "1f61e", "disappointed sad"),
    ("downcast",      0x1F613, "1f613", "downcast sweat relieved"),
    ("weary",         0x1F629, "1f629", "weary tired exhausted"),
    ("tired",         0x1F62B, "1f62b", "tired exhausted"),
    # ── Face Negative ────────────────────────────────────────────────────────
    ("triumph",       0x1F624, "1f624", "triumph puffing steam angry"),
    ("rage",          0x1F621, "1f621", "rage angry pouting"),
    ("angry",         0x1F620, "1f620", "angry mad"),
    ("swearing",      0x1F92C, "1f92c", "swearing angry cursing"),
    ("devil",         0x1F608, "1f608", "devil smiling evil"),
    ("imp",           0x1F47F, "1f47f", "imp devil evil angry"),
    ("skull",         0x1F480, "1f480", "skull dead bones"),
    ("crossbones",    0x2620,  "2620", "skull crossbones danger"),
    # ── Face Costume ─────────────────────────────────────────────────────────
    ("clown",         0x1F921, "1f921", "clown joker"),
    ("ogre",          0x1F479, "1f479", "ogre monster"),
    ("goblin",        0x1F47A, "1f47a", "goblin devil red"),
    ("poop",          0x1F4A9, "1f4a9", "poop pile smiling"),
    ("ghost",         0x1F47B, "1f47b", "ghost boo spooky"),
    ("alien",         0x1F47D, "1f47d", "alien ufo extraterrestrial"),
    ("space_invader", 0x1F47E, "1f47e", "space invader game alien"),
    ("robot",         0x1F916, "1f916", "robot machine"),
    # ── Cat Faces ────────────────────────────────────────────────────────────
    ("cat_smile",     0x1F638, "1f638", "cat smile happy"),
    ("cat_joy",       0x1F639, "1f639", "cat joy laugh tears"),
    ("cat_heart",     0x1F63B, "1f63b", "cat heart eyes love"),
    ("cat_smirk",     0x1F63C, "1f63c", "cat smirk wry"),
    ("cat_cry",       0x1F63F, "1f63f", "cat cry sad"),
    ("cat_pout",      0x1F63E, "1f63e", "cat pouting angry"),
    # ── People & Hands ───────────────────────────────────────────────────────
    ("thumbsup",      0x1F44D, "1f44d", "thumbs up good yes"),
    ("thumbsdown",    0x1F44E, "1f44e", "thumbs down no bad"),
    ("wave",          0x1F44B, "1f44b", "wave hand hello bye"),
    ("pray",          0x1F64F, "1f64f", "pray hands please thanks"),
    ("clap",          0x1F44F, "1f44f", "clap applause bravo"),
    ("ok_hand",       0x1F44C, "1f44c", "ok hand perfect"),
    ("muscle",        0x1F4AA, "1f4aa", "muscle strong flex"),
    # ── Hearts & Symbols ─────────────────────────────────────────────────────
    ("heart",         0x2764,  "2764",  "heart love red"),
    ("orange_heart",  0x1F9E1, "1f9e1", "orange heart"),
    ("yellow_heart",  0x1F49B, "1f49b", "yellow heart"),
    ("green_heart",   0x1F49A, "1f49a", "green heart"),
    ("blue_heart",    0x1F499, "1f499", "blue heart"),
    ("purple_heart",  0x1F49C, "1f49c", "purple heart"),
    ("broken_heart",  0x1F494, "1f494", "broken heart sad"),
    ("fire",          0x1F525, "1f525", "fire hot flame"),
    ("sparkles",      0x2728,  "2728",  "sparkles stars shiny"),
    ("star",          0x2B50,  "2b50",  "star gold"),
    ("tada",          0x1F389, "1f389", "tada party celebrate"),
    ("dizzy",         0x1F4AB, "1f4ab", "dizzy stars swirl"),
    ("100",           0x1F4AF, "1f4af", "100 perfect score"),
    ("boom",          0x1F4A5, "1f4a5", "boom explosion"),
    ("check",         0x2705,  "2705",  "check done yes green"),
    ("cross",         0x274C,  "274c",  "cross no cancel red"),
    ("exclamation",   0x2757,  "2757",  "exclamation important"),
    ("question",      0x2753,  "2753",  "question ask help"),
    ("zzz",           0x1F4A4, "1f4a4", "zzz sleep tired"),
    # ── Objects & Nature (commonly used in chat) ──────────────────────────────
    ("frog",          0x1F438, "1f438", "frog green"),
    ("dog",           0x1F415, "1f415", "dog puppy"),
    ("satellite",     0x1F4E1, "1f4e1", "satellite dish radio antenna"),
    ("plane",         0x2708,  "2708",  "plane airplane travel"),
    ("car",           0x1F697, "1f697", "car vehicle"),
    ("moto",          0x1F3CD, "1f3cd", "motorcycle bike"),
    ("ship",          0x1F6A2, "1f6a2", "ship boat sea"),
    ("palm",          0x1F334, "1f334", "palm tree beach"),
    ("house",         0x1F3E0, "1f3e0", "house home"),
    ("house2",        0x1F3E1, "1f3e1", "house garden home"),
    ("robot_face",    0x1F916, "1f916", "robot machine face"),  # duplicate guard handled below
    # ── Flags ─────────────────────────────────────────────────────────────────
    ("wales",         0x1F3F4, "1f3f4-e0067-e0062-e0077-e006c-e0073-e007f", "wales flag"),
    ("ireland",       0x1F1EE, "1f1ee-1f1ea", "ireland flag"),
    ("us_flag",       0x1F1FA, "1f1fa-1f1f8", "usa flag united states"),
]

# Deduplicate by codepoint — keep first occurrence
seen = {}
EMOJI_DEDUP = []
for entry in EMOJI:
    cp = entry[1]
    if cp not in seen:
        seen[cp] = True
        EMOJI_DEDUP.append(entry)
EMOJI = EMOJI_DEDUP

# Downloaded but NOT registered in the lookup table (codepoint collision).
EMOJI_EXTRA = [
    ("england", "1f3f4-e0067-e0062-e0065-e006e-e0067-e007f"),  # 🏴󠁧󠁢󠁥󠁮󠁧󠁿 England (shares U+1F3F4 with Wales above)
]

CDN = "https://cdn.jsdelivr.net/gh/jdecked/twemoji@16.0.1/assets/72x72/{}.png"

OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "src", "ui", "emoji")
os.makedirs(OUT_DIR, exist_ok=True)


def rgb_to_rgb565_le(r, g, b):
    """Return (lo, hi) bytes of little-endian RGB565."""
    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return rgb565 & 0xFF, (rgb565 >> 8) & 0xFF


def fetch_and_convert(cdn_name):
    url = CDN.format(cdn_name)
    with urllib.request.urlopen(url, timeout=10) as resp:
        data = resp.read()
    img = Image.open(io.BytesIO(data)).convert("RGBA")
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    pixels = []
    for py in range(SIZE):
        for px in range(SIZE):
            r, g, b, a = img.getpixel((px, py))
            lo, hi = rgb_to_rgb565_le(r, g, b)
            pixels.extend([lo, hi, a])
    return pixels


def write_c_file(sym, codepoint, pixels):
    name = f"emoji_{sym}"
    path = os.path.join(OUT_DIR, f"{name}.c")
    n = len(pixels)
    hex_rows = []
    for i in range(0, n, 12):
        chunk = pixels[i:i+12]
        hex_rows.append("    " + ", ".join(f"0x{b:02X}" for b in chunk))
    hex_block = ",\n".join(hex_rows)

    src = f"""\
// Saitama — {name}.c
// Copyright 2026 Saitama — MIT License
// Auto-generated by scripts/gen_emoji.py — do not edit by hand.
// Source: jdecked/twemoji 16.0.1 (CC-BY 4.0, Twitter/Twemoji contributors)

#include "emoji_data.h"

static const uint8_t {name}_map[] = {{
{hex_block}
}};

const lv_img_dsc_t {name} = {{
    .header = {{
        .cf          = LV_IMG_CF_TRUE_COLOR_ALPHA,
        .always_zero = 0,
        .reserved    = 0,
        .w           = {SIZE},
        .h           = {SIZE},
    }},
    .data_size = {n},
    .data      = {name}_map,
}};
"""
    with open(path, "w") as f:
        f.write(src)
    print(f"  wrote {path}  ({n} bytes)")


def write_header():
    path = os.path.join(OUT_DIR, "emoji_data.h")
    decls = "\n".join(
        f"extern const lv_img_dsc_t emoji_{sym};"
        for sym, _, _, _ in EMOJI
    )
    entries = "\n".join(
        f'    {{ 0x{cp:05X}u, &emoji_{sym}, "{kw}" }},'
        for sym, cp, _, kw in EMOJI
    )
    src = f"""\
// Saitama — emoji_data.h
// Copyright 2026 Saitama — MIT License
// Auto-generated by scripts/gen_emoji.py — do not edit by hand.

#pragma once
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {{
#endif

{decls}

#ifdef __cplusplus
}}
#endif

// Lookup table used by Emoji.cpp and ScreenHome.cpp
typedef struct {{ uint32_t codepoint; const lv_img_dsc_t* img; const char* name; }} OpsEmojiEntry;
static const OpsEmojiEntry kOpsEmoji[] = {{
{entries}
}};
static const int kOpsEmojiCount = {len(EMOJI)};
"""
    with open(path, "w") as f:
        f.write(src)
    print(f"  wrote {path}")


print(f"Downloading and converting {len(EMOJI)} registered + {len(EMOJI_EXTRA)} extra emoji at {SIZE}x{SIZE}px …")
for sym, cp, cdn_name, kw in EMOJI:
    print(f"  {cdn_name} (U+{cp:04X}) …", end=" ", flush=True)
    try:
        px = fetch_and_convert(cdn_name)
        write_c_file(sym, cp, px)
        print("ok")
    except Exception as e:
        print(f"FAILED: {e}")

for sym, cdn_name in EMOJI_EXTRA:
    print(f"  {cdn_name} (extra, not registered) …", end=" ", flush=True)
    try:
        px = fetch_and_convert(cdn_name)
        write_c_file(sym, 0, px)
        print("ok")
    except Exception as e:
        print(f"FAILED: {e}")

write_header()
print("Done.")
