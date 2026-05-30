// Saitama — UIScreen.cpp
// Copyright 2026 Saitama — MIT License
//
// Initialises LVGL 8, the TFT_eSPI display driver, and manages the
// top-level screen lifecycle:  Boot → Launcher → (app screens).

#include "UIScreen.h"
#include "ScreenBoot.h"
#include "ScreenLauncher.h"
#include "ScreenHome.h"
#include "ScreenHeard.h"
#include "ScreenTerminal.h"
#include "ScreenTrace.h"
#include "ScreenFinder.h"
#include "ScreenRepeaters.h"
#include "ScreenMap.h"
#include "ScreenPower.h"
#include "Theme.h"
#include "../hardware/Board.h"
#include "../mesh/MeshService.h"
#include "../utils/Config.h"
#include "../utils/Contacts.h"
#include "../utils/Log.h"
#include "../utils/Sound.h"
#include "../utils/Keymap.h"
#include "../utils/SDCard.h"
#include "../utils/GpsMgr.h"
#include "Emoji.h"

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <cmath>

// ── CPU governor ───────────────────────────────────────────────────
// Frequency table [governor 0-3][state: 0=active, 1=screensaver, 2=screen-off]
// All values are clamped to ≥80 MHz in _applyGovFreq() — OPI PSRAM on the
// ESP32-S3 crashes below 80 MHz when the clock source switches to XTAL.
//   0 Power Save : 80 / 80 / 80 MHz  (table says 40 but clamp applies)
//   1 Medium     : 80 / 80 / 80 MHz  (table says 40 but clamp applies)
//   2 Normal     : 240 / 80 / 80 MHz  ← default
//   3 Turbo      : 240 / 240 / 240 MHz
static constexpr uint32_t kGovFreq[4][3] = {
    {  40,  40,  40 },
    {  80,  40,  40 },
    { 240,  80,  80 },
    { 240, 240, 240 },
};
// 0=active, 1=screensaver, 2=screen-off
static void _applyGovFreq(uint8_t state)
{
    uint8_t gov = ops::config::get().cpuGovernor;
    if (gov > 3) gov = 2;
    uint32_t freq = kGovFreq[gov][state < 3 ? state : 0];
    // Hard floor at 80 MHz — two reasons:
    //   1. ESP32-S3 OPI PSRAM is unstable below 80 MHz (XTAL clock source crashes on PSRAM access).
    //   2. I2S APB clock requires ≥80 MHz while audio DMA is draining.
    if (freq < 80) freq = 80;
    setCpuFrequencyMhz(freq);
}

static TFT_eSPI tft = TFT_eSPI();

// LVGL 8 display driver — double-buffered 20-line strips in DMA-capable SRAM.
// Same approach as RatDeck (working T-Deck Plus reference).
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_disp_t*         s_disp    = nullptr;
static lv_color_t*        s_buf1    = nullptr;
static lv_color_t*        s_buf2    = nullptr;

static constexpr uint32_t BUF_PIXELS = OPS_SCREEN_W * 40;  // 40-line strip

// LVGL 8 indev driver (trackball → encoder for non-launcher screens)
static lv_indev_drv_t s_indev_drv;
static lv_indev_t*    s_indev = nullptr;

static int16_t enc_diff = 0;

// Screensaver & notification popup state (file-scope so touch_read can access)
static uint32_t  s_lastActivityMs    = 0;
static bool      s_screensaverActive = false;
static lv_obj_t* s_screensaverScreen = nullptr;
static lv_obj_t* s_ssTimeLbl         = nullptr;
static lv_obj_t* s_ssNameLbl         = nullptr;
static lv_obj_t* s_prevScreen        = nullptr;
static lv_obj_t* s_notifyPopup       = nullptr;
static bool      s_touchDismiss      = false;  // set in touch_read, consumed in tick()
static bool      s_screenOff         = false;  // backlight fully off (beyond screensaver)
static bool      s_govApplyPending   = false;  // deferred setCpuFrequencyMhz(), consumed in tick()
static bool      s_ssAnalog          = false;  // true = analog clock screensaver
static lv_obj_t* s_ssCanvas          = nullptr;
static void*     s_ssCanvasBuf       = nullptr;

// Touch calibration
static constexpr lv_coord_t kCalTx1 = 40,  kCalTy1 = 40;
static constexpr lv_coord_t kCalTx2 = 200, kCalTy2 = 200;
static uint8_t    s_calState       = 0;   // 0=idle 1=point1 2=point2
static lv_obj_t*  s_calOverlay     = nullptr;
static lv_obj_t*  s_calMsgLbl      = nullptr;
static lv_obj_t*  s_calCross       = nullptr;
static lv_coord_t s_calPx1         = 0;
static lv_coord_t s_calPy1         = 0;
static bool       s_calTapReady    = false;
static lv_coord_t s_calRawPx       = 0;
static lv_coord_t s_calRawPy       = 0;
static bool       s_calPrevPressed = false;
static uint32_t   s_calDismissAt   = 0;

// LVGL 8 touch indev (GT911 capacitive)
static lv_indev_drv_t s_touch_drv;
static lv_indev_t*    s_touch    = nullptr;
static uint8_t        s_gt911    = 0x5D;  // resolved by gt911_probe() at init
static bool           s_touchOk  = false;

// Probe GT911 at 0x5D then 0x14; read product ID to confirm. Logs result.
static void gt911_probe() {
    for (uint8_t addr : {(uint8_t)0x5D, (uint8_t)0x14}) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission(true) != 0) continue;

        // Product ID is 4 ASCII bytes at 0x8140 (should be "911\0")
        Wire.beginTransmission(addr);
        Wire.write(0x81); Wire.write(0x40);
        Wire.endTransmission(true);
        char pid[5] = {};
        if (Wire.requestFrom(addr, (uint8_t)4) == 4) {
            for (int i = 0; i < 4; i++) pid[i] = Wire.read();
        }
        while (Wire.available()) Wire.read();

        s_gt911   = addr;
        s_touchOk = true;
        OPS_LOG("Touch", "GT911 at 0x%02X  id=\"%s\"", addr, pid);
        return;
    }
    OPS_LOG("Touch", "GT911 not found on I2C — touch disabled");
}

static void touch_read(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    static lv_coord_t last_x = 0, last_y = 0;

    if (!s_touchOk) {
        data->point.x = last_x; data->point.y = last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Read buffer-status register 0x814E (STOP+START — repeated-start unreliable on ESP32)
    Wire.beginTransmission(s_gt911);
    Wire.write(0x81); Wire.write(0x4E);
    if (Wire.endTransmission(true) != 0) {
        data->point.x = last_x; data->point.y = last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (Wire.requestFrom(s_gt911, (uint8_t)1) < 1 || !Wire.available()) {
        data->point.x = last_x; data->point.y = last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    uint8_t status = Wire.read();

    // Clear status register so GT911 latches fresh data next poll
    Wire.beginTransmission(s_gt911);
    Wire.write(0x81); Wire.write(0x4E); Wire.write((uint8_t)0);
    Wire.endTransmission(true);

    if (!(status & 0x80) || (status & 0x0F) == 0) {
        data->point.x = last_x; data->point.y = last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        s_calPrevPressed = false;  // allow next physical touch to be treated as a new tap
        return;
    }

    // Read first touch point: 7 bytes starting at 0x814F (NOT 0x8150 — common off-by-one).
    // Layout: [track_id, x_low, x_high[3:0], y_low, y_high[3:0], size_low, size_high]
    Wire.beginTransmission(s_gt911);
    Wire.write(0x81); Wire.write(0x4F);
    Wire.endTransmission(true);
    if (Wire.requestFrom(s_gt911, (uint8_t)7) >= 7 && Wire.available() >= 7) {
        Wire.read();  // track id
        uint8_t xL = Wire.read(), xH = Wire.read();
        uint8_t yL = Wire.read(), yH = Wire.read();
        Wire.read(); Wire.read();  // size bytes
        while (Wire.available()) Wire.read();
        // GT911 reports portrait coords (X: 0-240, Y: 0-320).
        // TFT setRotation(1) is landscape: portrait-Y → screen-X, portrait-X → screen-Y mirrored.
        lv_coord_t px = (lv_coord_t)(((uint16_t)(xH & 0x0F) << 8) | xL);
        lv_coord_t py = (lv_coord_t)(((uint16_t)(yH & 0x0F) << 8) | yL);

        // Calibration mode: capture raw portrait coords on rising edge, suppress LVGL touch.
        if (s_calState != 0) {
            if (!s_calPrevPressed) {
                s_calRawPx = px;
                s_calRawPy = py;
                s_calTapReady = true;
            }
            s_calPrevPressed = true;
            data->point.x = last_x; data->point.y = last_y;
            data->state   = LV_INDEV_STATE_RELEASED;
            return;
        }
        s_calPrevPressed = false;

        // Apply calibration transform (identity defaults: XScale=1 XOff=0 YScale=1 YOff=0).
        const auto& tcfg = ops::config::get();
        lv_coord_t rx = (lv_coord_t)(tcfg.touchCalXOff + (float)py * tcfg.touchCalXScale);
        lv_coord_t ry = (lv_coord_t)(tcfg.touchCalYOff + (float)((OPS_SCREEN_H - 1) - px) * tcfg.touchCalYScale);
        last_x = (rx < 0) ? 0 : (rx >= OPS_SCREEN_W) ? OPS_SCREEN_W - 1 : rx;
        last_y = (ry < 0) ? 0 : (ry >= OPS_SCREEN_H) ? OPS_SCREEN_H - 1 : ry;
    }

    data->point.x = last_x;
    data->point.y = last_y;
    data->state = LV_INDEV_STATE_PRESSED;
    if (s_screensaverActive)
        s_touchDismiss = true;
    else
        s_lastActivityMs = millis();
}

// LVGL 8 flush callback
static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushPixels((uint16_t*)color_p, w * h);
    tft.endWrite();
    lv_disp_flush_ready(drv);
}

// LVGL 8 encoder read callback — only reports rotation; press is dispatched directly
// so LVGL never enters textarea edit mode (which would trap trackball navigation).
static void encoder_read(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    data->enc_diff = enc_diff;
    data->state    = LV_INDEV_STATE_RELEASED;
    enc_diff = 0;
}


// ---------- Touch calibration helpers ----------

static void _calBuildCross(lv_coord_t tx, lv_coord_t ty)
{
    if (s_calCross) { lv_obj_del(s_calCross); s_calCross = nullptr; }
    const lv_coord_t SZ = 40;
    s_calCross = lv_obj_create(s_calOverlay);
    lv_obj_set_size(s_calCross, SZ, SZ);
    lv_obj_set_pos(s_calCross, tx - SZ / 2, ty - SZ / 2);
    lv_obj_set_style_bg_opa(s_calCross, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_calCross, lv_color_make(255, 80, 80), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_calCross, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_calCross, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_calCross, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_calCross, LV_OBJ_FLAG_SCROLLABLE);

    // Horizontal arm
    lv_obj_t* hArm = lv_obj_create(s_calCross);
    lv_obj_set_size(hArm, SZ - 4, 2);
    lv_obj_align(hArm, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(hArm, lv_color_make(255, 80, 80), LV_PART_MAIN);
    lv_obj_set_style_border_width(hArm, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hArm, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hArm, LV_OBJ_FLAG_SCROLLABLE);

    // Vertical arm
    lv_obj_t* vArm = lv_obj_create(s_calCross);
    lv_obj_set_size(vArm, 2, SZ - 4);
    lv_obj_align(vArm, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(vArm, lv_color_make(255, 80, 80), LV_PART_MAIN);
    lv_obj_set_style_border_width(vArm, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(vArm, 0, LV_PART_MAIN);
    lv_obj_clear_flag(vArm, LV_OBJ_FLAG_SCROLLABLE);
}

static void _calBuildOverlay(lv_coord_t tx, lv_coord_t ty, const char* msg)
{
    s_calOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_calOverlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_calOverlay, 0, 0);
    lv_obj_set_style_bg_color(s_calOverlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_calOverlay, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_calOverlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_calOverlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_calOverlay, LV_OBJ_FLAG_SCROLLABLE);

    s_calMsgLbl = lv_label_create(s_calOverlay);
    lv_obj_set_style_text_font(s_calMsgLbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_calMsgLbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_width(s_calMsgLbl, OPS_SCREEN_W - 20);
    lv_obj_set_style_text_align(s_calMsgLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_calMsgLbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_calMsgLbl, msg);
    lv_obj_align(s_calMsgLbl, LV_ALIGN_TOP_MID, 0, 8);

    _calBuildCross(tx, ty);
}

static void _calDismiss()
{
    s_calState = 0;
    if (s_calOverlay) { lv_obj_del(s_calOverlay); s_calOverlay = nullptr; }
    s_calMsgLbl = nullptr;
    s_calCross  = nullptr;
}

namespace ops { namespace ui {

// ---------- Screensaver helpers ----------

static constexpr lv_coord_t SS_CANVAS_W = 180;
static constexpr lv_coord_t SS_CANVAS_H = 180;
static constexpr lv_coord_t SS_CLOCK_CX = 90;
static constexpr lv_coord_t SS_CLOCK_CY = 90;
static constexpr lv_coord_t SS_CLOCK_R  = 75;

static void _drawAnalogClock()
{
    if (!s_ssCanvas || !s_ssCanvasBuf) return;
    lv_canvas_fill_bg(s_ssCanvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = lv_color_white();
    arc.width = 2;
    arc.opa   = LV_OPA_COVER;
    lv_canvas_draw_arc(s_ssCanvas, SS_CLOCK_CX, SS_CLOCK_CY, SS_CLOCK_R, 0, 360, &arc);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_white();
    line.opa   = LV_OPA_COVER;
    for (int i = 0; i < 12; i++) {
        float ang  = i * (float)(2.0 * M_PI / 12.0);
        float outr = (float)(SS_CLOCK_R - 2);
        float inr  = (float)(SS_CLOCK_R - (i % 3 == 0 ? 10 : 6));
        lv_point_t pts[2] = {
            { (lv_coord_t)(SS_CLOCK_CX + outr * sinf(ang)), (lv_coord_t)(SS_CLOCK_CY - outr * cosf(ang)) },
            { (lv_coord_t)(SS_CLOCK_CX + inr  * sinf(ang)), (lv_coord_t)(SS_CLOCK_CY - inr  * cosf(ang)) }
        };
        line.width = (i % 3 == 0) ? 2 : 1;
        lv_canvas_draw_line(s_ssCanvas, pts, 2, &line);
    }

    time_t t = ops::config::localEpoch();
    struct tm lt;
    gmtime_r(&t, &lt);
    float h_ang = ((lt.tm_hour % 12) * 60 + lt.tm_min) * (float)(2.0 * M_PI / 720.0);
    float m_ang = lt.tm_min * (float)(2.0 * M_PI / 60.0);

    float hr = SS_CLOCK_R * 0.55f;
    line.width = 3;
    lv_point_t hpts[2] = {
        { SS_CLOCK_CX, SS_CLOCK_CY },
        { (lv_coord_t)(SS_CLOCK_CX + hr * sinf(h_ang)), (lv_coord_t)(SS_CLOCK_CY - hr * cosf(h_ang)) }
    };
    lv_canvas_draw_line(s_ssCanvas, hpts, 2, &line);

    float mr = SS_CLOCK_R * 0.80f;
    line.width = 2;
    lv_point_t mpts[2] = {
        { SS_CLOCK_CX, SS_CLOCK_CY },
        { (lv_coord_t)(SS_CLOCK_CX + mr * sinf(m_ang)), (lv_coord_t)(SS_CLOCK_CY - mr * cosf(m_ang)) }
    };
    lv_canvas_draw_line(s_ssCanvas, mpts, 2, &line);

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color     = lv_color_white();
    dot.bg_opa       = LV_OPA_COVER;
    dot.radius       = LV_RADIUS_CIRCLE;
    dot.border_width = 0;
    lv_canvas_draw_rect(s_ssCanvas, SS_CLOCK_CX - 3, SS_CLOCK_CY - 3, 6, 6, &dot);
}

static void _updateSsTime()
{
    if (s_ssAnalog) { _drawAnalogClock(); return; }
    if (!s_ssTimeLbl) return;
    time_t t = ops::config::localEpoch();
    struct tm lt;
    gmtime_r(&t, &lt);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    lv_label_set_text(s_ssTimeLbl, buf);
}

static void _deactivateScreensaver()
{
    if (!s_screensaverActive) return;
    s_screensaverActive = false;
    s_screenOff         = false;
    s_lastActivityMs    = millis();
    lv_obj_t* sav       = s_screensaverScreen;
    s_screensaverScreen = nullptr;
    s_ssTimeLbl         = nullptr;
    s_ssNameLbl         = nullptr;
    s_ssCanvas          = nullptr;
    s_ssAnalog          = false;
    if (s_prevScreen) {
        lv_scr_load(s_prevScreen);
        s_prevScreen = nullptr;
    }
    if (sav) lv_obj_del(sav);
    if (s_ssCanvasBuf) { free(s_ssCanvasBuf); s_ssCanvasBuf = nullptr; }
    Board::instance().setDisplayBrightness(
        (uint8_t)ops::config::get().brightness);
    _applyGovFreq(0);  // restore active-screen frequency
}

static void _activateScreensaver(bool analog = false)
{
    if (s_screensaverActive) return;
    s_ssAnalog          = analog;
    s_prevScreen        = lv_scr_act();
    s_screensaverScreen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screensaverScreen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screensaverScreen, LV_OPA_COVER, LV_PART_MAIN);

    if (analog) {
        s_ssCanvasBuf = ps_malloc((size_t)SS_CANVAS_W * SS_CANVAS_H * sizeof(lv_color_t));
        s_ssCanvas    = lv_canvas_create(s_screensaverScreen);
        lv_canvas_set_buffer(s_ssCanvas, s_ssCanvasBuf,
                             SS_CANVAS_W, SS_CANVAS_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(s_ssCanvas, LV_ALIGN_TOP_MID, 0, 15);
        _drawAnalogClock();

        s_ssNameLbl = lv_label_create(s_screensaverScreen);
        lv_obj_set_style_text_font(s_ssNameLbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_ssNameLbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_ssNameLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_ssNameLbl, LV_ALIGN_BOTTOM_MID, 0, -8);
        const char* name = ops::config::get().callsign[0]
                           ? ops::config::get().callsign : "OPS-NODE";
        lv_label_set_text(s_ssNameLbl, name);
    } else {
        // Flex-column container so time + name centre as a single group
        lv_obj_t* col = lv_obj_create(s_screensaverScreen);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(col);

        s_ssTimeLbl = lv_label_create(col);
        lv_obj_set_style_text_font(s_ssTimeLbl, &lv_font_montserrat_36, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_ssTimeLbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_ssTimeLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        _updateSsTime();

        s_ssNameLbl = lv_label_create(col);
        lv_obj_set_style_text_font(s_ssNameLbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_ssNameLbl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_ssNameLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        const char* name = ops::config::get().callsign[0]
                           ? ops::config::get().callsign : "OPS-NODE";
        lv_label_set_text(s_ssNameLbl, name);
    }

    lv_scr_load(s_screensaverScreen);
    s_screensaverActive = true;
    s_screenOff         = false;
    _applyGovFreq(1);  // screensaver frequency
    // Dim to ~12% of configured brightness (min 20) — clock stays readable.
    int cfg_b = ops::config::get().brightness;
    uint8_t ssDim = (uint8_t)((cfg_b / 5) < 20 ? 20 : cfg_b / 5);
    Board::instance().setDisplayBrightness(ssDim);
}

// ---------- Notification popup helpers ----------

static void _onNotifyPopupClick(lv_event_t* /*e*/)
{
    if (!s_notifyPopup) return;
    lv_obj_t* pop = s_notifyPopup;
    s_notifyPopup = nullptr;
    lv_obj_del(pop);
}

static void _showNotifyPopup(const ops::RxMessage& msg)
{
    if (s_screensaverActive) _deactivateScreensaver();

    if (s_notifyPopup) {
        lv_obj_del(s_notifyPopup);
        s_notifyPopup = nullptr;
    }

    lv_obj_t* parent = lv_scr_act();
    s_notifyPopup = lv_obj_create(parent);
    lv_obj_set_size(s_notifyPopup, 260, 100);
    lv_obj_align(s_notifyPopup, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_color(s_notifyPopup, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_notifyPopup, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_notifyPopup, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_notifyPopup, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_notifyPopup, 6, LV_PART_MAIN);
    lv_obj_add_flag(s_notifyPopup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_notifyPopup, _onNotifyPopupClick, LV_EVENT_CLICKED, nullptr);

    char hdr[64];
    if (msg.isDirect)
        snprintf(hdr, sizeof(hdr), "DM from %s", msg.senderName);
    else
        snprintf(hdr, sizeof(hdr), "#%s  %s",
                 msg.channelName[0] ? msg.channelName : "?", msg.senderName);

    lv_obj_t* hdrLbl = lv_label_create(s_notifyPopup);
    lv_obj_set_style_text_font(hdrLbl, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hdrLbl, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_label_set_text(hdrLbl, hdr);
    lv_obj_set_width(hdrLbl, 244);
    lv_label_set_long_mode(hdrLbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(hdrLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* txtLbl = lv_label_create(s_notifyPopup);
    lv_obj_set_style_text_font(txtLbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(txtLbl, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(txtLbl, msg.text);
    lv_obj_set_width(txtLbl, 244);
    lv_label_set_long_mode(txtLbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(txtLbl, LV_ALIGN_TOP_LEFT, 0, 20);

    lv_obj_t* hint = lv_label_create(s_notifyPopup);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_label_set_text(hint, "tap to dismiss");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

// -----------------------------------------

void init() {
    OPS_LOG("UI", "Initialising display");

    // TFT init
    tft.begin();
    tft.setRotation(1);
    tft.setSwapBytes(true);  // LVGL outputs little-endian RGB565; SPI needs big-endian
    tft.fillScreen(TFT_BLACK);

    // Switch GPIO 42 from digital to ledc PWM so brightness can be dimmed.
    // Board::init() drove the pin HIGH via digitalWrite; ledcAttachPin re-routes
    // the pin mux to the ledc peripheral, overriding that.
    Board::instance().initBacklightPWM();
    Board::instance().setDisplayBrightness(
        (uint8_t)ops::config::get().brightness);
    _applyGovFreq(0);  // start at active-screen frequency

    lv_init();
    ops::emoji::init();

    // Allocate double-buffered 20-line strips — DMA SRAM first, PSRAM fallback
    s_buf1 = (lv_color_t*)heap_caps_malloc(BUF_PIXELS * sizeof(lv_color_t),
                                            MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_buf2 = (lv_color_t*)heap_caps_malloc(BUF_PIXELS * sizeof(lv_color_t),
                                            MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_buf1) s_buf1 = (lv_color_t*)ps_malloc(BUF_PIXELS * sizeof(lv_color_t));
    if (!s_buf2) s_buf2 = (lv_color_t*)ps_malloc(BUF_PIXELS * sizeof(lv_color_t));

    if (!s_buf1 || !s_buf2) {
        OPS_LOG("UI", "FATAL: cannot allocate display buffers");
        return;
    }

    // Register LVGL 8 display driver
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, BUF_PIXELS);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = OPS_SCREEN_W;
    s_disp_drv.ver_res  = OPS_SCREEN_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp = lv_disp_drv_register(&s_disp_drv);

    // Register LVGL 8 encoder indev (trackball)
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_ENCODER;
    s_indev_drv.read_cb = encoder_read;
    s_indev = lv_indev_drv_register(&s_indev_drv);

    // Register GT911 touch indev
    lv_indev_drv_init(&s_touch_drv);
    s_touch_drv.type    = LV_INDEV_TYPE_POINTER;
    s_touch_drv.read_cb = touch_read;
    s_touch = lv_indev_drv_register(&s_touch_drv);

    lv_group_t* g = lv_group_create();
    lv_group_set_default(g);   // screens use lv_group_get_default() to add widgets
    lv_indev_set_group(s_indev, g);

    // Apply theme
    theme::apply(s_disp);

    // Probe GT911 (sets s_gt911 address, s_touchOk flag, logs product ID)
    gt911_probe();

    // Initialise GPS power manager (applies starting mode from config).
    ops::GpsMgr::instance().init();

    // Show boot screen — auto-transitions to launcher after 2.5 s
    ScreenBoot::show();

    s_lastActivityMs = millis();
    OPS_LOG("UI", "Display ready (%dx%d)", OPS_SCREEN_W, OPS_SCREEN_H);
}

void tick() {
    auto& board  = Board::instance();
    uint32_t now = millis();

    // GPS power management state machine
    ops::GpsMgr::instance().tick();

    // Touch dismiss flag set by touch_read when screensaver is active
    if (s_touchDismiss) {
        s_touchDismiss = false;
        if (s_screensaverActive) _deactivateScreensaver();
    }

    // Trackball: consume delta + press first so we can intercept for screensaver.
    int16_t dx, dy;
    board.consumeTrackballDelta(dx, dy);
    bool tbPress = board.consumeTrackballPress();

    if (s_screensaverActive) {
        // Any trackball activity wakes the screen; inputs are otherwise discarded.
        if (dx || dy || tbPress) _deactivateScreensaver();
    } else if (s_calState != 0) {
        // Calibration active — keep display awake, swallow all trackball input.
        if (dx || dy || tbPress) s_lastActivityMs = now;
    } else {
        if (dx || dy || tbPress) s_lastActivityMs = now;

        if (ScreenLauncher::isActive()) {
            int ndx = (dx > 0) ? 1 : (dx < 0) ? -1 : 0;
            int ndy = (dy > 0) ? 1 : (dy < 0) ? -1 : 0;
            if (ndx || ndy) ScreenLauncher::navigate(ndx, ndy);
            if (tbPress) ScreenLauncher::confirmSelect();
        } else if (ScreenMap::isActive()) {
            // Route raw trackball delta directly to map pan (8 px per encoder tick).
            if (dx || dy) ScreenMap::navigate(dx * 8, dy * 8);
        } else {
            // dy (up/down) always changes group focus.
            // dx (left/right) moves a focused slider, or changes focus otherwise.
            if (dy != 0 || dx != 0) {
                lv_group_t* ng    = lv_group_get_default();
                lv_obj_t*   nf    = ng ? lv_group_get_focused(ng) : nullptr;
                bool onSlider = nf && lv_obj_has_class(nf, &lv_slider_class);
                if (dy != 0) {
                    enc_diff = (dy > 0) ? 1 : -1;
                }
                if (dx != 0) {
                    if (onSlider) {
                        uint32_t sk = (dx > 0) ? LV_KEY_RIGHT : LV_KEY_LEFT;
                        lv_event_send(nf, LV_EVENT_KEY, &sk);
                    } else {
                        enc_diff = (dx > 0) ? 1 : -1;
                    }
                }
            }
            // Press: dispatch directly to focused widget — never route through the encoder
            // indev, which would toggle LVGL edit mode and trap focus inside a textarea.
            if (tbPress) {
                lv_group_t* pg = lv_group_get_default();
                lv_obj_t*   pf = pg ? lv_group_get_focused(pg) : nullptr;
                if (pf) {
                    uint32_t ek = LV_KEY_ENTER;
                    lv_event_send(pf, LV_EVENT_KEY, &ek);
                    if (!lv_obj_is_editable(pf))
                        lv_event_send(pf, LV_EVENT_CLICKED, nullptr);
                }
            }
        }
    }

    // Feed BBQ10 keyboard — rate-limited to 50 ms (I2C at 30 FPS floods the Wire error log).
    static uint32_t lastKbPoll = 0;
    if (now - lastKbPoll >= 50UL) {
        lastKbPoll = now;
        char k = 0;
        if (board.pollKeyboard(k)) {
            auto xlat   = ops::keymap::translate(k, ops::config::get().kbLayout, now);
            k           = xlat.ch;
            bool kbRepl = xlat.replace;
            if (s_screensaverActive) {
                _deactivateScreensaver();
            } else if (s_calState != 0) {
                // During calibration: backspace/ESC cancels; all other keys swallowed.
                if (k == 8 || k == 127 || k == 27) {
                    _calDismiss();
                    ScreenTerminal::appendLine("Touch calibration cancelled.");
                }
            } else {
                s_lastActivityMs = now;
                if (k == '\r' || k == '\n') {
                    if (ScreenLauncher::isActive()) {
                        ScreenLauncher::confirmSelect();
                    } else {
                        // Send Enter directly to focused widget — bypasses the LVGL group
                        // editing-mode intercept that eats LV_KEY_ENTER before it reaches
                        // the textarea's key handler (which would fire LV_EVENT_READY).
                        lv_group_t* g = lv_group_get_default();
                        lv_obj_t* foc = g ? lv_group_get_focused(g) : nullptr;
                        if (foc) {
                            uint32_t ek = LV_KEY_ENTER;
                            lv_event_send(foc, LV_EVENT_KEY, &ek);
                            if (!lv_obj_is_editable(foc))
                                lv_event_send(foc, LV_EVENT_CLICKED, nullptr);
                        }
                    }
                } else if (k != '\0' && !ScreenLauncher::isActive()) {
                    // All other keys: send directly to focused widget, bypassing group/editing-mode.
                    // '\0' means a rejected/suppressed key (e.g. hardware extended byte) — skip event.
                    // BACKSPACE on non-empty textarea deletes char; on empty textarea or button → ESC (back).
                    lv_group_t* grp = lv_group_get_default();
                    lv_obj_t*   foc = grp ? lv_group_get_focused(grp) : nullptr;
                    if (foc) {
                        // Accent cycle: erase the previous variant before inserting the next.
                        if (kbRepl && lv_obj_has_class(foc, &lv_textarea_class)) {
                            uint32_t bk = LV_KEY_BACKSPACE;
                            lv_event_send(foc, LV_EVENT_KEY, &bk);
                        }

                        if ((uint8_t)k > 127) {
                            // Non-ASCII from the accent cycle.
                            // LVGL's lv_textarea_add_char() stores the codepoint as a raw LE
                            // byte sequence, producing broken UTF-8 that crashes the text
                            // renderer.  Use lv_textarea_add_text() with a proper 2-byte
                            // UTF-8 string instead (Latin-1 Supplement: U+0080–U+00FF).
                            if (lv_obj_has_class(foc, &lv_textarea_class) &&
                                !lv_textarea_get_accepted_chars(foc) &&
                                lv_textarea_get_max_length(foc) == 0) {
                                uint8_t b = (uint8_t)k;
                                char utf8[3] = {
                                    (char)(0xC0u | (b >> 6)),
                                    (char)(0x80u | (b & 0x3Fu)),
                                    '\0'
                                };
                                lv_textarea_add_text(foc, utf8);
                            }
                            // else: constrained textarea — silently skip accented char
                        } else {
                            uint32_t key;
                            if (k == 8 || k == 127) {
                                if (lv_obj_has_class(foc, &lv_textarea_class)) {
                                    const char* txt = lv_textarea_get_text(foc);
                                    key = (txt && txt[0] != '\0') ? LV_KEY_BACKSPACE : LV_KEY_ESC;
                                } else {
                                    key = LV_KEY_ESC;
                                }
                            } else if (k == 27) {
                                key = LV_KEY_ESC;
                            } else {
                                key = (uint32_t)(uint8_t)k;
                            }
                            lv_event_send(foc, LV_EVENT_KEY, &key);
                        }
                    }
                }
            }
        }
    }

    // Activate screensaver after configured idle period.
    {
        int timeoutSec = ops::config::get().screenTimeoutSec;
        if (!s_screensaverActive && timeoutSec > 0 && s_lastActivityMs > 0) {
            if (now - s_lastActivityMs >= (uint32_t)timeoutSec * 1000UL)
                _activateScreensaver();
        }
    }

    // Screen off: cut backlight entirely after screenOffMin minutes of screensaver.
    if (s_screensaverActive && !s_screenOff) {
        uint8_t offMin = ops::config::get().screenOffMin;
        if (offMin >= 2) {
            if (now - s_lastActivityMs >= (uint32_t)offMin * 60000UL) {
                s_screenOff = true;
                Board::instance().setDisplayBrightness(0);
                _applyGovFreq(2);  // screen-off frequency
            }
        }
    }

    // Drain incoming mesh messages every tick.
    // appendLine() echoes every line to CDC serial via Serial.println().
    {
        ops::RxMessage msg;
        int n = 0;
        while (n < 8 && ops::MeshService::instance().dequeueMessage(msg)) {
            char line[220];
            if (msg.isDirect) {
                snprintf(line, sizeof(line),
                         "[DM] %s: %s  (h:%u rssi:%.0fdBm snr:%.0f)",
                         msg.senderName, msg.text, msg.hops,
                         (double)msg.rssi, (double)msg.snr);
                int cidx;
                if (ops::contacts::findByKey(msg.pubKeyPrefix, &cidx))
                    ops::contacts::setUnread(cidx, true);
                ScreenLauncher::refreshUnreadDot();
                ops::sound::playNotification();
                lv_refr_now(nullptr);
            } else {
                snprintf(line, sizeof(line),
                         "[#%s] %s: %s  (h:%u rssi:%.0fdBm snr:%.0f)",
                         msg.channelName[0] ? msg.channelName : "?",
                         msg.senderName, msg.text, msg.hops,
                         (double)msg.rssi, (double)msg.snr);
            }
            ScreenTerminal::appendLine(line);
            ScreenHome::appendMessage(msg);
            if (ops::config::get().notifyPopup) _showNotifyPopup(msg);
            n++;
        }
        ScreenHome::checkPendingAck();
    }

    ScreenTrace::tick();
    ScreenFinder::tick();
    ScreenPower::tick();

    // Drain discover results → ScreenFinder
    {
        ops::DiscoverEntry de;
        while (ops::MeshService::instance().pollDiscoverResult(de))
            ScreenFinder::addDiscoverResult(de);
    }

    // Drain async contact responses → terminal + repeater admin panel (if open).
    {
        char respLine[132];
        while (ops::MeshService::instance().pollContactResponse(respLine, sizeof(respLine))) {
            ScreenTerminal::appendLine(respLine);
            ScreenRepeaters::onContactResponse(respLine);
        }
    }

    // Handle repeater admin login result + 15-second timeout.
    ScreenRepeaters::tickLoginResult();

    // Refresh the Heard screen whenever any peer is added or updated.
    // peerSerial() increments on every _upsertPeer call (new node or RSSI update),
    // so this catches DM senders and path updates, not only adverts.
    {
        static uint32_t lastPeerSerial = 0;
        uint32_t cur = ops::MeshService::instance().peerSerial();
        if (cur != lastPeerSerial) {
            lastPeerSerial = cur;
            ScreenHeard::onPeersUpdated();
            ScreenLauncher::onAdvertPeersUpdated();
        }
    }

    // Periodic clock / battery refresh; also tick screensaver clock.
    static uint32_t lastClock = 0;
    static uint32_t lastBatt  = 0;
    if (now - lastClock >= 30000UL) {
        lastClock = now;
        ScreenLauncher::refreshClock();
        if (s_screensaverActive) _updateSsTime();
    }
    if (now - lastBatt >= 60000UL) {
        lastBatt = now;
        int  battPct = board.batteryPercent();
        bool chg     = board.batteryCharging();
        OPS_LOG("BATT", "pct=%d charging=%d", battPct, (int)chg);
        ScreenLauncher::refreshBattery(battPct, chg);
        ScreenLauncher::refreshSpeaker(ops::config::get().speakerEnabled);
        ScreenLauncher::refreshStatus(
            ops::config::get().gpsMode,
            board.hasGPSFix(),
            (int)board.gpsSatellites());
        ScreenLauncher::refreshRadio(
            ops::MeshService::instance().initialized(),
            ops::MeshService::instance().isActive());
    }

    // Retry SD mount if it failed at boot: try every 30 s, give up after 2 attempts.
    {
        static uint32_t s_sdRetryAt = 0;
        static int      s_sdTries   = 0;
        if (!ops::sdcard::isMounted() && s_sdTries < 2) {
            if (s_sdRetryAt == 0) {
                s_sdRetryAt = now + 30000UL;  // schedule first retry
            } else if (now >= s_sdRetryAt) {
                s_sdTries++;
                OPS_LOG("SD", "Auto-mount attempt %d/2", s_sdTries);
                if (!ops::sdcard::tryMount())
                    s_sdRetryAt = now + 30000UL;  // schedule next retry
                else
                    s_sdRetryAt = 0;  // mounted — no more retries needed
            }
        }
    }

    // Touch calibration state machine
    if (s_calState != 0 && s_calTapReady) {
        s_calTapReady = false;
        if (s_calState == 1) {
            // Point 1 captured — advance to point 2.
            s_calPx1 = s_calRawPx;
            s_calPy1 = s_calRawPy;
            s_calState = 2;
            if (s_calMsgLbl)
                lv_label_set_text(s_calMsgLbl, "Tap the crosshair (2/2)");
            _calBuildCross(kCalTx2, kCalTy2);
        } else if (s_calState == 2) {
            lv_coord_t px2 = s_calRawPx, py2 = s_calRawPy;
            // Guard: both points must be distinct on both axes.
            lv_coord_t dpx = s_calPx1 - px2;  // should be non-zero (Y axis denominator)
            lv_coord_t dpy = py2 - s_calPy1;  // should be non-zero (X axis denominator)
            if (dpx == 0 || dpy == 0) {
                if (s_calMsgLbl)
                    lv_label_set_text(s_calMsgLbl, "Bad tap (same point?)\nTap crosshair (2/2)");
            } else {
                float xScale = (float)(kCalTx2 - kCalTx1) / (float)dpy;
                float xOff   = (float)kCalTx1 - xScale * (float)s_calPy1;
                float yScale = (float)(kCalTy2 - kCalTy1) / (float)((OPS_SCREEN_H - 1 - px2) - (OPS_SCREEN_H - 1 - s_calPx1));
                float yOff   = (float)kCalTy1 - yScale * (float)(OPS_SCREEN_H - 1 - s_calPx1);
                if (xScale < 0.5f || xScale > 2.0f || yScale < 0.5f || yScale > 2.0f) {
                    // Wildly off — restart from point 1.
                    s_calState = 1;
                    if (s_calMsgLbl)
                        lv_label_set_text(s_calMsgLbl, "Out of range — retry\nTap crosshair (1/2)");
                    _calBuildCross(kCalTx1, kCalTy1);
                } else {
                    ops::config::setTouchCal(xScale, xOff, yScale, yOff);
                    s_calState = 0;
                    if (s_calCross) { lv_obj_del(s_calCross); s_calCross = nullptr; }
                    char buf[80];
                    snprintf(buf, sizeof(buf),
                             "Calibration saved!\nXS=%.3f XO=%.1f\nYS=%.3f YO=%.1f",
                             (double)xScale, (double)xOff,
                             (double)yScale, (double)yOff);
                    if (s_calMsgLbl) lv_label_set_text(s_calMsgLbl, buf);
                    s_calDismissAt = millis() + 3000;
                }
            }
        }
    }
    if (s_calDismissAt && millis() >= s_calDismissAt) {
        s_calDismissAt = 0;
        _calDismiss();
        ScreenTerminal::appendLine("Touch calibration complete.");
    }

    // Apply deferred governor change outside LVGL callbacks
    if (s_govApplyPending) {
        s_govApplyPending = false;
        if (s_screensaverActive) _applyGovFreq(1);
        else                     _applyGovFreq(0);
    }

    // Drive LVGL at ~30 FPS
    static uint32_t lastLvgl = 0;
    if (now - lastLvgl >= 33UL) {
        lastLvgl = now;
        lv_timer_handler();
    }
}

void showLauncher() {
    ScreenLauncher::show();
}

void applyGovernorNow() {
    s_govApplyPending = true;
}

void activateScreensaver(bool analog)
{
    _activateScreensaver(analog);
}

void startTouchCalibration()
{
    if (s_screensaverActive) _deactivateScreensaver();
    _calDismiss();  // clean up any previous session
    s_calState       = 1;
    s_calTapReady    = false;
    s_calPrevPressed = false;
    s_calDismissAt   = 0;
    _calBuildOverlay(kCalTx1, kCalTy1, "Touch calibration\nTap the crosshair (1/2)");
}

}}  // namespace ops::ui
