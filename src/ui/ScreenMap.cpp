// Saitama — ScreenMap.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScreenMap.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../map/MapEngine.h"
#include "../mesh/MeshService.h"
#include "../hardware/Board.h"
#include "../utils/Log.h"
#include "../utils/SDCard.h"
#include "../utils/Contacts.h"
#include "../utils/Repeaters.h"
#include "../utils/Config.h"

#include <lvgl.h>
#include <math.h>
#include <cstring>
#include <cstdio>

namespace ops { namespace ui {

// ── Layout constants ────────────────────────────────────────────────
static constexpr int TOP_H   = 28;
static constexpr int ZOOM_W  = 0;    // no strip — zoom buttons float over canvas
static constexpr int MAP_W   = OPS_SCREEN_W - ZOOM_W;  // 320
static constexpr int MAP_H   = OPS_SCREEN_H - TOP_H;   // 212
static constexpr int TILE_SZ = 256;
static constexpr int ZOOM_MIN = 1;
static constexpr int ZOOM_MAX = 12;
static constexpr int ZOOM_DEF = 10;

// Tiles range from 103 bytes (blank) to ~29 KB (dense city tiles).
// Allocated once in PSRAM via _build().
static constexpr size_t PNG_BUF_SZ = 32768;
static uint8_t* s_pngBuf = nullptr;

// ── Static members ──────────────────────────────────────────────────
lv_obj_t*   ScreenMap::_screen       = nullptr;
lv_obj_t*   ScreenMap::_canvas       = nullptr;
lv_color_t* ScreenMap::_canvasBuf    = nullptr;
lv_obj_t*   ScreenMap::_zoomLbl      = nullptr;
lv_obj_t*   ScreenMap::_mountOverlay = nullptr;

float ScreenMap::_centerLat  = 51.5f;   // London fallback before GPS fix
float ScreenMap::_centerLng  = -0.12f;
int   ScreenMap::_zoom       = ZOOM_DEF;
bool  ScreenMap::_gpsCentered = false;

// ── File-scope helpers ───────────────────────────────────────────────
static ops::MapEngine s_eng;

// Marker colors
static constexpr lv_color_t PURPLE = LV_COLOR_MAKE(148, 0, 211);
static const     lv_color_t BLUE   = theme::ACCENT;
static constexpr lv_color_t GREEN  = LV_COLOR_MAKE(0, 200, 80);

// Blit one decoded tile (3 bytes/px: lo, hi RGB565, alpha) into the canvas buffer.
static void _blitTile(lv_color_t* buf,
                      const uint8_t* decoded,
                      int drawX, int drawY)
{
    int colStart = (drawX < 0)      ? -drawX       : 0;
    int colEnd   = (drawX + TILE_SZ > MAP_W) ? MAP_W - drawX : TILE_SZ;
    if (colStart >= colEnd) return;

    for (int row = 0; row < TILE_SZ; row++)
    {
        int cy = drawY + row;
        if (cy < 0 || cy >= MAP_H) continue;

        const uint8_t* srcRow = decoded + row * TILE_SZ * 3 + colStart * 3;
        lv_color_t*    dstRow = buf + cy * MAP_W + (drawX + colStart);

        for (int col = colStart; col < colEnd; col++)
        {
            dstRow->full = (uint16_t)srcRow[0] | ((uint16_t)srcRow[1] << 8);
            dstRow++;
            srcRow += 3;
        }
    }
}

// Draw a filled circle marker and name label on the canvas.
static void _drawMarker(lv_obj_t* canvas, int px, int py,
                        lv_color_t color, const char* name)
{
    static constexpr int R = 5;
    if (px < -R || px >= MAP_W + R || py < -R || py >= MAP_H + R) return;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color     = color;
    dsc.bg_opa       = LV_OPA_COVER;
    dsc.radius       = LV_RADIUS_CIRCLE;
    dsc.border_width = 1;
    dsc.border_color = lv_color_white();
    dsc.border_opa   = LV_OPA_70;
    lv_canvas_draw_rect(canvas, px - R, py - R, R * 2, R * 2, &dsc);

    if (name && name[0])
    {
        lv_draw_label_dsc_t ldsc;
        lv_draw_label_dsc_init(&ldsc);
        ldsc.color = color;
        ldsc.font  = &lv_font_montserrat_10;
        // Place label to the right of the dot, vertically centred on it
        lv_canvas_draw_text(canvas, px + R + 2, py - 5, 90, &ldsc, name);
    }
}

// ── show() ───────────────────────────────────────────────────────────
void ScreenMap::show()
{
    // Snap to GPS on first fix
    if (!_gpsCentered)
    {
        auto& b = Board::instance();
        if (b.hasGPSFix())
        {
            float lat = b.gpsLat();
            float lng = b.gpsLng();
            if (lat != 0.0f || lng != 0.0f)
            {
                _centerLat  = lat;
                _centerLng  = lng;
                _gpsCentered = true;
            }
        }
    }

    if (!_screen)
    {
        s_eng.init();   // idempotent: just checks SD.exists("/maps")
        _build();
    }

    _updateZoomLabel();
    _redrawMap();
    lv_scr_load(_screen);
    OPS_LOG("Map", "Map screen shown z=%d lat=%.4f lng=%.4f",
            _zoom, (double)_centerLat, (double)_centerLng);
}

bool ScreenMap::isActive()
{
    return _screen && (lv_scr_act() == _screen);
}

void ScreenMap::navigate(int dxPx, int dyPx)
{
    float n = powf(2.0f, (float)_zoom);
    float degPerPx = 360.0f / (n * (float)TILE_SZ);
    _centerLng += dxPx * degPerPx;
    _centerLat -= dyPx * degPerPx;
    if (_centerLat >  85.0f) _centerLat =  85.0f;
    if (_centerLat < -85.0f) _centerLat = -85.0f;
    _redrawMap();
}

// ── _build() ─────────────────────────────────────────────────────────
void ScreenMap::_build()
{
    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ──────────────────────────────────────────────────────
    lv_obj_t* bar = lv_obj_create(_screen);
    lv_obj_set_size(bar, OPS_SCREEN_W, TOP_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_set_style_pad_ver(bar, 2, 0);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto mkBarBtn = [&](const char* label, lv_event_cb_t cb) -> lv_obj_t*
    {
        lv_obj_t* btn = lv_btn_create(bar);
        lv_group_remove_obj(btn);
        lv_obj_set_height(btn, TOP_H - 6);
        lv_obj_set_style_bg_color(btn, theme::BG, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, theme::BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_hor(btn, 6, 0);
        if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, theme::ACCENT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
        return btn;
    };

    mkBarBtn(LV_SYMBOL_HOME, _onHomeClick);

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, LV_SYMBOL_IMAGE " Map");
    lv_obj_set_style_text_color(title, theme::TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_flex_grow(title, 1);

    mkBarBtn(LV_SYMBOL_CLOSE, _onExitClick);

    // ── Map canvas (left, below top bar) ─────────────────────────────
    _canvasBuf = (lv_color_t*)ps_malloc(MAP_W * MAP_H * sizeof(lv_color_t));
    if (!_canvasBuf)
    {
        OPS_LOG("Map", "FATAL: cannot allocate canvas buffer");
        return;
    }
    s_pngBuf = (uint8_t*)ps_malloc(PNG_BUF_SZ);
    if (!s_pngBuf)
    {
        OPS_LOG("Map", "FATAL: cannot allocate PNG decode buffer");
        return;
    }
    memset(_canvasBuf, 0, MAP_W * MAP_H * sizeof(lv_color_t));

    _canvas = lv_canvas_create(_screen);
    lv_canvas_set_buffer(_canvas, _canvasBuf, MAP_W, MAP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(_canvas, 0, TOP_H);
    lv_group_remove_obj(_canvas);

    // Key handler on canvas for backspace → exit
    lv_obj_add_event_cb(_canvas, _onKey, LV_EVENT_KEY, nullptr);
    lv_group_add_obj(lv_group_get_default(), _canvas);
    lv_group_focus_obj(_canvas);

    // Touch drag → pan. LV_OBJ_FLAG_CLICKABLE is required for the canvas
    // (lv_img subclass) to receive pointer events.
    lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_canvas, _onTouch, LV_EVENT_PRESSING, nullptr);


    // ── Floating zoom buttons ────────────────────────────────────────────
    // Sit over the map canvas bottom-right; right edge x=226 clears the GT911
    // hardware deadzone (py_max≈231 → screen x>231 unreachable).
    _zoomLbl = nullptr;  // no zoom label; _updateZoomLabel() guards on null

    auto mkZoomBtn = [&](const char* label, lv_coord_t bx, lv_coord_t by, lv_event_cb_t cb)
    {
        lv_obj_t* btn = lv_btn_create(_screen);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 44, 28);
        lv_obj_set_pos(btn, bx, by);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
    };

    // x=242 → right edge=286; y positions at bottom of map area
    mkZoomBtn("+", 242, OPS_SCREEN_H - 4 - 28 - 4 - 28, _onZoomIn);
    mkZoomBtn("-", 242, OPS_SCREEN_H - 4 - 28,           _onZoomOut);
}

// ── _redrawMap() ──────────────────────────────────────────────────────
void ScreenMap::_redrawMap()
{
    if (!_canvas || !_canvasBuf) return;

    // Fill background (dark slate — visible for missing tiles)
    lv_canvas_fill_bg(_canvas, theme::BG, LV_OPA_COVER);

    if (!s_eng.hasTileDir())
    {
        lv_obj_invalidate(_canvas);
        _showMountDialog();
        return;
    }
    // Dismiss any lingering mount dialog if tiles are now available
    if (_mountOverlay)
    {
        lv_obj_del(_mountOverlay);
        _mountOverlay = nullptr;
    }

    // Fractional tile coords of map center
    float txF, tyF;
    ops::MapEngine::latLngToTileFrac(_centerLat, _centerLng, _zoom, txF, tyF);

    int   txC  = (int)txF;
    int   tyC  = (int)tyF;
    float offX = (txF - txC) * (float)TILE_SZ;  // pixel offset of center within tile
    float offY = (tyF - tyC) * (float)TILE_SZ;

    // Canvas position of the top-left corner of the center tile
    int baseX = MAP_W / 2 - (int)offX;
    int baseY = MAP_H / 2 - (int)offY;

    int maxTile = (1 << _zoom);

    // Render up to 3×3 surrounding tiles (only the ones actually intersecting the canvas)
    for (int dty = -1; dty <= 1; dty++)
    {
        for (int dtx = -1; dtx <= 1; dtx++)
        {
            int tx    = txC + dtx;
            int ty    = tyC + dty;
            int drawX = baseX + dtx * TILE_SZ;
            int drawY = baseY + dty * TILE_SZ;

            if (drawX + TILE_SZ <= 0 || drawX >= MAP_W) continue;
            if (drawY + TILE_SZ <= 0 || drawY >= MAP_H) continue;
            if (tx < 0 || ty < 0 || tx >= maxTile || ty >= maxTile) continue;

            size_t pngSz = s_eng.readTile(_zoom, tx, ty, s_pngBuf, PNG_BUF_SZ);
            if (pngSz == 0) continue;

            // Decode PNG via LVGL's built-in LodePNG decoder.
            // After decode (16-bit depth), img_data is 3 bytes/px: [lo, hi, alpha].
            lv_img_dsc_t src;
            memset(&src, 0, sizeof(src));
            src.header.cf  = LV_IMG_CF_TRUE_COLOR_ALPHA;
            src.header.w   = TILE_SZ;
            src.header.h   = TILE_SZ;
            src.data       = s_pngBuf;
            src.data_size  = (uint32_t)pngSz;

            lv_img_decoder_dsc_t dec;
            memset(&dec, 0, sizeof(dec));
            lv_res_t res = lv_img_decoder_open(&dec, &src, lv_color_make(0, 0, 0), 0);
            if (res == LV_RES_OK && dec.img_data)
            {
                _blitTile(_canvasBuf,
                          (const uint8_t*)dec.img_data,
                          drawX, drawY);
            }
            else
            {
                OPS_LOG("Map", "Tile %d/%d/%d decode failed (sz=%u)",
                        _zoom, tx, ty, (unsigned)pngSz);
            }
            lv_img_decoder_close(&dec);
        }
    }

    // ── Overlay markers ───────────────────────────────────────────────
    // Helper: convert stored int32_t lat/lon to canvas pixel coords.
    auto markerPx = [&](int32_t latI, int32_t lonI, int& px, int& py) -> bool
    {
        if (latI == 0 && lonI == 0) return false;
        float mlat = (float)latI / 1000000.0f;
        float mlng = (float)lonI / 1000000.0f;
        float mtxF, mtyF;
        ops::MapEngine::latLngToTileFrac(mlat, mlng, _zoom, mtxF, mtyF);
        px = MAP_W / 2 + (int)((mtxF - txF) * (float)TILE_SZ);
        py = MAP_H / 2 + (int)((mtyF - tyF) * (float)TILE_SZ);
        return true;
    };

    // Saved repeaters → purple
    for (int i = 0; i < ops::repeaters::count(); i++)
    {
        ops::Repeater r;
        if (!ops::repeaters::get(i, r)) continue;
        int px, py;
        if (markerPx(r.lat, r.lon, px, py))
            _drawMarker(_canvas, px, py, PURPLE, r.name);
    }

    // Saved contacts → blue
    for (int i = 0; i < ops::contacts::count(); i++)
    {
        ops::Contact c;
        if (!ops::contacts::get(i, c)) continue;
        int px, py;
        if (markerPx(c.lat, c.lon, px, py))
            _drawMarker(_canvas, px, py, BLUE, c.name);
    }

    // Live peers not yet in the NVS stores (freshly heard this session)
    auto& svc = ops::MeshService::instance();
    for (int i = 0; i < svc.peerCount(); i++)
    {
        ops::PeerInfo p;
        if (!svc.getPeer(i, p)) continue;
        if (p.lat == 0 && p.lon == 0) continue;
        // Skip if already plotted via NVS store
        int dummy;
        if (p.type == 2 && ops::repeaters::findByKey(p.pubKeyPrefix, &dummy)) continue;
        if (p.type != 2 && ops::contacts::findByKey(p.pubKeyPrefix, &dummy)) continue;
        int px, py;
        if (markerPx(p.lat, p.lon, px, py))
            _drawMarker(_canvas, px, py, (p.type == 2) ? PURPLE : BLUE, p.name);
    }

    // Self position → green dot with callsign
    {
        auto& b = Board::instance();
        float selfLat = 0.0f, selfLng = 0.0f;
        if (b.hasGPSFix())
        {
            selfLat = b.gpsLat();
            selfLng = b.gpsLng();
        }
        else
        {
            const auto& cfg = ops::config::get();
            selfLat = cfg.manualLat;
            selfLng = cfg.manualLon;
        }
        if (selfLat != 0.0f || selfLng != 0.0f)
        {
            int px, py;
            if (markerPx((int32_t)(selfLat * 1000000.0f),
                         (int32_t)(selfLng * 1000000.0f), px, py))
                _drawMarker(_canvas, px, py, GREEN,
                            ops::config::get().callsign);
        }
    }

    lv_obj_invalidate(_canvas);
}

// ── _updateZoomLabel() ────────────────────────────────────────────────
void ScreenMap::_updateZoomLabel()
{
    if (!_zoomLbl) return;
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", _zoom);
    lv_label_set_text(_zoomLbl, buf);
}

// ── Button callbacks ─────────────────────────────────────────────────
void ScreenMap::_onHomeClick(lv_event_t*)
{
    ScreenLauncher::show();
}

void ScreenMap::_onExitClick(lv_event_t*)
{
    ScreenLauncher::show();
}

void ScreenMap::_onZoomIn(lv_event_t*)
{
    if (_zoom >= ZOOM_MAX) return;
    _zoom++;
    _updateZoomLabel();
    _redrawMap();
}

void ScreenMap::_onZoomOut(lv_event_t*)
{
    if (_zoom <= ZOOM_MIN) return;
    _zoom--;
    _updateZoomLabel();
    _redrawMap();
}

void ScreenMap::_onKey(lv_event_t* e)
{
    uint32_t* key = (uint32_t*)lv_event_get_param(e);
    if (key && (*key == LV_KEY_ESC))
        ScreenLauncher::show();
}

void ScreenMap::_onTouch(lv_event_t* /*e*/)
{
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    // Negate: dragging finger right moves the map right (center shifts west).
    if (vect.x != 0 || vect.y != 0)
        navigate(-vect.x, -vect.y);
}

// ── SD remount dialog ─────────────────────────────────────────────────
void ScreenMap::_showMountDialog()
{
    if (_mountOverlay) return;  // already visible

    // Semi-transparent dim layer over the whole screen
    _mountOverlay = lv_obj_create(_screen);
    lv_obj_set_size(_mountOverlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(_mountOverlay, 0, 0);
    lv_obj_set_style_bg_color(_mountOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_mountOverlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_mountOverlay, 0, 0);
    lv_obj_set_style_pad_all(_mountOverlay, 0, 0);
    lv_obj_clear_flag(_mountOverlay, LV_OBJ_FLAG_SCROLLABLE);

    // Dialog box
    lv_obj_t* box = lv_obj_create(_mountOverlay);
    lv_obj_set_size(box, 230, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, -ZOOM_W / 2, 0);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::BORDER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(box);
    lv_label_set_text(title, LV_SYMBOL_WARNING " No Map Tiles");
    lv_obj_set_style_text_color(title, theme::TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

    lv_obj_t* body = lv_label_create(box);
    lv_label_set_text(body,
        "SD card not mounted or\n"
        "/maps/osm/ not found.\n"
        "Insert card and tap Mount.");
    lv_obj_set_style_text_color(body, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_10, 0);
    lv_obj_set_width(body, 206);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);

    // Button row
    lv_obj_t* row = lv_obj_create(box);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto mkBtn = [&](const char* label, lv_color_t bg, lv_event_cb_t cb) -> lv_obj_t*
    {
        lv_obj_t* btn = lv_btn_create(row);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 90, 30);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, bg.full == theme::BG.full ? 1 : 0, 0);
        lv_obj_set_style_border_color(btn, theme::BORDER, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
        return btn;
    };

    mkBtn("Cancel",            theme::BG,      _onMountCancel);
    mkBtn(LV_SYMBOL_REFRESH " Mount", theme::PRIMARY, _onMountClick);
}

void ScreenMap::_onMountClick(lv_event_t*)
{
    if (_mountOverlay)
    {
        lv_obj_del(_mountOverlay);
        _mountOverlay = nullptr;
    }
    OPS_LOG("Map", "Attempting SD remount");
    ops::sdcard::init();   // re-runs SPI + SD.begin(); safe to call again
    s_eng.init();           // re-checks /maps/osm existence
    _redrawMap();           // shows dialog again if still not found
}

void ScreenMap::_onMountCancel(lv_event_t*)
{
    ScreenLauncher::show();
}

}}  // namespace ops::ui
