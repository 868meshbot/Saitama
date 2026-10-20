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
#include "../utils/PsramAlloc.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "Emoji.h"
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

// Sun / moon emoji images (C objects, so declared at global scope).
extern "C" const lv_img_dsc_t emoji_sun;
extern "C" const lv_img_dsc_t emoji_moon;

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

// Marker colours (RGB24) — day tiles are light, night tiles dark, so each
// set is picked to read on its own background.
static constexpr uint32_t COL_REPEATER       = 0x9400D3;  // day: purple
static constexpr uint32_t COL_CONTACT        = 0xE02020;  // day: red
static constexpr uint32_t COL_SELF           = 0x1E5AFF;  // day: blue
static constexpr uint32_t COL_REPEATER_NIGHT = 0xFFC107;  // night: golden yellow
static constexpr uint32_t COL_CONTACT_NIGHT  = 0x24B4D7;  // night: cyan-blue
static constexpr uint32_t COL_SELF_NIGHT     = 0xFF3B30;  // night: red
static bool s_markersStale = false;   // style changed: rebuild markers in tick()

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
static NameLabel   (&s_nameLabels)[64] = ops::psram::array<NameLabel[64]>();
static int         s_markerCount = 0;

// ── Static member definitions ─────────────────────────────────────────
lv_obj_t* ScreenMap::_screen       = nullptr;
lv_obj_t* ScreenMap::_zoomLbl      = nullptr;
lv_obj_t* ScreenMap::_mountOverlay = nullptr;

float ScreenMap::_centerLat   = 51.5f;
float ScreenMap::_centerLng   = -0.12f;
int   ScreenMap::_zoom        = ZOOM_DEF;
bool  ScreenMap::_gpsCentered = false;

// ── Map style: day / night tile folders ───────────────────────────────
//   /maps/osm/day/{z}/{x}/{y}.png     standard OSM
//   /maps/osm/night/{z}/{x}/{y}.png   dark style (e.g. Thunderforest Transport Dark)
//   /maps/osm/{z}/{x}/{y}.png         old flat layout — used as "day"
// Day is the default; Auto shows night tiles while night mode is active; the
// top-bar moon/sun button switches day <-> night by hand. With no night
// tiles, night shows the day tiles colour-inverted (NavBoxLib smart invert).
// NavBoxLib keeps a pointer to the path pattern, so s_tileFmt is rewritten in
// place and the tile cache flushed when the style changes.

static char s_tileFmt[40] = "/maps/osm/%d/%d/%d.png";
static lv_obj_t* s_styleImg = nullptr;   // top-bar day/night button icon
static void _updateStyleBtn();
static bool s_tileInvert  = false;
static uint32_t s_styleCheckMs = 0;

static bool _hasLegacyTiles()
{
    char p[24];
    for (int z = 1; z <= 19; z++) {
        snprintf(p, sizeof(p), "/maps/osm/%d", z);
        if (SD.exists(p)) return true;
    }
    return false;
}

static void _resolveTiles(char* fmt, size_t n, bool& invert)
{
    const auto& cfg = ops::config::get();
    bool wantNight = cfg.mapStyle == 1 || (cfg.mapStyle == 2 && ops::config::nightModeActive());
    bool hasDay    = SD.exists("/maps/osm/day");
    bool hasNight  = SD.exists("/maps/osm/night");
    const char* dayRoot = hasDay ? "/maps/osm/day" : (_hasLegacyTiles() ? "/maps/osm" : nullptr);

    const char* root;
    invert = false;
    if (wantNight) {
        if (hasNight)     root = "/maps/osm/night";
        else if (dayRoot) { root = dayRoot; invert = true; }   // no night tiles: invert day
        else              root = "/maps/osm";
    } else {
        if (dayRoot)       root = dayRoot;
        else if (hasNight) root = "/maps/osm/night";             // only night tiles exist
        else               root = "/maps/osm";
    }
    snprintf(fmt, n, "%s/%%d/%%d/%%d.png", root);
}

// ── Tile availability check ───────────────────────────────────────────
static bool _hasTiles()
{
    return ops::sdcard::isMounted() && SD.exists("/maps/osm");
}

// ── show() ───────────────────────────────────────────────────────────
// Re-resolves the tile folder (style setting / night mode may have changed)
// and, if it differs, switches: rewrite the pattern NavBoxLib points at, then
// flush its cache — setSmartInvert(false) clears and reloads every tile.
static void _applyMapStyle()
{
    if (!ops::sdcard::isMounted()) return;
    char fmt[sizeof(s_tileFmt)];
    bool inv;
    _resolveTiles(fmt, sizeof(fmt), inv);
    if (strcmp(fmt, s_tileFmt) == 0 && inv == s_tileInvert) return;
    strncpy(s_tileFmt, fmt, sizeof(s_tileFmt) - 1);
    s_tileFmt[sizeof(s_tileFmt) - 1] = '\0';
    s_tileInvert = inv;
    s_renderer.setSmartInvert(false);          // drop cached tiles, reload
    if (inv) s_renderer.setSmartInvert(true);  // new tiles load inverted
    OPS_LOG("Map", "Map style -> %s%s", s_tileFmt, inv ? " (inverted)" : "");
    _updateStyleBtn();
    s_markersStale = true;   // marker colours follow day/night
}

// The button shows what a tap switches TO: moon while showing day tiles,
// sun while showing night (night folder or inverted day).
static bool _showingNight()
{
    return s_tileInvert || strstr(s_tileFmt, "/night/") != nullptr;
}

static void _updateStyleBtn()
{
    if (!s_styleImg) return;
    lv_img_set_src(s_styleImg, _showingNight() ? &emoji_sun : &emoji_moon);
}

static void _onStyleToggle(lv_event_t*)
{
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.mapStyle = _showingNight() ? 0 : 1;   // manual day or night
    ops::config::save();
    _applyMapStyle();
    _updateStyleBtn();
}

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
        _applyMapStyle();
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
    uint32_t now = millis();
    if (now - s_styleCheckMs > 30000) {   // night mode starting/ending while open
        s_styleCheckMs = now;
        _applyMapStyle();
    }
    if (s_markersStale) {
        s_markersStale = false;
        _refreshMarkers();
        _repositionLabels();
    }
    s_renderer.iterate(now);
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
        return lbl;
    };

    mkBarBtn(LV_SYMBOL_HOME,  _onHomeClick);

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, LV_SYMBOL_IMAGE " Map");
    lv_obj_set_style_text_color(title, theme::TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_flex_grow(title, 1);

    // Day/night switch, left of Close
    // The emoji are 16 px images; scaled to ~11 px to sit with the 10 pt
    // bar symbols. The empty label keeps the button the same size as its
    // neighbours.
    {
        lv_obj_t* lbl = mkBarBtn("", _onStyleToggle);
        lv_obj_t* btn = lv_obj_get_parent(lbl);
        lv_obj_set_width(lbl, 11);
        s_styleImg = lv_img_create(btn);
        lv_img_set_src(s_styleImg, &emoji_moon);
        lv_img_set_zoom(s_styleImg, 176);   // 256 = 100%
        lv_img_set_antialias(s_styleImg, true);
        lv_obj_center(s_styleImg);
    }

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

    if (ops::sdcard::isMounted()) _resolveTiles(s_tileFmt, sizeof(s_tileFmt), s_tileInvert);
    OPS_LOG("Map", "Tiles %s%s", s_tileFmt, s_tileInvert ? " (inverted)" : "");
    bool ok = s_renderer.begin(_screen, MAP_W, MAP_H, s_tileFmt);   // keeps the pointer
    if (!ok) {
        OPS_LOG("Map", "NavBoxLib begin() failed — map unavailable");
        return;
    }
    s_renderer.setXY(0, TOP_H);
    if (s_tileInvert) s_renderer.setSmartInvert(true);
    _updateStyleBtn();
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
    s_markersStale = false;

    const bool night = _showingNight();
    const uint32_t colRepeater = night ? COL_REPEATER_NIGHT : COL_REPEATER;
    const uint32_t colContact  = night ? COL_CONTACT_NIGHT  : COL_CONTACT;
    const uint32_t colSelf     = night ? COL_SELF_NIGHT     : COL_SELF;

    auto addMark = [&](double lat, double lon, uint32_t col, const char* name)
    {
        if ((lat == 0.0 && lon == 0.0) || s_markerCount >= 64) return;
        int idx = s_markerCount;
        char lbl = (name && name[0]) ? name[0] : '?';
        s_markerIds[idx] = ml->add(Marker(GeoPoint(lat, lon), 10, col, lbl));

        // Name label — parented to _screen so it floats above tile layer
        lv_obj_t* nlbl = lv_label_create(ScreenMap::_screen);
        lv_label_set_text(nlbl, name ? name : "");
        lv_obj_set_style_text_font(nlbl, theme::bodyFont10(), 0);
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

    // Saved repeaters
    for (int i = 0; i < ops::repeaters::count(); i++) {
        ops::Repeater r;
        if (!ops::repeaters::get(i, r)) continue;
        addMark((double)r.lat / 1e6, (double)r.lon / 1e6, colRepeater, r.name);
    }

    // Saved contacts
    for (int i = 0; i < ops::contacts::count(); i++) {
        ops::Contact c;
        if (!ops::contacts::get(i, c)) continue;
        addMark((double)c.lat / 1e6, (double)c.lon / 1e6, colContact, c.name);
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
                p.type == 2 ? colRepeater : colContact, p.name);
    }

    // Self position
    {
        auto& b = Board::instance();
        double slat = 0.0, slng = 0.0;
        if (b.hasGPSFix()) { slat = b.gpsLat(); slng = b.gpsLng(); }
        else {
            const auto& cfg = ops::config::get();
            slat = cfg.manualLat; slng = cfg.manualLon;
        }
        addMark(slat, slng, colSelf, ops::config::get().callsign);
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
