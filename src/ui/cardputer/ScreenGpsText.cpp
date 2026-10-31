// Saitama — cardputer/ScreenGpsText.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenGpsText.h"
#include "../ScreenLauncher.h"
#include "../Theme.h"
#include "../../hardware/Board.h"
#include "../../utils/Config.h"
#include "../../utils/Log.h"
#include <lvgl.h>
#include <stdio.h>

namespace ops { namespace ui {

static lv_obj_t*   s_screen = nullptr;
static lv_obj_t*   s_body   = nullptr;
static lv_obj_t*   s_text   = nullptr;
static lv_timer_t* s_timer  = nullptr;

static void _refresh()
{
    if (!s_text) return;
    auto& b = Board::instance();
    const auto& cfg = config::get();
    static const char* kModes[] = { "Off", "Intermittent", "On" };

    char buf[320];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "Mode     %s\n", kModes[cfg.gpsMode < 3 ? cfg.gpsMode : 2]);
    if (b.hasGPSFix())
        n += snprintf(buf + n, sizeof(buf) - n, "Fix      yes  %u sats  HDOP %.1f\n",
                      (unsigned)b.gpsSatellites(), (double)b.gpsHdop());
    else
        n += snprintf(buf + n, sizeof(buf) - n, "Fix      no   %u sats\n", (unsigned)b.gpsSatellites());
    if (b.hasGPSFix()) {
        n += snprintf(buf + n, sizeof(buf) - n, "Position %.5f, %.5f\n",
                      (double)b.gpsLat(), (double)b.gpsLng());
        n += snprintf(buf + n, sizeof(buf) - n, "Altitude %.0f m\n", (double)b.gpsAltM());
    } else if (cfg.manualLat != 0.0f || cfg.manualLon != 0.0f) {
        n += snprintf(buf + n, sizeof(buf) - n, "Manual   %.5f, %.5f\n",
                      (double)cfg.manualLat, (double)cfg.manualLon);
    }
    uint16_t y; uint8_t mo, d, h, mi, s;
    if (b.gpsDateTime(y, mo, d, h, mi, s) && y >= 2024)
        n += snprintf(buf + n, sizeof(buf) - n, "UTC      %04u-%02u-%02u %02u:%02u:%02u\n",
                      y, mo, d, h, mi, s);
    uint32_t sync = b.gpsLastSyncMs();
    if (sync)
        n += snprintf(buf + n, sizeof(buf) - n, "Clock    synced %lu min ago\n",
                      (unsigned long)((millis() - sync) / 60000UL));
    else
        n += snprintf(buf + n, sizeof(buf) - n, "Clock    not synced from GPS\n");
    snprintf(buf + n, sizeof(buf) - n, "NMEA     %lu sentences", (unsigned long)b.gpsNmeaCount());
    lv_label_set_text(s_text, buf);
}

static void _onKey(lv_event_t* e)
{
    if (lv_event_get_key(e) != LV_KEY_ESC) return;
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    ScreenLauncher::show();
}

void ScreenGpsText::show()
{
    if (!s_screen) {
        s_screen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(s_screen, theme::BG, 0);
        lv_obj_set_style_pad_all(s_screen, 0, 0);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* title = lv_label_create(s_screen);
        lv_label_set_text(title, LV_SYMBOL_GPS " GPS");
        lv_obj_set_style_text_color(title, theme::ACCENT, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);

        s_body = lv_obj_create(s_screen);
        lv_obj_set_size(s_body, OPS_SCREEN_W, OPS_SCREEN_H - 20);
        lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, 20);
        lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_body, 0, 0);
        lv_obj_set_style_pad_hor(s_body, 4, 0);
        lv_obj_set_style_pad_ver(s_body, 0, 0);
        lv_obj_add_flag(s_body, LV_OBJ_FLAG_USER_1);   // up/down scroll it
        lv_obj_add_event_cb(s_body, _onKey, LV_EVENT_KEY, nullptr);

        s_text = lv_label_create(s_body);
        lv_obj_set_width(s_text, OPS_SCREEN_W - 8);
        lv_obj_set_style_text_color(s_text, theme::TEXT, 0);
        lv_obj_set_style_text_font(s_text, &lv_font_montserrat_12, 0);
    }

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, s_body);
        lv_group_focus_obj(s_body);
    }
    _refresh();
    if (!s_timer) s_timer = lv_timer_create([](lv_timer_t*) { _refresh(); }, 1000, nullptr);
    lv_scr_load(s_screen);
    OPS_LOG("UI", "GPS shown");
}

}}  // namespace ops::ui
