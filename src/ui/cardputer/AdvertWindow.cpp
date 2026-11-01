// Saitama — cardputer/AdvertWindow.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "AdvertWindow.h"
#include "../Theme.h"
#include "../ScreenSettings.h"
#include "../../utils/AutoAdvert.h"
#include "../../utils/Log.h"
#include <lvgl.h>

namespace ops { namespace ui {

// One button per auto-advert interval: Enter picks it and closes. No status
// lines, timer or drop-down — just the setting (adverts can still be sent by
// hand with the Terminal "advert" command).

static lv_obj_t* s_modal = nullptr;

static const char* const kLabels[autoadvert::NUM_CHOICES] = {
    "Off", "Every 3 hours", "Every 6 hours", "Every 12 hours", "Every 24 hours"
};

static void _close()
{
    if (s_modal) lv_obj_del_async(s_modal);   // runs from its own button / key
    s_modal = nullptr;
    ScreenSettings::show();   // refresh the Advert row's value
}

static void _onPick(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!autoadvert::setChoice(idx))
        OPS_LOG("Advert", "auto advert setting not saved (SD?)");
    _close();
}

static void _onKey(lv_event_t* e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC) _close();
}

void openAdvertWindow()
{
    if (s_modal) return;
    const int cur = autoadvert::choice();

    // 135 px: title 16 + 5 buttons x 18 + gaps = about 120 px.
    s_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, theme::BG, 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 4, 0);
    lv_obj_set_style_pad_row(s_modal, 3, 0);
    lv_obj_set_flex_flow(s_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_modal, _onKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* title = lv_label_create(s_modal);
    lv_label_set_text(title, LV_SYMBOL_UPLOAD " Auto advert");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_group_t* g = lv_group_get_default();
    lv_obj_t* focus = nullptr;
    for (int i = 0; i < autoadvert::NUM_CHOICES; i++) {
        bool on = (i == cur);
        lv_obj_t* b = lv_btn_create(s_modal);
        lv_obj_set_size(b, lv_pct(100), 18);
        lv_obj_set_style_bg_color(b, on ? theme::PRIMARY : theme::BG_CARD, 0);
        lv_obj_set_style_border_color(b, on ? theme::ACCENT : theme::BORDER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, _onPick, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(b, _onKey, LV_EVENT_KEY, nullptr);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, kLabels[i]);
        lv_obj_set_style_text_color(l, on ? theme::TEXT : theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_center(l);
        if (g) lv_group_add_obj(g, b);
        if (on) focus = b;
    }
    if (g && focus) lv_group_focus_obj(focus);
}

}}  // namespace ops::ui
