// Saitama — Screen2048.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "Screen2048.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../utils/Log.h"
#include "../games/g2048_core.h"

#include <Preferences.h>
#include <esp_random.h>
#include <cstdio>

namespace ops { namespace ui {

// ── Layout ────────────────────────────────────────────────────────────
static constexpr int TOP_H   = 28;
static constexpr int BOT_H   = 24;
static constexpr int CELL_SZ = 42;
static constexpr int GAP     = 3;
static constexpr int GRID_SZ = CELL_SZ * 4 + GAP * 5;              // 183 px
static constexpr int GRID_X  = (OPS_SCREEN_W - GRID_SZ) / 2;       // 68 px
static constexpr int GAME_H  = OPS_SCREEN_H - TOP_H - BOT_H;       // 188 px
static constexpr int GRID_Y  = TOP_H + (GAME_H - GRID_SZ) / 2;     // 30 px

// ── Game state ────────────────────────────────────────────────────────
static g2048::Board s_board;
static uint32_t     s_seed        = 1u;
static uint32_t     s_best        = 0;
static bool         s_keepPlaying = false;  // true after user dismisses win overlay

// ── LVGL objects ──────────────────────────────────────────────────────
lv_obj_t* Screen2048::_screen = nullptr;
static lv_obj_t* s_body        = nullptr;
static lv_obj_t* s_cells[16]   = {};
static lv_obj_t* s_scoreLbl    = nullptr;
static lv_obj_t* s_bestLbl     = nullptr;
static lv_obj_t* s_overlay     = nullptr;
static lv_obj_t* s_overlayMsg  = nullptr;
static lv_obj_t* s_overlayScr  = nullptr;
static lv_obj_t* s_keepBtn     = nullptr;

// ── NVS ──────────────────────────────────────────────────────────────
static void _loadBest()
{
    Preferences p;
    p.begin("games", true);
    s_best = (uint32_t)p.getUInt("2048_b", 0);
    p.end();
}

static void _saveBest()
{
    Preferences p;
    p.begin("games", false);
    p.putUInt("2048_b", s_best);
    p.end();
}

// ── Tile colours ──────────────────────────────────────────────────────
static lv_color_t _tileBg(uint32_t v)
{
    switch (v) {
        case    0: return theme::BG_CARD;
        case    2: return lv_color_make(0x46, 0x52, 0x72);
        case    4: return lv_color_make(0x1e, 0x68, 0x5a);
        case    8: return lv_color_make(0xc8, 0x62, 0x20);
        case   16: return lv_color_make(0xc8, 0x42, 0x10);
        case   32: return lv_color_make(0xb8, 0x28, 0x10);
        case   64: return lv_color_make(0x98, 0x14, 0x58);
        case  128: return lv_color_make(0x70, 0x10, 0xa0);
        case  256: return lv_color_make(0x20, 0x14, 0xa0);
        case  512: return lv_color_make(0x10, 0x58, 0xa8);
        case 1024: return lv_color_make(0xb8, 0x88, 0x00);
        case 2048: return lv_color_make(0xff, 0xcc, 0x00);
        default:   return lv_color_make(0xff, 0xee, 0x88);
    }
}

static lv_color_t _tileFg(uint32_t v)
{
    return (v >= 2048) ? lv_color_make(0x20, 0x10, 0x00)
                       : lv_color_make(0xe8, 0xe8, 0xe8);
}

static const lv_font_t* _tileFont(uint32_t v)
{
    if (v < 100)  return &lv_font_montserrat_16;
    if (v < 1000) return &lv_font_montserrat_14;
    return &lv_font_montserrat_12;
}

// ── _refresh() — sync board → LVGL ───────────────────────────────────
static void _refresh()
{
    char buf[12];

    for (int i = 0; i < 16; i++) {
        uint32_t v = s_board.c[i / 4][i % 4];
        lv_obj_t* cell = s_cells[i];
        if (!cell) continue;
        lv_obj_set_style_bg_color(cell, _tileBg(v), 0);
        lv_obj_t* lbl = lv_obj_get_child(cell, 0);
        if (!lbl) continue;
        if (v) snprintf(buf, sizeof(buf), "%u", (unsigned)v);
        else   buf[0] = '\0';
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, _tileFg(v), 0);
        lv_obj_set_style_text_font(lbl, _tileFont(v), 0);
    }

    if (s_best < s_board.score) { s_best = s_board.score; _saveBest(); }

    if (s_scoreLbl) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)s_board.score);
        lv_label_set_text(s_scoreLbl, buf);
    }
    if (s_bestLbl) {
        snprintf(buf, sizeof(buf), "Best: %u", (unsigned)s_best);
        lv_label_set_text(s_bestLbl, buf);
    }

    if (!s_overlay) return;

    bool showOverlay = false;
    const char* msg = "";
    if (s_board.won && !s_keepPlaying) {
        msg = "You reached 2048!";
        showOverlay = true;
        if (s_keepBtn) lv_obj_clear_flag(s_keepBtn, LV_OBJ_FLAG_HIDDEN);
    } else if (g2048::hasLost(s_board)) {
        msg = "Game Over";
        showOverlay = true;
        if (s_keepBtn) lv_obj_add_flag(s_keepBtn, LV_OBJ_FLAG_HIDDEN);
    }
    if (showOverlay) {
        if (s_overlayMsg) lv_label_set_text(s_overlayMsg, msg);
        if (s_overlayScr) {
            snprintf(buf, sizeof(buf), "Score: %u", (unsigned)s_board.score);
            lv_label_set_text(s_overlayScr, buf);
        }
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

// ── _newGame() ───────────────────────────────────────────────────────
static void _newGame()
{
    s_seed = (uint32_t)esp_random();
    g2048::reset(s_board);
    s_keepPlaying = false;
    g2048::spawnTile(s_board, s_seed);
    g2048::spawnTile(s_board, s_seed);
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    _refresh();
}

// ── _doMove() — validate, slide, spawn ───────────────────────────────
static void _doMove(int dir)
{
    if (g2048::hasLost(s_board)) return;
    if (s_board.won && !s_keepPlaying) return;
    if (g2048::move(s_board, dir)) {
        g2048::spawnTile(s_board, s_seed);
        _refresh();
    }
}

// ── navigate() — called from UIScreen::tick() ─────────────────────────
void Screen2048::navigate(int dx, int dy)
{
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    int dir = -1;
    if (ax >= ay) {
        if      (dx > 0) dir = 1;   // right
        else if (dx < 0) dir = 0;   // left
    } else {
        if      (dy > 0) dir = 3;   // down
        else if (dy < 0) dir = 2;   // up
    }
    if (dir >= 0) _doMove(dir);
}

// ── Callbacks ─────────────────────────────────────────────────────────
void Screen2048::_onKey(lv_event_t* e)
{
    uint32_t key = lv_event_get_key(e);
    if      (key == LV_KEY_ESC)                 ScreenLauncher::show();
    else if (key == 'n' || key == 'N')           _newGame();
    else if (key == 'a' || key == LV_KEY_LEFT)   _doMove(0);
    else if (key == 'd' || key == LV_KEY_RIGHT)  _doMove(1);
    else if (key == 'w' || key == LV_KEY_UP)     _doMove(2);
    else if (key == 's' || key == LV_KEY_DOWN)   _doMove(3);
}

void Screen2048::_onHomeClick(lv_event_t*) { ScreenLauncher::show(); }
void Screen2048::_onNewClick(lv_event_t*)  { _newGame(); }
void Screen2048::_onKeepClick(lv_event_t*)
{
    s_keepPlaying = true;
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

// ── _build() — create LVGL tree once ─────────────────────────────────
void Screen2048::_build()
{
    _loadBest();

    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ───────────────────────────────────────────────────────
    {
        lv_obj_t* bar = lv_obj_create(_screen);
        lv_obj_set_size(bar, OPS_SCREEN_W, TOP_H);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_hor(bar, 4, 0);
        lv_obj_set_style_pad_ver(bar, 2, 0);
        lv_obj_set_style_pad_column(bar, 6, 0);
        lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bar,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        auto mkBtn = [](lv_obj_t* par, const char* text, lv_event_cb_t cb) -> lv_obj_t*
        {
            lv_obj_t* btn = lv_btn_create(par);
            lv_group_remove_obj(btn);
            lv_obj_set_height(btn, TOP_H - 6);
            lv_obj_set_style_bg_color(btn, theme::PRIMARY, 0);
            lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_radius(btn, 4, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_pad_hor(btn, 6, 0);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, text);
            lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
            lv_obj_center(lbl);
            return btn;
        };

        lv_obj_t* homeBtn = mkBtn(bar, LV_SYMBOL_HOME, _onHomeClick);
        lv_obj_set_style_bg_color(homeBtn, theme::BG, 0);
        lv_obj_set_style_border_color(homeBtn, theme::BORDER, 0);
        lv_obj_set_style_border_width(homeBtn, 1, 0);
        lv_obj_t* homeLbl = lv_obj_get_child(homeBtn, 0);
        lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);

        lv_obj_t* title = lv_label_create(bar);
        lv_label_set_text(title, "2048");
        lv_obj_set_style_text_color(title, theme::TEXT, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

        // Spacer
        lv_obj_t* sp = lv_obj_create(bar);
        lv_obj_set_size(sp, 1, 1);
        lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sp, 0, 0);
        lv_obj_set_style_pad_all(sp, 0, 0);
        lv_obj_set_flex_grow(sp, 1);

        s_scoreLbl = lv_label_create(bar);
        lv_label_set_text(s_scoreLbl, "0");
        lv_obj_set_style_text_color(s_scoreLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(s_scoreLbl, &lv_font_montserrat_12, 0);

        mkBtn(bar, "N", _onNewClick);
    }

    // ── Body — transparent, receives key events ───────────────────────
    s_body = lv_obj_create(_screen);
    lv_obj_set_size(s_body, OPS_SCREEN_W, GAME_H);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_body, _onKey, LV_EVENT_KEY, nullptr);

    // ── 4×4 grid ──────────────────────────────────────────────────────
    {
        static const lv_coord_t kCols[] = {
            LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
            LV_GRID_TEMPLATE_LAST
        };
        static const lv_coord_t kRows[] = {
            LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
            LV_GRID_TEMPLATE_LAST
        };

        lv_obj_t* grid = lv_obj_create(_screen);
        lv_obj_set_size(grid, GRID_SZ, GRID_SZ);
        lv_obj_set_pos(grid, GRID_X, GRID_Y);
        lv_obj_set_style_bg_color(grid, theme::BORDER, 0);
        lv_obj_set_style_border_width(grid, 0, 0);
        lv_obj_set_style_radius(grid, 6, 0);
        lv_obj_set_style_pad_all(grid, GAP, 0);
        lv_obj_set_style_pad_row(grid, GAP, 0);
        lv_obj_set_style_pad_column(grid, GAP, 0);
        lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(grid, LV_LAYOUT_GRID);
        lv_obj_set_grid_dsc_array(grid, kCols, kRows);

        for (int i = 0; i < 16; i++) {
            int col = i % 4;
            int row = i / 4;

            lv_obj_t* cell = lv_obj_create(grid);
            s_cells[i] = cell;
            lv_group_remove_obj(cell);
            lv_obj_set_style_bg_color(cell, theme::BG_CARD, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_radius(cell, 4, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_grid_cell(cell,
                LV_GRID_ALIGN_STRETCH, col, 1,
                LV_GRID_ALIGN_STRETCH, row, 1);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(cell,
                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            lv_obj_t* lbl = lv_label_create(cell);
            lv_label_set_text(lbl, "");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(lbl, lv_color_make(0xe8, 0xe8, 0xe8), 0);
            lv_obj_center(lbl);
        }
    }

    // ── Bottom bar ────────────────────────────────────────────────────
    {
        lv_obj_t* bot = lv_obj_create(_screen);
        lv_obj_set_size(bot, OPS_SCREEN_W, BOT_H);
        lv_obj_align(bot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_color(bot, theme::BG_CARD, 0);
        lv_obj_set_style_border_width(bot, 0, 0);
        lv_obj_set_style_radius(bot, 0, 0);
        lv_obj_set_style_pad_hor(bot, 6, 0);
        lv_obj_set_style_pad_ver(bot, 2, 0);
        lv_obj_set_scrollbar_mode(bot, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(bot, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bot,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        s_bestLbl = lv_label_create(bot);
        lv_label_set_text(s_bestLbl, "Best: 0");
        lv_obj_set_style_text_color(s_bestLbl, theme::ACCENT, 0);
        lv_obj_set_style_text_font(s_bestLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* sp = lv_obj_create(bot);
        lv_obj_set_size(sp, 1, 1);
        lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sp, 0, 0);
        lv_obj_set_style_pad_all(sp, 0, 0);
        lv_obj_set_flex_grow(sp, 1);

        lv_obj_t* hint = lv_label_create(bot);
        lv_label_set_text(hint, "WASD  N=new");
        lv_obj_set_style_text_color(hint, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    }

    // ── Win / game-over overlay ───────────────────────────────────────
    {
        s_overlay = lv_obj_create(_screen);
        lv_obj_set_size(s_overlay, 188, LV_SIZE_CONTENT);
        lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(s_overlay, theme::BG_CARD, 0);
        lv_obj_set_style_bg_opa(s_overlay, 240, 0);
        lv_obj_set_style_border_color(s_overlay, theme::ACCENT, 0);
        lv_obj_set_style_border_width(s_overlay, 2, 0);
        lv_obj_set_style_radius(s_overlay, 8, 0);
        lv_obj_set_style_shadow_width(s_overlay, 16, 0);
        lv_obj_set_style_shadow_color(s_overlay, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(s_overlay, 180, 0);
        lv_obj_set_style_pad_all(s_overlay, 10, 0);
        lv_obj_set_style_pad_row(s_overlay, 6, 0);
        lv_obj_set_scrollbar_mode(s_overlay, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_overlay,
            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        s_overlayMsg = lv_label_create(s_overlay);
        lv_label_set_text(s_overlayMsg, "");
        lv_obj_set_style_text_color(s_overlayMsg, theme::TEXT, 0);
        lv_obj_set_style_text_font(s_overlayMsg, &lv_font_montserrat_14, 0);

        s_overlayScr = lv_label_create(s_overlay);
        lv_label_set_text(s_overlayScr, "");
        lv_obj_set_style_text_color(s_overlayScr, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(s_overlayScr, &lv_font_montserrat_10, 0);

        auto mkOvBtn = [](lv_obj_t* par, const char* text,
                          lv_color_t bg, lv_event_cb_t cb) -> lv_obj_t*
        {
            lv_obj_t* btn = lv_btn_create(par);
            lv_group_remove_obj(btn);
            lv_obj_set_size(btn, 148, 28);
            lv_obj_set_style_bg_color(btn, bg, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_radius(btn, 6, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, text);
            lv_obj_set_style_text_color(lbl, theme::BG, 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
            lv_obj_center(lbl);
            return btn;
        };

        s_keepBtn = mkOvBtn(s_overlay, "Keep Playing", theme::ACCENT,   _onKeepClick);
        mkOvBtn(s_overlay, "New Game",     theme::PRIMARY, _onNewClick);

        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

// ── show() / isActive() ───────────────────────────────────────────────
void Screen2048::show()
{
    if (!_screen) {
        _build();
        _newGame();
    }
    lv_group_focus_obj(s_body);
    lv_scr_load(_screen);
    OPS_LOG("UI", "2048 shown, score=%u best=%u",
            (unsigned)s_board.score, (unsigned)s_best);
}

bool Screen2048::isActive()
{
    return _screen && (lv_scr_act() == _screen);
}

}}  // namespace ops::ui
