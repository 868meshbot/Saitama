// Saitama — cardputer/AdvertWindow.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "AdvertWindow.h"
#include "../Theme.h"
#include "../ScreenSettings.h"
#include "../../mesh/MeshService.h"
#include "../../utils/AutoAdvert.h"
#include "../../utils/Config.h"
#include "../../utils/Log.h"
#include <lvgl.h>
#include <stdio.h>
#include <time.h>

namespace ops { namespace ui {

static lv_obj_t*   s_modal   = nullptr;
static lv_obj_t*   s_status  = nullptr;   // last sent + repeaters that answered
static lv_obj_t*   s_next    = nullptr;   // next auto advert countdown
static lv_obj_t*   s_dd      = nullptr;
static lv_timer_t* s_timer   = nullptr;
static uint32_t    s_sentAt  = 0;         // unix time of the last Send here

static void _close()
{
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_modal) lv_obj_del_async(s_modal);
    s_modal = s_status = s_next = s_dd = nullptr;
    ScreenSettings::show();   // refresh the Advert row's value
}

static void _refresh()
{
    if (!s_modal) return;
    if (s_status) {
        char buf[64];
        if (s_sentAt == 0) {
            snprintf(buf, sizeof(buf), "Not sent from here yet");
        } else {
            // Repeaters heard since the advert went out.
            auto& svc = MeshService::instance();
            int n = 0;
            for (int i = 0; i < svc.peerCount(); i++) {
                PeerInfo p;
                if (svc.getPeer(i, p) && p.type == 2 && p.lastSeen >= s_sentAt) n++;
            }
            time_t t = (time_t)s_sentAt + (config::localEpoch() - time(nullptr));
            struct tm tm;
            gmtime_r(&t, &tm);
            snprintf(buf, sizeof(buf), "Sent %02d:%02d, %d repeater%s heard",
                     tm.tm_hour, tm.tm_min, n, n == 1 ? "" : "s");
        }
        lv_label_set_text(s_status, buf);
    }
    if (s_next) {
        uint32_t s = autoadvert::secondsUntilNext();
        char buf[40];
        if (autoadvert::choice() <= 0) snprintf(buf, sizeof(buf), "Auto advert off");
        else snprintf(buf, sizeof(buf), "Next auto advert in %luh %02lum",
                      (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
        lv_label_set_text(s_next, buf);
    }
}

static void _send(bool flood)
{
    s_sentAt = (uint32_t)time(nullptr);
    bool ok = MeshService::instance().sendAdvert(0, flood);
    OPS_LOG("Advert", "%s advert %s", flood ? "Flood" : "Zero-hop", ok ? "sent" : "failed");
    _refresh();
}

static void _onZeroHop(lv_event_t*) { _send(false); }
static void _onFlood(lv_event_t*)   { _send(true); }
static void _onClose(lv_event_t*)   { _close(); }

static void _onAuto(lv_event_t*)
{
    if (!s_dd) return;
    autoadvert::setChoice((int)lv_dropdown_get_selected(s_dd));
    _refresh();
}

static void _onKey(lv_event_t* e)
{
    uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_ESC) _close();
}

static lv_obj_t* _btn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, lv_color_t col)
{
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_set_size(b, lv_pct(100), 20);
    lv_obj_set_style_bg_color(b, theme::BG, 0);
    lv_obj_set_style_bg_color(b, theme::PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(b, col, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 1, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(b, _onKey, LV_EVENT_KEY, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t* _label(lv_obj_t* parent, lv_color_t col)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    return l;
}

void openAdvertWindow()
{
    if (s_modal) return;

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
    lv_obj_add_event_cb(s_modal, _onKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* title = lv_label_create(s_modal);
    lv_label_set_text(title, LV_SYMBOL_UPLOAD " Advert");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    s_status = _label(s_modal, theme::TEXT);

    lv_obj_t* zh = _btn(s_modal, "Send zero-hop", _onZeroHop, theme::ACCENT);
    lv_obj_t* fl = _btn(s_modal, "Send flood",    _onFlood,   theme::ACCENT);

    lv_obj_t* row = lv_obj_create(s_modal);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* autoLbl = lv_label_create(row);
    lv_label_set_text(autoLbl, "Auto");
    lv_obj_set_style_text_color(autoLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(autoLbl, &lv_font_montserrat_12, 0);
    s_dd = lv_dropdown_create(row);
    lv_dropdown_set_options(s_dd, autoadvert::OPTIONS);
    lv_dropdown_set_selected(s_dd, (uint16_t)autoadvert::choice());
    lv_obj_set_width(s_dd, 120);
    lv_obj_set_style_text_font(s_dd, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_ver(s_dd, 2, 0);
    lv_obj_add_event_cb(s_dd, _onAuto, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_dd, _onKey, LV_EVENT_KEY, nullptr);
    lv_obj_t* ddList = lv_dropdown_get_list(s_dd);
    lv_obj_set_style_text_font(ddList, &lv_font_montserrat_12, 0);

    s_next = _label(s_modal, theme::TEXT_MUTED);

    lv_obj_t* cl = _btn(s_modal, "Close", _onClose, theme::TEXT_MUTED);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, zh);
        lv_group_add_obj(g, fl);
        lv_group_add_obj(g, s_dd);
        lv_group_add_obj(g, cl);
        lv_group_focus_obj(zh);
    }

    _refresh();
    s_timer = lv_timer_create([](lv_timer_t*) { _refresh(); }, 2000, nullptr);
}

}}  // namespace ops::ui
