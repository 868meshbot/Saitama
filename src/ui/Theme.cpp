// Saitama — Theme.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "Theme.h"

namespace ops { namespace theme {

// ── Colour variable definitions ──────────────────────────────────────
lv_color_t BG         = LV_COLOR_MAKE(13,  17,  23);
lv_color_t BG_CARD    = LV_COLOR_MAKE(22,  27,  34);
lv_color_t TEXT       = LV_COLOR_MAKE(230, 237, 243);
lv_color_t TEXT_MUTED = LV_COLOR_MAKE(139, 148, 158);
lv_color_t ACCENT     = LV_COLOR_MAKE(88,  166, 255);
lv_color_t PRIMARY    = LV_COLOR_MAKE(0,   51,  170);
lv_color_t GREEN      = LV_COLOR_MAKE(63,  185, 80);
lv_color_t RED        = LV_COLOR_MAKE(248, 81,  73);
lv_color_t ORANGE     = LV_COLOR_MAKE(210, 153, 34);
lv_color_t BORDER     = LV_COLOR_MAKE(48,  54,  61);

// ── applyTheme() ─────────────────────────────────────────────────────
void applyTheme(uint8_t choice)
{
    switch (choice) {

    case 1: // ── Green ────────────────────────────────────────────────
        BG         = LV_COLOR_MAKE(13,  17,  23);
        BG_CARD    = LV_COLOR_MAKE(22,  27,  34);
        TEXT       = LV_COLOR_MAKE(230, 237, 243);
        TEXT_MUTED = LV_COLOR_MAKE(139, 148, 158);
        ACCENT     = LV_COLOR_MAKE(63,  185, 80);   // blue→green swap
        PRIMARY    = LV_COLOR_MAKE(0,   100, 30);
        GREEN      = LV_COLOR_MAKE(88,  166, 255);  // green→blue swap
        RED        = LV_COLOR_MAKE(248, 81,  73);
        ORANGE     = LV_COLOR_MAKE(210, 153, 34);
        BORDER     = LV_COLOR_MAKE(48,  54,  61);
        break;

    case 2: // ── Dracula ──────────────────────────────────────────────
        BG         = LV_COLOR_MAKE(40,  42,  54);   // #282a36
        BG_CARD    = LV_COLOR_MAKE(68,  71,  90);   // #44475a
        TEXT       = LV_COLOR_MAKE(248, 248, 242);  // #f8f8f2
        TEXT_MUTED = LV_COLOR_MAKE(98,  114, 164);  // #6272a4
        ACCENT     = LV_COLOR_MAKE(189, 147, 249);  // #bd93f9 purple
        PRIMARY    = LV_COLOR_MAKE(68,  71,  90);   // #44475a
        GREEN      = LV_COLOR_MAKE(80,  250, 123);  // #50fa7b
        RED        = LV_COLOR_MAKE(255, 85,  85);   // #ff5555
        ORANGE     = LV_COLOR_MAKE(255, 184, 108);  // #ffb86c
        BORDER     = LV_COLOR_MAKE(98,  114, 164);  // #6272a4
        break;

    case 3: // ── Tokyo Night ──────────────────────────────────────────
        BG         = LV_COLOR_MAKE(26,  27,  38);   // #1a1b26
        BG_CARD    = LV_COLOR_MAKE(36,  40,  59);   // #24283b
        TEXT       = LV_COLOR_MAKE(192, 202, 245);  // #c0caf5
        TEXT_MUTED = LV_COLOR_MAKE(122, 162, 247);  // #7aa2f7
        ACCENT     = LV_COLOR_MAKE(187, 154, 247);  // #bb9af7 purple
        PRIMARY    = LV_COLOR_MAKE(125, 207, 255);  // #7dcfff cyan
        GREEN      = LV_COLOR_MAKE(115, 218, 202);  // #73daca
        RED        = LV_COLOR_MAKE(247, 118, 142);  // #f7768e
        ORANGE     = LV_COLOR_MAKE(255, 158, 100);  // #ff9e64
        BORDER     = LV_COLOR_MAKE(65,  72,  104);  // #414868
        break;

    case 4: // ── Catppuccin Frappe ────────────────────────────────────
        BG         = LV_COLOR_MAKE(48,  52,  70);   // #303446 base
        BG_CARD    = LV_COLOR_MAKE(41,  44,  60);   // #292c3c mantle
        TEXT       = LV_COLOR_MAKE(198, 208, 245);  // #c6d0f5
        TEXT_MUTED = LV_COLOR_MAKE(148, 156, 187);  // #949cbb overlay2
        ACCENT     = LV_COLOR_MAKE(140, 170, 238);  // #8caaee blue
        PRIMARY    = LV_COLOR_MAKE(65,  69,  89);   // #414559 surface0
        GREEN      = LV_COLOR_MAKE(166, 209, 137);  // #a6d189
        RED        = LV_COLOR_MAKE(231, 130, 132);  // #e78284
        ORANGE     = LV_COLOR_MAKE(239, 159, 118);  // #ef9f76 peach
        BORDER     = LV_COLOR_MAKE(81,  87,  109);  // #51576d surface1
        break;

    case 5: // ── Catppuccin Mocha ─────────────────────────────────────
        BG         = LV_COLOR_MAKE(30,  30,  46);   // #1e1e2e base
        BG_CARD    = LV_COLOR_MAKE(24,  24,  37);   // #181825 mantle
        TEXT       = LV_COLOR_MAKE(205, 214, 244);  // #cdd6f4
        TEXT_MUTED = LV_COLOR_MAKE(147, 153, 178);  // #9399b2 overlay2
        ACCENT     = LV_COLOR_MAKE(137, 180, 250);  // #89b4fa blue
        PRIMARY    = LV_COLOR_MAKE(49,  50,  68);   // #313244 surface0
        GREEN      = LV_COLOR_MAKE(166, 227, 161);  // #a6e3a1
        RED        = LV_COLOR_MAKE(243, 139, 168);  // #f38ba8
        ORANGE     = LV_COLOR_MAKE(250, 179, 135);  // #fab387 peach
        BORDER     = LV_COLOR_MAKE(69,  71,  90);   // #45475a surface1
        break;

    case 6: // ── Synthwave '84 ────────────────────────────────────────
        BG         = LV_COLOR_MAKE(42,  33,  57);   // #2a2139
        BG_CARD    = LV_COLOR_MAKE(52,  28,  79);   // #341c4f
        TEXT       = LV_COLOR_MAKE(255, 255, 255);  // #ffffff
        TEXT_MUTED = LV_COLOR_MAKE(109, 119, 179);  // #6d77b3
        ACCENT     = LV_COLOR_MAKE(255, 126, 219);  // #ff7edb neon pink
        PRIMARY    = LV_COLOR_MAKE(73,  84,  149);  // #495495
        GREEN      = LV_COLOR_MAKE(114, 241, 184);  // #72f1b8 neon cyan
        RED        = LV_COLOR_MAKE(254, 68,  80);   // #fe4450 neon red
        ORANGE     = LV_COLOR_MAKE(254, 222, 93);   // #fede5d neon yellow
        BORDER     = LV_COLOR_MAKE(73,  84,  149);  // #495495
        break;

    case 7: // ── Kaolin ───────────────────────────────────────────────
        BG         = LV_COLOR_MAKE(27,  30,  31);   // #1b1e1f
        BG_CARD    = LV_COLOR_MAKE(37,  40,  41);   // #252829
        TEXT       = LV_COLOR_MAKE(211, 201, 188);  // #d3c9bc
        TEXT_MUTED = LV_COLOR_MAKE(92,  101, 102);  // #5c6566
        ACCENT     = LV_COLOR_MAKE(129, 179, 198);  // #81b3c6 blue
        PRIMARY    = LV_COLOR_MAKE(45,  49,  50);   // #2d3132
        GREEN      = LV_COLOR_MAKE(159, 176, 124);  // #9fb07c
        RED        = LV_COLOR_MAKE(193, 116, 118);  // #c17476
        ORANGE     = LV_COLOR_MAKE(196, 154, 118);  // #c49a76
        BORDER     = LV_COLOR_MAKE(58,  61,  62);   // #3a3d3e
        break;

    case 8: // ── One Dark ─────────────────────────────────────────────
        BG         = LV_COLOR_MAKE(40,  44,  52);   // #282c34
        BG_CARD    = LV_COLOR_MAKE(33,  37,  43);   // #21252b
        TEXT       = LV_COLOR_MAKE(171, 178, 191);  // #abb2bf
        TEXT_MUTED = LV_COLOR_MAKE(92,  99,  112);  // #5c6370
        ACCENT     = LV_COLOR_MAKE(97,  175, 239);  // #61afef blue
        PRIMARY    = LV_COLOR_MAKE(62,  68,  81);   // #3e4451
        GREEN      = LV_COLOR_MAKE(152, 195, 121);  // #98c379
        RED        = LV_COLOR_MAKE(224, 108, 117);  // #e06c75
        ORANGE     = LV_COLOR_MAKE(209, 154, 102);  // #d19a66
        BORDER     = LV_COLOR_MAKE(62,  68,  81);   // #3e4451
        break;

    case 9: // ── Neovim (Gruvbox Dark) ────────────────────────────────
        BG         = LV_COLOR_MAKE(40,  40,  40);   // #282828
        BG_CARD    = LV_COLOR_MAKE(60,  56,  54);   // #3c3836
        TEXT       = LV_COLOR_MAKE(235, 219, 178);  // #ebdbb2
        TEXT_MUTED = LV_COLOR_MAKE(146, 131, 116);  // #928374
        ACCENT     = LV_COLOR_MAKE(131, 165, 152);  // #83a598 aqua
        PRIMARY    = LV_COLOR_MAKE(80,  73,  69);   // #504945
        GREEN      = LV_COLOR_MAKE(184, 187, 38);   // #b8bb26
        RED        = LV_COLOR_MAKE(251, 73,  52);   // #fb4934
        ORANGE     = LV_COLOR_MAKE(254, 128, 25);   // #fe8019
        BORDER     = LV_COLOR_MAKE(80,  73,  69);   // #504945
        break;

    case 10: // ── Nyx ─────────────────────────────────────────────────
        BG         = LV_COLOR_MAKE(10,  12,  24);   // deep night
        BG_CARD    = LV_COLOR_MAKE(18,  21,  38);
        TEXT       = LV_COLOR_MAKE(220, 225, 240);
        TEXT_MUTED = LV_COLOR_MAKE(120, 130, 170);
        ACCENT     = LV_COLOR_MAKE(140, 170, 255);  // moonlit blue
        PRIMARY    = LV_COLOR_MAKE(110, 120, 255);  // violet-blue
        GREEN      = LV_COLOR_MAKE(120, 210, 180);
        RED        = LV_COLOR_MAKE(255, 120, 160);
        ORANGE     = LV_COLOR_MAKE(255, 190, 120);
        BORDER     = LV_COLOR_MAKE(45,  52,  85);
        break;

    case 11: // ── Rat Dark ────────────────────────────────────────────
        BG         = LV_COLOR_MAKE(20,  18,  15);
        BG_CARD    = LV_COLOR_MAKE(32,  28,  22);
        TEXT       = LV_COLOR_MAKE(235, 225, 200);
        TEXT_MUTED = LV_COLOR_MAKE(150, 130, 100);
        ACCENT     = LV_COLOR_MAKE(145, 255, 0);    // slime green
        PRIMARY    = LV_COLOR_MAKE(70,  60,  45);
        GREEN      = LV_COLOR_MAKE(80,  255, 80);
        RED        = LV_COLOR_MAKE(255, 70,  50);   // hot rod red
        ORANGE     = LV_COLOR_MAKE(255, 140, 0);    // flames
        BORDER     = LV_COLOR_MAKE(90,  60,  35);   // rust
        break;

    default: // ── Default ──────────────────────────────────────────────
        BG         = LV_COLOR_MAKE(13,  17,  23);   // #0d1117
        BG_CARD    = LV_COLOR_MAKE(22,  27,  34);   // #161b22
        TEXT       = LV_COLOR_MAKE(230, 237, 243);  // #e6edf3
        TEXT_MUTED = LV_COLOR_MAKE(139, 148, 158);  // #8b949e
        ACCENT     = LV_COLOR_MAKE(88,  166, 255);  // #58a6ff
        PRIMARY    = LV_COLOR_MAKE(0,   51,  170);  // #0033AA
        GREEN      = LV_COLOR_MAKE(63,  185, 80);   // #3fb950
        RED        = LV_COLOR_MAKE(248, 81,  73);   // #f85149
        ORANGE     = LV_COLOR_MAKE(210, 153, 34);   // #d29922
        BORDER     = LV_COLOR_MAKE(48,  54,  61);   // #30363d
        break;
    }
}

// ── apply() ──────────────────────────────────────────────────────────
void apply(lv_disp_t* disp)
{
    lv_theme_t* th = lv_theme_default_init(
        disp,
        PRIMARY,
        ACCENT,
        true,
        LV_FONT_DEFAULT
    );
    (void)th;
}

}}  // namespace ops::theme
