// Saitama — QRPopup.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "QRPopup.h"
#include "Theme.h"
#include "../utils/Log.h"
#include <qrcode.h>
#include <Arduino.h>
#include <cstring>

namespace ops { namespace ui {

static constexpr uint8_t QR_VERSION = 6;
static constexpr int     QR_SCALE   = 4;
static constexpr int     QR_MODULES = 4 * QR_VERSION + 17;   // 41
static constexpr int     QR_PX      = QR_MODULES * QR_SCALE; // 164

static lv_color_t* s_qrBuf = nullptr;

static void _onQrClose(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_del(overlay);
}

void showQrPopup(const char* title, const char* data)
{
    if (!s_qrBuf)
        s_qrBuf = (lv_color_t*)ps_malloc(QR_PX * QR_PX * sizeof(lv_color_t));
    if (!s_qrBuf) {
        OPS_LOG("QR", "ps_malloc failed for canvas buffer");
        return;
    }

    QRCode qr;
    uint8_t buf[qrcode_getBufferSize(QR_VERSION)];
    if (qrcode_initText(&qr, buf, QR_VERSION, ECC_LOW, data) != 0) {
        OPS_LOG("QR", "qrcode_initText failed — data too long?");
        return;
    }

    // Dim overlay (tap to close)
    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, _onQrClose, LV_EVENT_CLICKED, overlay);

    // White panel (QR codes require white background)
    lv_obj_t* panel = lv_obj_create(overlay);
    lv_obj_set_width(panel, QR_PX + 16);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, OPS_SCREEN_H - 4, 0);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 5, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title label
    lv_obj_t* titleLbl = lv_label_create(panel);
    lv_label_set_text(titleLbl, title);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(titleLbl, QR_PX);
    lv_obj_set_style_text_color(titleLbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(titleLbl, LV_TEXT_ALIGN_CENTER, 0);

    // QR canvas
    lv_obj_t* canvas = lv_canvas_create(panel);
    lv_canvas_set_buffer(canvas, s_qrBuf, QR_PX, QR_PX, LV_COLOR_FORMAT_NATIVE);
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.bg_color = lv_color_black();
    rdsc.bg_opa   = LV_OPA_COVER;
    rdsc.radius   = 0;

    {
        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);
        for (uint8_t y = 0; y < qr.size; y++) {
            for (uint8_t x = 0; x < qr.size; x++) {
                if (qrcode_getModule(&qr, x, y)) {
                    lv_area_t a = {
                        (lv_coord_t)(x * QR_SCALE),
                        (lv_coord_t)(y * QR_SCALE),
                        (lv_coord_t)(x * QR_SCALE + QR_SCALE - 1),
                        (lv_coord_t)(y * QR_SCALE + QR_SCALE - 1)
                    };
                    lv_draw_rect(&layer, &rdsc, &a);
                }
            }
        }
        lv_canvas_finish_layer(canvas, &layer);
    }

    // Close button
    lv_obj_t* closeBtn = lv_btn_create(panel);
    lv_group_remove_obj(closeBtn);
    lv_obj_set_size(closeBtn, QR_PX, 24);
    lv_obj_set_style_bg_color(closeBtn, LV_COLOR_MAKE(0x33, 0x33, 0x33), 0);
    lv_obj_set_style_bg_color(closeBtn, LV_COLOR_MAKE(0x55, 0x55, 0x55), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(closeBtn, 0, 0);
    lv_obj_set_style_radius(closeBtn, 4, 0);
    lv_obj_set_style_shadow_width(closeBtn, 0, 0);
    lv_obj_add_event_cb(closeBtn, _onQrClose, LV_EVENT_CLICKED, overlay);
    lv_obj_t* closeLbl = lv_label_create(closeBtn);
    lv_label_set_text(closeLbl, "Close");
    lv_obj_set_style_text_color(closeLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(closeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(closeLbl);

    OPS_LOG("QR", "Shown: %s", title);
}

}}  // namespace ops::ui
