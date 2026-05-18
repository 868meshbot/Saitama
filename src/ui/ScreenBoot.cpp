// Saitama — ScreenBoot.cpp
// Copyright 2026 Saitama — MIT License

#include "ScreenBoot.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../version.h"
#include "../utils/Log.h"

namespace ops { namespace ui {

lv_obj_t* ScreenBoot::_screen = nullptr;

void ScreenBoot::show() {
    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Project name ───────────────────────────────────────────────────
    lv_obj_t* title = lv_label_create(_screen);
    lv_label_set_text(title, "Saitama");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -32);

    // ── Tagline ────────────────────────────────────────────────────────
    lv_obj_t* sub = lv_label_create(_screen);
    lv_label_set_text(sub, "LoRa Mesh Terminal");
    lv_obj_set_style_text_color(sub, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_10, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -12);

    // ── Version ────────────────────────────────────────────────────────
    lv_obj_t* ver = lv_label_create(_screen);
    lv_label_set_text(ver, "v" OPS_VERSION_STRING);
    lv_obj_set_style_text_color(ver, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_10, 0);
    lv_obj_align(ver, LV_ALIGN_CENTER, 0, 4);

    // ── Spinner (rotating arc animation) ─────────────────────────────
    // lv_spinner_create() sets up an arc widget with a built-in rotation
    // animation. We style the indicator arc (moving part) and the track.
    lv_obj_t* spin = lv_spinner_create(_screen, 1000, 60);
    lv_obj_set_size(spin, 36, 36);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, 42);
    lv_obj_set_style_arc_color(spin, theme::ACCENT,   LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 4,               LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, theme::BG_CARD,  LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 4,               LV_PART_MAIN);

    // ── Make active, then schedule transition to launcher ──────────────
    lv_scr_load(_screen);

    // One-shot timer: 2500 ms → show launcher
    lv_timer_t* t = lv_timer_create(_onTimerDone, 2500, nullptr);
    lv_timer_set_repeat_count(t, 1);

    OPS_LOG("UI", "Boot screen ready");
}

void ScreenBoot::_onTimerDone(lv_timer_t* /*t*/) {
    // Boot screen is done — transition to the main launcher.
    // The boot screen object stays in memory harmlessly; PSRAM is plentiful.
    ScreenLauncher::show();
}

}}  // namespace ops::ui
