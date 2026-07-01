// Saitama — ScreenMap.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// NavBoxLib-backed map screen (navboxlib experimental branch).
// Replaces the manual lv_canvas + MapEngine tile renderer with NavBoxLib's
// managed LVGL image-object tile cache and MarkerLayer overlay system.
//
// Tile path format:  /maps/osm/{z}/{x}/{y}.png  (standard OSM slippy map)
// Tile cache:        TILECACHE_SIZE (default 4) tiles in a PSRAM-backed MemPool
// Markers:           NavBoxLib MarkerLayer — single-char label (first letter of name)

#include "ScreenMap.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../mesh/MeshService.h"
#include "../hardware/Board.h"
#include "../utils/Log.h"
#include "../utils/SDCard.h"
#include "../utils/Contacts.h"
#include "../utils/Repeaters.h"
#include "../utils/Config.h"

#include <lvgl.h>
#include <navboxlib/MapRenderer.h>
#include <navboxlib/MapLayer.h>
#include <navboxlib/GeoPoint.h>
#include <SD.h>
#include <cstring>
#include <cstdio>

namespace ops { namespace ui {

// ── Layout ────────────────────────────────────────────────────────────
static constexpr int TOP_H    = 28;
static constexpr int MAP_W    = OPS_SCREEN_W;
static constexpr int MAP_H    = OPS_SCREEN_H - TOP_H;
static constexpr int ZOOM_MIN = 1;
static constexpr int ZOOM_MAX = 18;
static constexpr int ZOOM_DEF = 10;

// PSRAM pool: (TILECACHE_SIZE + 1) tiles × 256×256 × 2 bytes (RGB565)
// Pre-set into mempool_ before begin() so NavBoxLib's alloc() uses PSRAM.
static constexpr size_t POOL_SZ = (TILECACHE_SIZE + 1) * 256 * 256 * 2;

// Marker colours (RGB24)
static constexpr uint32_t COL_REPEATER = 0x9400D3;  // purple
static constexpr uint32_t COL_CONTACT  = 0x24B4D7;  // cyan-blue
static constexpr uint32_t COL_SELF     = 0x00C850;  // green

// Name label records — one lv_label per marker, positioned via renderer.project()
struct NameLabel {
    lv_obj_t* obj;   // label widget parented to _screen
    double    lat;
    double    lon;
};

// ── Forward declarations ──────────────────────────────────────────────
static void _repositionLabels();

// ── File-scope state ──────────────────────────────────────────────────
static MapRenderer s_renderer;
static uint16_t    s_markerIds[64];
static NameLabel   s_nameLabels[64];
static int         s_markerCount = 0;

// ── Static member definitions ─────────────────────────────────────────
lv_obj_t* ScreenMap::_screen       = nullptr;
lv_obj_t* ScreenMap::_zoomLbl      = nullptr;
lv_obj_t* ScreenMap::_mountOverlay = nullptr;

float ScreenMap::_centerLat   = 51.5f;
float ScreenMap::_centerLng   = -0.12f;
int   ScreenMap::_zoom        = ZOOM_DEF;
bool  ScreenMap::_gpsCentered = false;

// ── Tile availability check ───────────────────────────────────────────
static bool _hasTiles()
{
    return ops::sdcard::isMounted() && SD.exists("/maps/osm");
}

// ── show() ───────────────────────────────────────────────────────────
void ScreenMap::show()
{
    if (!_gpsCentered) {
        auto& b = Board::instance();
        if (b.hasGPSFix()) {
            float lat = b.gpsLat(), lng = b.gpsLng();
            if (lat != 0.0f || lng != 0.0f) {
                _centerLat = lat; _centerLng = lng; _gpsCentered = true;
            }
        }
    }

    if (!_screen) _build();

    _refreshMarkers();

    if (!_hasTiles()) {
        _showMountDialog();
    } else {
        if (_mountOverlay) { lv_obj_del(_mountOverlay); _mountOverlay = nullptr; }
        s_renderer.setCenter(GeoPoint(_centerLat, _centerLng), _zoom);
        s_renderer.invalidate();
        _repositionLabels();
    }

    lv_scr_load(_screen);
    OPS_LOG("Map", "NavBoxLib map z=%d lat=%.4f lng=%.4f",
            _zoom, (double)_centerLat, (double)_centerLng);
}

bool ScreenMap::isActive()
{
    return _screen && (lv_scr_act() == _screen);
}

void ScreenMap::tick()
{
    if (!isActive()) return;
    s_renderer.iterate(millis());
}

void ScreenMap::navigate(int dxPx, int dyPx)
{
    s_renderer.panPx((int16_t)dxPx, (int16_t)dyPx);
    _centerLat = (float)s_renderer.lat();
    _centerLng = (float)s_renderer.lon();
    _repositionLabels();
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

    auto mkBarBtn = [&](const char* label, lv_event_cb_t cb)
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
    };

    mkBarBtn(LV_SYMBOL_HOME,  _onHomeClick);

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, LV_SYMBOL_IMAGE " Map");
    lv_obj_set_style_text_color(title, theme::TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_flex_grow(title, 1);

    mkBarBtn(LV_SYMBOL_CLOSE, _onExitClick);

    // ── NavBoxLib renderer ────────────────────────────────────────────
    // Pre-set buf_ from PSRAM before begin() is called so that NavBoxLib's
    // MemPool::alloc() uses PSRAM rather than internal DRAM for tile buffers.
    // (MemPool::init() is not called by begin() internally; the caller owns
    // pool initialisation.)
    {
        uint8_t* pool = (uint8_t*)ps_malloc(POOL_SZ);
        if (pool) {
            s_renderer.mempool_.buf_     = pool;
            s_renderer.mempool_.bufsize_ = POOL_SZ;
            s_renderer.mempool_.bufpos_  = 0;
            OPS_LOG("Map", "NavBoxLib PSRAM pool %u KB", (unsigned)(POOL_SZ / 1024));
        } else {
            OPS_LOG("Map", "WARN: PSRAM pool alloc failed — tiling may OOM");
        }
    }

    bool ok = s_renderer.begin(_screen, MAP_W, MAP_H,
                                "/maps/osm/%d/%d/%d.png");
    if (!ok) {
        OPS_LOG("Map", "NavBoxLib begin() failed — map unavailable");
        return;
    }
    s_renderer.setXY(0, TOP_H);
    s_renderer.colBg_     = 0x1A1A2E;  // dark slate background for missing tiles
    s_renderer.colAccent_ = 0x24B4D7;  // default dot colour (overridden per marker)
    s_renderer.colHome_   = 0x00C850;  // home/self marker colour

    // Attach key and touch events to NavBoxLib's root LVGL object so that
    // the keyboard handler and trackball drag both work while map is active.
    lv_obj_t* base = s_renderer.getLvglBase();
    if (base) {
        lv_obj_add_flag(base, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(base, _onKey,   LV_EVENT_KEY,      nullptr);
        lv_obj_add_event_cb(base, _onTouch, LV_EVENT_PRESSING, nullptr);
        lv_group_add_obj(lv_group_get_default(), base);
        lv_group_focus_obj(base);

        // tilesBase_ (first child of base_) is lv_obj and clickable by default.
        // In LVGL 8 events don't bubble without LV_OBJ_FLAG_EVENT_BUBBLE, so
        // clearing clickable here lets touch fall through to base_ and our handler.
        lv_obj_t* tilesBase = lv_obj_get_child(base, 0);
        if (tilesBase) lv_obj_clear_flag(tilesBase, LV_OBJ_FLAG_CLICKABLE);
    }

    // ── Floating zoom +/- buttons (bottom-right, clear of GT911 deadzone) ─
    auto mkZoomBtn = [&](const char* label, lv_coord_t bx, lv_coord_t by,
                         lv_event_cb_t cb)
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
    mkZoomBtn("+", 242, OPS_SCREEN_H - 4 - 28 - 4 - 28, _onZoomIn);
    mkZoomBtn("-", 242, OPS_SCREEN_H - 4 - 28,           _onZoomOut);
}

// ── _repositionLabels() ───────────────────────────────────────────────
// Called after every pan/zoom to move name labels to current screen coords.
static void _repositionLabels()
{
    for (int i = 0; i < s_markerCount; i++) {
        if (!s_nameLabels[i].obj) continue;
        lv_coord_t px, py;
        bool vis = s_renderer.project(s_nameLabels[i].lat, s_nameLabels[i].lon, px, py);
        if (vis && px >= 0 && px < MAP_W && py >= TOP_H && py < OPS_SCREEN_H) {
            lv_obj_set_pos(s_nameLabels[i].obj, px + 8, py - 5);
            lv_obj_clear_flag(s_nameLabels[i].obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_nameLabels[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ── _refreshMarkers() ─────────────────────────────────────────────────
// Clears the NavBoxLib MarkerLayer and repopulates it from the current
// contacts, repeaters, live peers, and self position.
// Called on every show() — NavBoxLib repositions dots automatically on
// subsequent pan/zoom; name labels are repositioned via _repositionLabels().
void ScreenMap::_refreshMarkers()
{
    MarkerLayer* ml = s_renderer.getMarkerLayer();
    if (!ml) return;

    // Remove existing markers and delete their name labels
    for (int i = 0; i < s_markerCount; i++) {
        ml->remove(s_markerIds[i]);
        if (s_nameLabels[i].obj) {
            lv_obj_del(s_nameLabels[i].obj);
            s_nameLabels[i].obj = nullptr;
        }
    }
    s_markerCount = 0;

    auto addMark = [&](double lat, double lon, uint32_t col, const char* name)
    {
        if ((lat == 0.0 && lon == 0.0) || s_markerCount >= 64) return;
        int idx = s_markerCount;
        char lbl = (name && name[0]) ? name[0] : '?';
        s_markerIds[idx] = ml->add(Marker(GeoPoint(lat, lon), 10, col, lbl));

        // Name label — parented to _screen so it floats above tile layer
        lv_obj_t* nlbl = lv_label_create(ScreenMap::_screen);
        lv_label_set_text(nlbl, name ? name : "");
        lv_obj_set_style_text_font(nlbl, &lv_font_montserrat_10, 0);
        // Convert RGB24 colour to lv_color_t for the label
        lv_obj_set_style_text_color(nlbl,
            lv_color_make((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF), 0);
        lv_obj_set_style_bg_opa(nlbl, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(nlbl, 0, 0);
        lv_obj_set_style_pad_all(nlbl, 0, 0);
        lv_label_set_long_mode(nlbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(nlbl, 80);
        lv_group_remove_obj(nlbl);

        s_nameLabels[idx] = { nlbl, lat, lon };
        s_markerCount++;
    };

    // Saved repeaters → purple
    for (int i = 0; i < ops::repeaters::count(); i++) {
        ops::Repeater r;
        if (!ops::repeaters::get(i, r)) continue;
        addMark((double)r.lat / 1e6, (double)r.lon / 1e6, COL_REPEATER, r.name);
    }

    // Saved contacts → blue
    for (int i = 0; i < ops::contacts::count(); i++) {
        ops::Contact c;
        if (!ops::contacts::get(i, c)) continue;
        addMark((double)c.lat / 1e6, (double)c.lon / 1e6, COL_CONTACT, c.name);
    }

    // Live peers not yet persisted in NVS stores
    auto& svc = ops::MeshService::instance();
    for (int i = 0; i < svc.peerCount(); i++) {
        ops::PeerInfo p;
        if (!svc.getPeer(i, p)) continue;
        if (p.lat == 0 && p.lon == 0) continue;
        int dummy;
        if (p.type == 2 && ops::repeaters::findByKey(p.pubKeyPrefix, &dummy)) continue;
        if (p.type != 2 && ops::contacts::findByKey(p.pubKeyPrefix, &dummy)) continue;
        addMark((double)p.lat / 1e6, (double)p.lon / 1e6,
                p.type == 2 ? COL_REPEATER : COL_CONTACT, p.name);
    }

    // Self position → green
    {
        auto& b = Board::instance();
        double slat = 0.0, slng = 0.0;
        if (b.hasGPSFix()) { slat = b.gpsLat(); slng = b.gpsLng(); }
        else {
            const auto& cfg = ops::config::get();
            slat = cfg.manualLat; slng = cfg.manualLon;
        }
        addMark(slat, slng, COL_SELF, ops::config::get().callsign);
    }
}

// ── _updateZoomLabel() ────────────────────────────────────────────────
void ScreenMap::_updateZoomLabel() {}  // no standalone zoom label in this layout

// ── Button & key callbacks ────────────────────────────────────────────
void ScreenMap::_onHomeClick(lv_event_t*)  { ScreenLauncher::show(); }
void ScreenMap::_onExitClick(lv_event_t*)  { ScreenLauncher::show(); }

void ScreenMap::_onZoomIn(lv_event_t*)
{
    if (_zoom >= ZOOM_MAX) return;
    _zoom++;
    s_renderer.setZoom(_zoom);
    _repositionLabels();
}

void ScreenMap::_onZoomOut(lv_event_t*)
{
    if (_zoom <= ZOOM_MIN) return;
    _zoom--;
    s_renderer.setZoom(_zoom);
    _repositionLabels();
}

void ScreenMap::_onKey(lv_event_t* e)
{
    uint32_t* key = (uint32_t*)lv_event_get_param(e);
    if (key && (*key == LV_KEY_ESC)) ScreenLauncher::show();
}

void ScreenMap::_onTouch(lv_event_t*)
{
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    if (vect.x != 0 || vect.y != 0)
        navigate(-vect.x, -vect.y);
}

// ── SD remount dialog ─────────────────────────────────────────────────
void ScreenMap::_showMountDialog()
{
    if (_mountOverlay) return;

    _mountOverlay = lv_obj_create(_screen);
    lv_obj_set_size(_mountOverlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(_mountOverlay, 0, 0);
    lv_obj_set_style_bg_color(_mountOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_mountOverlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_mountOverlay, 0, 0);
    lv_obj_set_style_pad_all(_mountOverlay, 0, 0);
    lv_obj_clear_flag(_mountOverlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(_mountOverlay);
    lv_obj_set_size(box, 230, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
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

    lv_obj_t* t = lv_label_create(box);
    lv_label_set_text(t, LV_SYMBOL_WARNING " No Map Tiles");
    lv_obj_set_style_text_color(t, theme::TEXT, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);

    lv_obj_t* body = lv_label_create(box);
    lv_label_set_text(body,
        "SD card not mounted or\n"
        "/maps/osm/ not found.\n"
        "Insert card and tap Mount.");
    lv_obj_set_style_text_color(body, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_10, 0);
    lv_obj_set_width(body, 206);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);

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

    auto mkBtn = [&](const char* label, lv_color_t bg, lv_event_cb_t cb)
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
    };
    mkBtn("Cancel",                    theme::BG,      _onMountCancel);
    mkBtn(LV_SYMBOL_REFRESH " Mount", theme::PRIMARY, _onMountClick);
}

void ScreenMap::_onMountClick(lv_event_t*)
{
    if (_mountOverlay) { lv_obj_del(_mountOverlay); _mountOverlay = nullptr; }
    OPS_LOG("Map", "Attempting SD remount");
    ops::sdcard::init();
    if (_hasTiles()) {
        s_renderer.setCenter(GeoPoint(_centerLat, _centerLng), _zoom);
        s_renderer.invalidate();
        _repositionLabels();
    } else {
        _showMountDialog();
    }
}

void ScreenMap::_onMountCancel(lv_event_t*) { ScreenLauncher::show(); }

}}  // namespace ops::ui
