// Saitama — ScreenZeroXZero.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenZeroXZero.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../utils/Log.h"
#include "../games/ttt_core.h"

#include <Preferences.h>
#include <esp_random.h>
#include <cstdio>

namespace ops { namespace ui {

// ── Layout ────────────────────────────────────────────────────────────
static constexpr int TOP_H   = 28;
static constexpr int BOT_H   = 24;
static constexpr int CELL_SZ = 60;
static constexpr int GAP     = 3;
static constexpr int GRID_SZ = CELL_SZ * 3 + GAP * 2;               // 186 px
static constexpr int GRID_X  = (OPS_SCREEN_W - GRID_SZ) / 2;        // 67 px
static constexpr int GAME_H  = OPS_SCREEN_H - TOP_H - BOT_H;        // 188 px
static constexpr int GRID_Y  = TOP_H + (GAME_H - GRID_SZ) / 2;      // 29 px

// ── Game state ────────────────────────────────────────────────────────
static ttt::Board s_board;
static bool       s_gameOver = false;
static bool       s_vsAI    = true;
static int        s_turn    = 1;     // 1=X  2=O
static int        s_wins    = 0;     // X (human player 1) wins
static int        s_draws   = 0;

// ── LVGL objects ──────────────────────────────────────────────────────
lv_obj_t* ScreenZeroXZero::_screen = nullptr;
static lv_obj_t* s_body        = nullptr;
static lv_obj_t* s_cells[9]    = {};
static lv_obj_t* s_statusLbl   = nullptr;
static lv_obj_t* s_scoreLbl    = nullptr;
static lv_obj_t* s_modeLbl     = nullptr;
static lv_obj_t* s_overlay     = nullptr;  // game-over overlay
static lv_obj_t* s_overlayMsg  = nullptr;  // result text inside overlay

// ── NVS ──────────────────────────────────────────────────────────────
static void _loadScores()
{
    Preferences p;
    p.begin("games", true);
    s_wins  = p.getInt("ttt_w", 0);
    s_draws = p.getInt("ttt_d", 0);
    p.end();
}

static void _saveScores()
{
    Preferences p;
    p.begin("games", false);
    p.putInt("ttt_w", s_wins);
    p.putInt("ttt_d", s_draws);
    p.end();
}

// ── _refresh() — sync board → LVGL labels ────────────────────────────
static void _refresh()
{
    static const char* kMark[3] = { "", "X", "O" };

    for (int i = 0; i < 9; i++) {
        lv_obj_t* lbl = lv_obj_get_child(s_cells[i], 0);
        if (!lbl) continue;
        uint8_t v = s_board.c[i];
        lv_label_set_text(lbl, kMark[v]);
        lv_obj_set_style_text_color(lbl,
            v == 1 ? theme::ACCENT : theme::RED, 0);
    }

    if (s_scoreLbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "W:%d D:%d", s_wins, s_draws);
        lv_label_set_text(s_scoreLbl, buf);
    }

    int w = ttt::winner(s_board);

    if (s_statusLbl) {
        const char* msg;
        if      (w == 1)                msg = "You win!";
        else if (w == 2)                msg = s_vsAI ? "Device wins" : "O wins";
        else if (w == 3)                msg = "Draw";
        else if (s_vsAI && s_turn == 2) msg = "Thinking...";
        else msg = (s_turn == 1) ? "Your turn (X)" : "O turn";
        lv_label_set_text(s_statusLbl, msg);
    }

    // Show or hide the game-over overlay
    if (s_overlay && s_overlayMsg) {
        if (w != 0) {
            const char* result;
            if      (w == 1) result = "You win!";
            else if (w == 2) result = s_vsAI ? "Device wins" : "O wins";
            else             result = "Draw";
            lv_label_set_text(s_overlayMsg, result);
            lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ── _newGame() ───────────────────────────────────────────────────────
static void _newGame()
{
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    ttt::reset(s_board);
    s_gameOver = false;
    s_turn     = 1;
    _refresh();
}

static void _onPlayAgainClick(lv_event_t*) { _newGame(); }

// ── _aiPlay() — O plays with minimax + 20 % random blunder ───────────
static void _aiPlay()
{
    int idx = -1;
    if ((esp_random() % 10) < 2) {
        int emp[9]; int cnt = 0;
        for (int i = 0; i < 9; i++) if (!s_board.c[i]) emp[cnt++] = i;
        if (cnt) idx = emp[esp_random() % cnt];
    }
    if (idx < 0) idx = ttt::bestMove(s_board, 2);
    if (idx < 0) return;

    s_board.c[idx] = 2;
    int w = ttt::winner(s_board);
    if (w) {
        s_gameOver = true;
        if (w == 3) { s_draws++; _saveScores(); }
    } else {
        s_turn = 1;
    }
    _refresh();
}

// ── _humanPlay() — current player places a mark ──────────────────────
static void _humanPlay(int idx)
{
    if (s_gameOver || s_board.c[idx]) return;

    s_board.c[idx] = (uint8_t)s_turn;
    int w = ttt::winner(s_board);
    if (w) {
        s_gameOver = true;
        if (w == 1) { s_wins++; _saveScores(); }
        if (w == 3) { s_draws++; _saveScores(); }
        _refresh();
        return;
    }
    if (s_vsAI && s_turn == 1) {
        s_turn = 2;
        _refresh();
        _aiPlay();
    } else {
        s_turn = (s_turn == 1) ? 2 : 1;
        _refresh();
    }
}

// ── Callbacks ─────────────────────────────────────────────────────────
void ScreenZeroXZero::_onCellClick(lv_event_t* e)
{
    _humanPlay((int)(intptr_t)lv_event_get_user_data(e));
}

void ScreenZeroXZero::_onKey(lv_event_t* e)
{
    uint32_t key = lv_event_get_key(e);
    if      (key == LV_KEY_ESC)          ScreenLauncher::show();
    else if (key == 'n' || key == 'N')   _newGame();
    else if (key == 'm' || key == 'M') {
        s_vsAI = !s_vsAI;
        if (s_modeLbl) lv_label_set_text(s_modeLbl, s_vsAI ? "AI" : "2P");
        _newGame();
    }
}

void ScreenZeroXZero::_onHomeClick(lv_event_t*) { ScreenLauncher::show(); }

void ScreenZeroXZero::_onModeClick(lv_event_t*)
{
    s_vsAI = !s_vsAI;
    if (s_modeLbl) lv_label_set_text(s_modeLbl, s_vsAI ? "AI" : "2P");
    _newGame();
}

void ScreenZeroXZero::_onNewClick(lv_event_t*) { _newGame(); }

// ── _build() — create LVGL tree once ─────────────────────────────────
void ScreenZeroXZero::_build()
{
    _loadScores();

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
        lv_obj_set_style_pad_column(bar, 4, 0);
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

        // Home
        lv_obj_t* homeBtn = mkBtn(bar, LV_SYMBOL_HOME, _onHomeClick);
        lv_obj_set_style_bg_color(homeBtn, theme::BG, 0);
        lv_obj_set_style_border_color(homeBtn, theme::BORDER, 0);
        lv_obj_set_style_border_width(homeBtn, 1, 0);
        lv_obj_t* homeLbl = lv_obj_get_child(homeBtn, 0);
        lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);

        // Title
        lv_obj_t* title = lv_label_create(bar);
        lv_label_set_text(title, "0x0");
        lv_obj_set_style_text_color(title, theme::TEXT, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

        // Spacer
        lv_obj_t* sp = lv_obj_create(bar);
        lv_obj_set_size(sp, 1, 1);
        lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sp, 0, 0);
        lv_obj_set_style_pad_all(sp, 0, 0);
        lv_obj_set_flex_grow(sp, 1);

        // Score
        s_scoreLbl = lv_label_create(bar);
        lv_label_set_text(s_scoreLbl, "W:0 D:0");
        lv_obj_set_style_text_color(s_scoreLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(s_scoreLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_pad_right(s_scoreLbl, 2, 0);

        // Mode toggle (AI / 2P)
        lv_obj_t* modeBtn = mkBtn(bar, "AI", _onModeClick);
        s_modeLbl = lv_obj_get_child(modeBtn, 0);

        // New game
        mkBtn(bar, "N", _onNewClick);
    }

    // ── Body — transparent, fills game area, receives key events ─────
    s_body = lv_obj_create(_screen);
    lv_obj_set_size(s_body, OPS_SCREEN_W, GAME_H);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_body, _onKey, LV_EVENT_KEY, nullptr);

    // ── 3×3 grid ──────────────────────────────────────────────────────
    {
        static const lv_coord_t kCols[] = {
            LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
        };
        static const lv_coord_t kRows[] = {
            LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
        };

        lv_obj_t* grid = lv_obj_create(_screen);
        lv_obj_set_size(grid, GRID_SZ, GRID_SZ);
        lv_obj_set_pos(grid, GRID_X, GRID_Y);
        lv_obj_set_style_bg_color(grid, theme::BORDER, 0);  // gap colour = grid lines
        lv_obj_set_style_border_width(grid, 0, 0);
        lv_obj_set_style_radius(grid, 4, 0);
        lv_obj_set_style_pad_all(grid, GAP, 0);
        lv_obj_set_style_pad_row(grid, GAP, 0);
        lv_obj_set_style_pad_column(grid, GAP, 0);
        lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(grid, LV_LAYOUT_GRID);
        lv_obj_set_grid_dsc_array(grid, kCols, kRows);

        for (int i = 0; i < 9; i++) {
            int col = i % 3;
            int row = i / 3;

            lv_obj_t* cell = lv_btn_create(grid);
            s_cells[i] = cell;
            lv_group_remove_obj(cell);
            lv_obj_set_style_bg_color(cell, theme::BG_CARD, 0);
            lv_obj_set_style_bg_color(cell, theme::PRIMARY, LV_STATE_PRESSED);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_radius(cell, 2, 0);
            lv_obj_set_style_shadow_width(cell, 0, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_set_grid_cell(cell,
                LV_GRID_ALIGN_STRETCH, col, 1,
                LV_GRID_ALIGN_STRETCH, row, 1);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(cell,
                LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_add_event_cb(cell, _onCellClick, LV_EVENT_CLICKED,
                                (void*)(intptr_t)i);

            lv_obj_t* lbl = lv_label_create(cell);
            lv_label_set_text(lbl, "");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_36, 0);
            lv_obj_set_style_text_color(lbl, theme::ACCENT, 0);
            lv_obj_center(lbl);
        }
    }

    // ── Status bar ────────────────────────────────────────────────────
    {
        lv_obj_t* bot = lv_obj_create(_screen);
        lv_obj_set_size(bot, OPS_SCREEN_W, BOT_H);
        lv_obj_align(bot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_color(bot, theme::BG_CARD, 0);
        lv_obj_set_style_border_width(bot, 0, 0);
        lv_obj_set_style_radius(bot, 0, 0);
        lv_obj_set_style_pad_all(bot, 4, 0);
        lv_obj_set_scrollbar_mode(bot, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);

        s_statusLbl = lv_label_create(bot);
        lv_label_set_text(s_statusLbl, "Your turn (X)");
        lv_obj_set_style_text_color(s_statusLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(s_statusLbl, &lv_font_montserrat_12, 0);
        lv_obj_align(s_statusLbl, LV_ALIGN_CENTER, 0, 0);
    }

    // ── Game-over overlay (hidden until a game ends) ──────────────────
    // Floats above the grid; child of _screen so it draws on top.
    {
        s_overlay = lv_obj_create(_screen);
        lv_obj_set_size(s_overlay, 160, 76);
        lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(s_overlay, theme::BG_CARD, 0);
        lv_obj_set_style_bg_opa(s_overlay, 240, 0);
        lv_obj_set_style_border_color(s_overlay, theme::ACCENT, 0);
        lv_obj_set_style_border_width(s_overlay, 2, 0);
        lv_obj_set_style_radius(s_overlay, 8, 0);
        lv_obj_set_style_shadow_width(s_overlay, 12, 0);
        lv_obj_set_style_shadow_color(s_overlay, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(s_overlay, 180, 0);
        lv_obj_set_style_pad_all(s_overlay, 8, 0);
        lv_obj_set_style_pad_row(s_overlay, 6, 0);
        lv_obj_set_scrollbar_mode(s_overlay, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_overlay,
            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        s_overlayMsg = lv_label_create(s_overlay);
        lv_label_set_text(s_overlayMsg, "");
        lv_obj_set_style_text_color(s_overlayMsg, theme::TEXT, 0);
        lv_obj_set_style_text_font(s_overlayMsg, &lv_font_montserrat_16, 0);

        lv_obj_t* btn = lv_btn_create(s_overlay);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 120, 28);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, _onPlayAgainClick, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "Play Again");
        lv_obj_set_style_text_color(lbl, theme::BG, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);

        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

// ── show() ────────────────────────────────────────────────────────────
void ScreenZeroXZero::show()
{
    if (!_screen) {
        _build();
        _newGame();
    }

    lv_group_focus_obj(s_body);
    lv_scr_load(_screen);
    OPS_LOG("UI", "0x0 shown");
}

}}  // namespace ops::ui
