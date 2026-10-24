// Saitama — ScreenSnake.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenSnake.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../utils/Log.h"
#include "../games/snake_core.h"

#include <Preferences.h>
#include <esp_random.h>
#include <cstdio>

namespace ops { namespace ui {

// ── Layout ────────────────────────────────────────────────────────────
static constexpr int TOP_H    = 28;
static constexpr int CELL     = 10;
static constexpr int BORDER_W = 2;
static constexpr int FIELD_W  = snake::COLS * CELL + BORDER_W * 2;     // 304 px
static constexpr int FIELD_H  = snake::ROWS * CELL + BORDER_W * 2;     // 204 px
static constexpr int GAME_H   = OPS_SCREEN_H - TOP_H;                  // 212 px
static constexpr int FIELD_X  = (OPS_SCREEN_W - FIELD_W) / 2;          // 8 px
static constexpr int FIELD_Y  = (GAME_H - FIELD_H) / 2;                // 4 px inside the body

// ── Timing ────────────────────────────────────────────────────────────
static constexpr uint32_t STEP_START_MS = 160;
static constexpr uint32_t STEP_MIN_MS   = 70;
static constexpr uint32_t STEP_FASTER   = 4;    // ms faster per food eaten
static constexpr int      SWIPE_PX      = 18;   // drag distance that counts as a turn

// ── Game state ────────────────────────────────────────────────────────
enum State : uint8_t { READY, RUNNING, PAUSED, OVER };

static snake::Game s_game;
static uint32_t    s_seed  = 1u;
static uint32_t    s_best  = 0;
static State       s_state = READY;

// Touch: anchor point of the current drag, and whether it already turned.
static lv_point_t  s_anchor  = { 0, 0 };
static bool        s_swiped  = false;

// ── LVGL objects ──────────────────────────────────────────────────────
lv_obj_t* ScreenSnake::_screen = nullptr;
static lv_obj_t*   s_body       = nullptr;
static lv_obj_t*   s_field      = nullptr;
static lv_obj_t*   s_scoreLbl   = nullptr;
static lv_obj_t*   s_bestLbl    = nullptr;
static lv_obj_t*   s_msg        = nullptr;
static lv_obj_t*   s_overlay    = nullptr;
static lv_obj_t*   s_overlayScr = nullptr;
static lv_timer_t* s_timer      = nullptr;

// ── NVS ──────────────────────────────────────────────────────────────
static void _loadBest()
{
    Preferences p;
    p.begin("games", true);
    s_best = (uint32_t)p.getUInt("snake_b", 0);
    p.end();
}

static void _saveBest()
{
    Preferences p;
    p.begin("games", false);
    p.putUInt("snake_b", s_best);
    p.end();
}

// ── Drawing helpers ───────────────────────────────────────────────────
static void _cellArea(uint16_t c, lv_area_t& a)
{
    lv_area_t f;
    lv_obj_get_content_coords(s_field, &f);
    a.x1 = f.x1 + snake::cellX(c) * CELL;
    a.y1 = f.y1 + snake::cellY(c) * CELL;
    a.x2 = a.x1 + CELL - 1;
    a.y2 = a.y1 + CELL - 1;
}

static void _invCell(uint16_t c)
{
    if (c == snake::NO_CELL || !s_field) return;
    lv_area_t a;
    _cellArea(c, a);
    lv_obj_invalidate_area(s_field, &a);
}

// ── _refreshLabels() — score, best, message, overlay ─────────────────
static void _refreshLabels()
{
    char buf[24];

    if (s_best < s_game.score) { s_best = s_game.score; _saveBest(); }

    if (s_scoreLbl) {
        snprintf(buf, sizeof(buf), "Score: %u", (unsigned)s_game.score);
        lv_label_set_text(s_scoreLbl, buf);
    }
    if (s_bestLbl) {
        snprintf(buf, sizeof(buf), "Best: %u", (unsigned)s_best);
        lv_label_set_text(s_bestLbl, buf);
    }

    if (s_msg) {
        if (s_state == READY) {
            lv_label_set_text(s_msg, "Swipe or roll to start");
            lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        } else if (s_state == PAUSED) {
            lv_label_set_text(s_msg, "Paused - click to resume");
            lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!s_overlay) return;
    if (s_state == OVER) {
        if (s_overlayScr) {
            snprintf(buf, sizeof(buf), "Score: %u", (unsigned)s_game.score);
            lv_label_set_text(s_overlayScr, buf);
        }
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

// ── State changes ─────────────────────────────────────────────────────
static void _newGame()
{
    s_seed = (uint32_t)esp_random();
    snake::reset(s_game, s_seed);
    s_state = READY;
    if (s_timer) lv_timer_set_period(s_timer, STEP_START_MS);
    if (s_field) lv_obj_invalidate(s_field);
    _refreshLabels();
}

static void _start()
{
    if (s_state != READY && s_state != PAUSED) return;
    s_state = RUNNING;
    _refreshLabels();
}

static void _pause()
{
    if (s_state != RUNNING) return;
    s_state = PAUSED;
    _refreshLabels();
}

// Steering input from any source. The first input of a game starts it.
static void _steer(uint8_t d)
{
    if (s_state == OVER) return;
    snake::turn(s_game, d);
    _start();
}

// ── navigate() / press() — called from UIScreen::tick() ───────────────
void ScreenSnake::navigate(int dx, int dy)
{
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    if (ax >= ay) {
        if      (dx > 0) _steer(snake::RIGHT);
        else if (dx < 0) _steer(snake::LEFT);
    } else {
        if      (dy > 0) _steer(snake::DOWN);
        else if (dy < 0) _steer(snake::UP);
    }
}

void ScreenSnake::press()
{
    if      (s_state == RUNNING) _pause();
    else if (s_state == OVER)    _newGame();
    else                         _start();
}

// ── Game tick ─────────────────────────────────────────────────────────
void ScreenSnake::_onTick(lv_timer_t* t)
{
    if (!isActive()) {
        // Left the screen (home, screensaver): pause and stop ticking.
        _pause();
        lv_timer_pause(t);
        return;
    }
    if (s_state != RUNNING) return;

    uint16_t oldHead = s_game.body[s_game.head];
    uint16_t oldFood = s_game.food;
    snake::Step r = snake::step(s_game, s_seed);

    if (r == snake::STEP_DEAD || r == snake::STEP_WON) {
        s_state = OVER;
        lv_obj_invalidate(s_field);
        _refreshLabels();
        OPS_LOG("UI", "Snake over, score=%u len=%u",
                (unsigned)s_game.score, (unsigned)s_game.len);
        return;
    }

    _invCell(oldHead);                        // head colour → body colour
    _invCell(s_game.body[s_game.head]);
    _invCell(s_game.vacated);
    if (r == snake::STEP_ATE) {
        _invCell(oldFood);
        _invCell(s_game.food);
        uint32_t faster = (uint32_t)s_game.eaten * STEP_FASTER;
        uint32_t period = (faster < STEP_START_MS - STEP_MIN_MS)
                          ? STEP_START_MS - faster : STEP_MIN_MS;
        lv_timer_set_period(t, period);
        _refreshLabels();
    }
}

// ── Callbacks ─────────────────────────────────────────────────────────
void ScreenSnake::_onKey(lv_event_t* e)
{
    uint32_t key = lv_event_get_key(e);
    if      (key == LV_KEY_ESC)                        ScreenLauncher::show();
    else if (key == 'n' || key == 'N')                  _newGame();
    else if (key == ' ' || key == LV_KEY_ENTER)         press();
    else if (key == 'a' || key == LV_KEY_LEFT)          _steer(snake::LEFT);
    else if (key == 'd' || key == LV_KEY_RIGHT)         _steer(snake::RIGHT);
    else if (key == 'w' || key == LV_KEY_UP)            _steer(snake::UP);
    else if (key == 's' || key == LV_KEY_DOWN)          _steer(snake::DOWN);
}

// Swipes: every SWIPE_PX of drag in one direction is a turn, measured
// from where the last turn happened, so you can steer without lifting.
// A tap with no drag starts, pauses or resumes.
void ScreenSnake::_onTouch(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_anchor = p;
        s_swiped = false;
    } else if (code == LV_EVENT_PRESSING) {
        int dx = p.x - s_anchor.x;
        int dy = p.y - s_anchor.y;
        int ax = dx < 0 ? -dx : dx;
        int ay = dy < 0 ? -dy : dy;
        if (ax < SWIPE_PX && ay < SWIPE_PX) return;
        navigate(ax >= ay ? dx : 0, ax >= ay ? 0 : dy);
        s_anchor = p;
        s_swiped = true;
    } else if (code == LV_EVENT_RELEASED) {
        if (!s_swiped && s_state != OVER) press();
    }
}

void ScreenSnake::_onDraw(lv_event_t* e)
{
    lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 2;

    lv_area_t a;
    if (s_game.food != snake::NO_CELL) {
        _cellArea(s_game.food, a);
        lv_area_increase(&a, -1, -1);
        dsc.bg_color = theme::RED;
        dsc.radius   = LV_RADIUS_CIRCLE;
        lv_draw_rect(ctx, &dsc, &a);
        dsc.radius   = 2;
    }

    dsc.bg_color = (s_state == OVER) ? theme::TEXT_MUTED : theme::GREEN;
    for (int i = 1; i < s_game.len; i++) {
        uint16_t c = s_game.body[(s_game.head + snake::CELLS - i) % snake::CELLS];
        _cellArea(c, a);
        lv_area_increase(&a, -1, -1);
        lv_draw_rect(ctx, &dsc, &a);
    }

    _cellArea(s_game.body[s_game.head], a);
    dsc.bg_color = (s_state == OVER) ? theme::RED : theme::ACCENT;
    lv_draw_rect(ctx, &dsc, &a);
}

void ScreenSnake::_onHomeClick(lv_event_t*) { ScreenLauncher::show(); }
void ScreenSnake::_onNewClick(lv_event_t*)  { _newGame(); }

// ── _build() — create LVGL tree once ─────────────────────────────────
void ScreenSnake::_build()
{
    _loadBest();

    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar: home, title, score, best, new game ───────────────────
    {
        lv_obj_t* bar = lv_obj_create(_screen);
        lv_obj_set_size(bar, OPS_SCREEN_W, TOP_H);
        lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_hor(bar, 4, 0);
        lv_obj_set_style_pad_ver(bar, 2, 0);
        lv_obj_set_style_pad_column(bar, 8, 0);
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
        lv_label_set_text(title, "Snake");
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
        lv_label_set_text(s_scoreLbl, "Score: 0");
        lv_obj_set_style_text_color(s_scoreLbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(s_scoreLbl, &lv_font_montserrat_12, 0);

        s_bestLbl = lv_label_create(bar);
        lv_label_set_text(s_bestLbl, "Best: 0");
        lv_obj_set_style_text_color(s_bestLbl, theme::ACCENT, 0);
        lv_obj_set_style_text_font(s_bestLbl, &lv_font_montserrat_10, 0);

        mkBtn(bar, "N", _onNewClick);
    }

    // ── Body — transparent, receives key and touch events ─────────────
    s_body = lv_obj_create(_screen);
    lv_obj_set_size(s_body, OPS_SCREEN_W, GAME_H);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_body, _onKey,   LV_EVENT_KEY,      nullptr);
    lv_obj_add_event_cb(s_body, _onTouch, LV_EVENT_PRESSED,  nullptr);
    lv_obj_add_event_cb(s_body, _onTouch, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_body, _onTouch, LV_EVENT_RELEASED, nullptr);

    // ── Playing field — bordered box, snake drawn on top ──────────────
    s_field = lv_obj_create(s_body);
    lv_group_remove_obj(s_field);
    lv_obj_set_size(s_field, FIELD_W, FIELD_H);
    lv_obj_set_pos(s_field, FIELD_X, FIELD_Y);
    lv_obj_set_style_bg_color(s_field, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(s_field, theme::ACCENT, 0);
    lv_obj_set_style_border_width(s_field, BORDER_W, 0);
    lv_obj_set_style_radius(s_field, 0, 0);
    lv_obj_set_style_pad_all(s_field, 0, 0);
    lv_obj_set_scrollbar_mode(s_field, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_field, LV_OBJ_FLAG_CLICKABLE);  // presses go to s_body
    lv_obj_add_event_cb(s_field, _onDraw, LV_EVENT_DRAW_MAIN_END, nullptr);

    s_msg = lv_label_create(s_field);
    lv_label_set_text(s_msg, "");
    lv_obj_set_style_text_color(s_msg, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_12, 0);
    lv_obj_align(s_msg, LV_ALIGN_BOTTOM_MID, 0, -8);

    // ── Game-over overlay ─────────────────────────────────────────────
    {
        s_overlay = lv_obj_create(_screen);
        lv_obj_set_size(s_overlay, 188, LV_SIZE_CONTENT);
        lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, TOP_H / 2);
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

        lv_obj_t* msg = lv_label_create(s_overlay);
        lv_label_set_text(msg, "Game Over");
        lv_obj_set_style_text_color(msg, theme::TEXT, 0);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);

        s_overlayScr = lv_label_create(s_overlay);
        lv_label_set_text(s_overlayScr, "");
        lv_obj_set_style_text_color(s_overlayScr, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(s_overlayScr, &lv_font_montserrat_10, 0);

        lv_obj_t* btn = lv_btn_create(s_overlay);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 148, 28);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, _onNewClick, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "New Game");
        lv_obj_set_style_text_color(lbl, theme::BG, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);

        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    s_timer = lv_timer_create(_onTick, STEP_START_MS, nullptr);
    lv_timer_pause(s_timer);
}

// ── show() / isActive() ───────────────────────────────────────────────
void ScreenSnake::show()
{
    if (!_screen) {
        _build();
        _newGame();
    }
    lv_group_focus_obj(s_body);
    lv_scr_load(_screen);
    lv_timer_resume(s_timer);
    _refreshLabels();
    OPS_LOG("UI", "Snake shown, score=%u best=%u",
            (unsigned)s_game.score, (unsigned)s_best);
}

bool ScreenSnake::isActive()
{
    return _screen && (lv_scr_act() == _screen);
}

}}  // namespace ops::ui
