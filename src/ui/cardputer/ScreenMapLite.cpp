// Saitama — cardputer/ScreenMapLite.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Offline map for the Cardputer (no PSRAM). Implements the ScreenMap API the
// T-Deck map (ScreenMap.cpp, NavBoxLib) provides, so UIScreen and the
// launcher call it the same way.
//
// No tile cache and no screen buffer: each visible 256 px JPEG tile is
// decoded by LVGL's bundled TJpgDec one MCU block (<= 16x16) at a time and
// pushed straight to the TFT. One tile per tick, so the mesh loop keeps
// running; markers and the info line go on once every tile is down. The
// LVGL screen behind is empty, so LVGL only paints here when something
// covers the map (a message popup, a notice, the screensaver returning);
// flush_cb tells us, and the map is drawn again once the overlay has gone.
//
// Tiles (baseline JPEG, standard slippy-map numbering; make them with
// scripts/map_tiles_cardputer.py):
//   /maps/osm/{z}/{x}/{y}.jpg       then   /maps/osm/day/{z}/{x}/{y}.jpg
// PNG tiles for the T-Deck can sit beside them on the same card.
//
// Keys: arrows pan a quarter screen, = / - zoom, c centres on our position,
// Esc / Backspace back to the launcher.

#include "../../features.h"
#if OPS_MAP_LITE

#include "../ScreenMap.h"
#include "../ScreenLauncher.h"
#include "MapLite.h"
#include "../../hardware/Board.h"
#include "../../mesh/MeshService.h"
#include "../../utils/Config.h"
#include "../../utils/Contacts.h"
#include "../../utils/Repeaters.h"
#include "../../utils/PsramAlloc.h"
#include "../../utils/SDCard.h"
#include "../../utils/Log.h"

#include <TFT_eSPI.h>
// LVGL's TJpgDec R0.03 (LV_USE_SJPG): its jd_prepare/jd_decomp override the
// ROM ones at link time, so its header (struct layout) must be the one used.
#include <src/extra/libs/sjpg/tjpgd.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>

namespace ops { namespace ui {

// ── Layout ────────────────────────────────────────────────────────────
static constexpr int MAP_W    = OPS_SCREEN_W;
static constexpr int INFO_H   = 11;                       // bottom info line
static constexpr int MAP_H    = OPS_SCREEN_H - INFO_H;
static constexpr int TILE     = 256;
static constexpr int ZOOM_MIN = 2;
static constexpr int ZOOM_MAX = 16;
static constexpr int ZOOM_DEF = 12;
static constexpr int PAN_X    = MAP_W / 4;
static constexpr int PAN_Y    = MAP_H / 4;

// RGB565
static constexpr uint16_t COL_NOTILE   = 0x39E7;   // dark grey
static constexpr uint16_t COL_REPEATER = 0x901A;   // purple (T-Deck day colours)
static constexpr uint16_t COL_CONTACT  = 0xE104;   // red
static constexpr uint16_t COL_SELF     = 0x1ADF;   // blue

// TJpgDec work area (JD_SZBUF 512 + Huffman/IDCT tables) plus one MCU
// of RGB565 output. Allocated while the map is shown, freed when it leaves.
static constexpr size_t JD_POOL  = 3900;
static constexpr size_t MCU_PX   = 16 * 16;

// ── State ─────────────────────────────────────────────────────────────
static lv_obj_t* s_screen = nullptr;
static lv_obj_t* s_keyObj = nullptr;   // hidden; takes the keys
static uint8_t*  s_work   = nullptr;   // JD_POOL + MCU_PX * 2 bytes

static int    s_zoom = ZOOM_DEF;
static double s_cx = 0.0, s_cy = 0.0;  // view centre, world pixels at s_zoom
static bool   s_placed = false;        // centre set (first show)

// Drawing: tiles of the current view are drawn one per tick.
static constexpr int MAX_TILES = 4;    // 240x124 view spans at most 2x2 tiles
struct TileJob { int tx, ty, sx, sy; };// tile index, screen pos of its corner
static TileJob s_jobs[MAX_TILES];
static int      s_jobCount = 0;
static int      s_jobNext  = -1;       // -1 = idle
static int      s_tilesFound = 0;
static bool     s_dirty = true;        // draw again when nothing covers the map
static uint32_t s_lastFlushMs = 0;     // last LVGL paint

// ── Mercator ──────────────────────────────────────────────────────────
static double _world(int z) { return (double)TILE * (double)(1UL << z); }

static double _lonToX(double lon, int z) { return (lon + 180.0) / 360.0 * _world(z); }
static double _latToY(double lat, int z)
{
    if (lat >  85.05) lat =  85.05;
    if (lat < -85.05) lat = -85.05;
    double r = lat * M_PI / 180.0;
    return (1.0 - asinh(tan(r)) / M_PI) / 2.0 * _world(z);   // asinh(tan) = ln(tan + sec)
}
static double _xToLon(double x, int z) { return x / _world(z) * 360.0 - 180.0; }
static double _yToLat(double y, int z)
{
    double n = M_PI * (1.0 - 2.0 * y / _world(z));
    return atan(sinh(n)) * 180.0 / M_PI;
}

static void _clampCentre()
{
    double w = _world(s_zoom);
    s_cx = fmod(s_cx, w);
    if (s_cx < 0) s_cx += w;
    if (s_cy < 0) s_cy = 0;
    if (s_cy > w) s_cy = w;
}

// Our position: GPS fix, else the manual one in Settings.
static bool _selfPos(double& lat, double& lon)
{
    auto& b = Board::instance();
    if (b.hasGPSFix()) { lat = b.gpsLat(); lon = b.gpsLng(); return true; }
    const auto& cfg = config::get();
    if (cfg.manualLat != 0.0f || cfg.manualLon != 0.0f) {
        lat = cfg.manualLat; lon = cfg.manualLon;
        return true;
    }
    return false;
}

static void _centreOn(double lat, double lon)
{
    s_cx = _lonToX(lon, s_zoom);
    s_cy = _latToY(lat, s_zoom);
    _clampCentre();
}

// Smallest/largest number among the entry names of a folder ("2003",
// "1355.jpg"). False if it has none or can't be opened.
static bool _numRange(const char* path, long& lo, long& hi)
{
    DIR* d = opendir(path);
    if (!d) return false;
    bool any = false;
    while (struct dirent* e = readdir(d)) {
        char* end;
        long v = strtol(e->d_name, &end, 10);
        if (end == e->d_name) continue;
        if (!any || v < lo) lo = v;
        if (!any || v > hi) hi = v;
        any = true;
    }
    closedir(d);
    return any;
}

// No GPS fix and no manual position: centre on the tiles on the card at
// the current zoom, so the map doesn't open on an empty default.
static bool _centreOnCardTiles()
{
    if (!sdcard::isMounted()) return false;
    static const char* const kRoots[] = { "/sd/maps/osm", "/sd/maps/osm/day" };
    for (const char* root : kRoots) {
        char path[48];
        snprintf(path, sizeof(path), "%s/%d", root, s_zoom);
        long x0, x1;
        if (!_numRange(path, x0, x1)) continue;
        long xm = (x0 + x1) / 2, y0 = 0, y1 = 0;
        bool got = false;
        for (long dx = 0; dx <= x1 - x0 && !got; dx++) {   // nearest column to the middle
            for (int s = 0; s < 2 && !got; s++) {
                long x = s ? xm + dx : xm - dx;
                if (x < x0 || x > x1) continue;
                snprintf(path, sizeof(path), "%s/%d/%ld", root, s_zoom, x);
                got = _numRange(path, y0, y1);
            }
        }
        if (!got) continue;
        s_cx = (x0 + x1 + 1) * (TILE / 2.0);
        s_cy = (y0 + y1 + 1) * (TILE / 2.0);
        _clampCentre();
        return true;
    }
    return false;
}

// ── JPEG tile decode → TFT ────────────────────────────────────────────
struct DecodeCtx {
    FILE*     f;
    int       ox, oy;    // screen position of the tile's top-left corner
    TFT_eSPI* tft;
    uint16_t* mcu;
};

static size_t _jdIn(JDEC* jd, uint8_t* buf, size_t len)
{
    DecodeCtx* c = (DecodeCtx*)jd->device;
    if (buf) return fread(buf, 1, len, c->f);
    return fseek(c->f, (long)len, SEEK_CUR) == 0 ? len : 0;
}

static int _jdOut(JDEC* jd, void* bitmap, JRECT* r)
{
    DecodeCtx* c = (DecodeCtx*)jd->device;
    int x0 = c->ox + r->left, y0 = c->oy + r->top;
    int w  = r->right - r->left + 1;
    int h  = r->bottom - r->top + 1;
    if (y0 >= MAP_H) return 0;             // rest of the tile is off-screen

    int cx0 = x0 < 0 ? 0 : x0,  cx1 = x0 + w > MAP_W ? MAP_W : x0 + w;
    int cy0 = y0 < 0 ? 0 : y0,  cy1 = y0 + h > MAP_H ? MAP_H : y0 + h;
    if (cx0 >= cx1 || cy0 >= cy1) return 1;

    const uint8_t* src = (const uint8_t*)bitmap;   // RGB888 (JD_FORMAT 0)
    int n = 0;
    for (int y = cy0; y < cy1; y++) {
        const uint8_t* p = src + ((y - y0) * w + (cx0 - x0)) * 3;
        for (int x = cx0; x < cx1; x++, p += 3)
            c->mcu[n++] = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
    }
    c->tft->pushImage(cx0, cy0, cx1 - cx0, cy1 - cy0, c->mcu);
    return 1;
}

static FILE* _openTile(int z, int x, int y)
{
    char path[48];
    snprintf(path, sizeof(path), "/sd/maps/osm/%d/%d/%d.jpg", z, x, y);
    FILE* f = fopen(path, "rb");
    if (f) return f;
    snprintf(path, sizeof(path), "/sd/maps/osm/day/%d/%d/%d.jpg", z, x, y);
    return fopen(path, "rb");
}

static void _fillTileArea(TFT_eSPI& tft, int sx, int sy, uint16_t col)
{
    int x0 = sx < 0 ? 0 : sx, x1 = sx + TILE > MAP_W ? MAP_W : sx + TILE;
    int y0 = sy < 0 ? 0 : sy, y1 = sy + TILE > MAP_H ? MAP_H : sy + TILE;
    if (x0 < x1 && y0 < y1) tft.fillRect(x0, y0, x1 - x0, y1 - y0, col);
}

static void _drawTile(const TileJob& j)
{
    TFT_eSPI& tft = directTft();
    int n = 1 << s_zoom;
    FILE* f = nullptr;
    if (j.ty >= 0 && j.ty < n && s_work && sdcard::isMounted())
        f = _openTile(s_zoom, ((j.tx % n) + n) % n, j.ty);
    if (!f) { _fillTileArea(tft, j.sx, j.sy, COL_NOTILE); return; }

    DecodeCtx ctx{ f, j.sx, j.sy, &tft, (uint16_t*)(s_work + JD_POOL) };
    static JDEC jd;                        // off the loop stack
    JRESULT rc = jd_prepare(&jd, _jdIn, s_work, JD_POOL, &ctx);
    if (rc == JDR_OK) rc = jd_decomp(&jd, _jdOut, 0);
    fclose(f);
    if (rc == JDR_OK || rc == JDR_INTR) {
        s_tilesFound++;
    } else {
        OPS_LOG("Map", "tile %d/%d/%d: JPEG error %d (progressive or grey?)",
                s_zoom, j.tx, j.ty, (int)rc);
        _fillTileArea(tft, j.sx, j.sy, COL_NOTILE);
    }
}

// ── Markers and info line ─────────────────────────────────────────────
static void _dot(TFT_eSPI& tft, double lat, double lon, uint16_t col, int r)
{
    if (lat == 0.0 && lon == 0.0) return;
    double w  = _world(s_zoom);
    double dx = _lonToX(lon, s_zoom) - s_cx;
    if (dx >  w / 2) dx -= w;              // nearest copy across the date line
    if (dx < -w / 2) dx += w;
    int x = (int)lround(MAP_W / 2 + dx);
    int y = (int)lround(MAP_H / 2 + _latToY(lat, s_zoom) - s_cy);
    if (x < r || x >= MAP_W - r || y < r || y >= MAP_H - r) return;
    tft.fillCircle(x, y, r, col);
    tft.drawCircle(x, y, r + 1, TFT_BLACK);
}

static void _drawMarkers()
{
    TFT_eSPI& tft = directTft();
    for (int i = 0; i < repeaters::count(); i++) {
        Repeater r;
        if (repeaters::get(i, r)) _dot(tft, r.lat / 1e6, r.lon / 1e6, COL_REPEATER, 2);
    }
    for (int i = 0; i < contacts::count(); i++) {
        Contact c;
        if (contacts::get(i, c)) _dot(tft, c.lat / 1e6, c.lon / 1e6, COL_CONTACT, 2);
    }
    auto& svc = MeshService::instance();
    for (int i = 0; i < svc.peerCount(); i++) {   // heard, not saved
        PeerInfo p;
        if (!svc.getPeer(i, p) || (p.lat == 0 && p.lon == 0)) continue;
        int idx;
        if (p.type == 2 ? repeaters::findByKey(p.pubKeyPrefix, &idx)
                        : contacts::findByKey(p.pubKeyPrefix, &idx)) continue;
        _dot(tft, p.lat / 1e6, p.lon / 1e6, p.type == 2 ? COL_REPEATER : COL_CONTACT, 2);
    }
    double lat, lon;
    if (_selfPos(lat, lon)) {               // ours last, on top
        _dot(tft, lat, lon, COL_SELF, 3);
    }
}

static void _drawInfo()
{
    TFT_eSPI& tft = directTft();
    tft.fillRect(0, MAP_H, MAP_W, INFO_H, TFT_BLACK);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    char buf[40];
    if (s_tilesFound == 0) {
        snprintf(buf, sizeof(buf), "Z%d no tiles here", s_zoom);
    } else {
        snprintf(buf, sizeof(buf), "Z%d %.4f,%.4f", s_zoom,
                 _yToLat(s_cy, s_zoom), _xToLon(s_cx, s_zoom));
    }
    tft.drawString(buf, 2, MAP_H + 2);
    tft.setTextColor(0xAD55, TFT_BLACK);   // light grey
    tft.drawString("(c)OSM", MAP_W - 6 * 6 - 2, MAP_H + 2);
}

static void _drawNoCard()
{
    TFT_eSPI& tft = directTft();
    tft.fillRect(0, 0, MAP_W, OPS_SCREEN_H, TFT_BLACK);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("No SD card - maps need JPEG tiles in", 4, 50);
    tft.drawString("/maps/osm/{z}/{x}/{y}.jpg", 4, 62);
    tft.setTextColor(0xAD55, TFT_BLACK);
    tft.drawString("Esc: back", 4, 80);
}

// ── Draw scheduling ───────────────────────────────────────────────────
static void _planView()
{
    int left = (int)floor(s_cx - MAP_W / 2.0);
    int top  = (int)floor(s_cy - MAP_H / 2.0);
    int tx0 = (int)floor(left / (double)TILE), tx1 = (int)floor((left + MAP_W - 1) / (double)TILE);
    int ty0 = (int)floor(top  / (double)TILE), ty1 = (int)floor((top  + MAP_H - 1) / (double)TILE);
    s_jobCount = 0;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1 && s_jobCount < MAX_TILES; tx++)
            s_jobs[s_jobCount++] = { tx, ty, tx * TILE - left, ty * TILE - top };
    s_jobNext    = 0;
    s_tilesFound = 0;
}

static void _requestDraw() { s_dirty = true; }

// Something of LVGL's is on top of the map (popup on the screen, notice on
// the top layer): leave it alone until it has gone.
static bool _overlayShowing()
{
    auto visibleKids = [](lv_obj_t* parent, lv_obj_t* skip) {
        uint32_t n = lv_obj_get_child_cnt(parent);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t* c = lv_obj_get_child(parent, (int32_t)i);
            if (c != skip && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) return true;
        }
        return false;
    };
    return visibleKids(s_screen, s_keyObj) || visibleKids(lv_layer_top(), nullptr);
}

// ── Keys ──────────────────────────────────────────────────────────────
// Zoom levels with a folder on the card; the full range if there are none.
static int s_zLo = ZOOM_MIN, s_zHi = ZOOM_MAX;

static bool _zoomOnCard(int z)
{
    char path[32];
    snprintf(path, sizeof(path), "/sd/maps/osm/%d", z);
    DIR* d = opendir(path);
    if (!d) {
        snprintf(path, sizeof(path), "/sd/maps/osm/day/%d", z);
        d = opendir(path);
    }
    if (d) closedir(d);
    return d != nullptr;
}

static void _scanZoomRange()
{
    s_zLo = ZOOM_MIN; s_zHi = ZOOM_MAX;
    if (!sdcard::isMounted()) return;
    int lo = -1, hi = -1;
    for (int z = ZOOM_MIN; z <= ZOOM_MAX; z++)
        if (_zoomOnCard(z)) { if (lo < 0) lo = z; hi = z; }
    if (lo >= 0) { s_zLo = lo; s_zHi = hi; }
}

static void _zoomBy(int d)
{
    int z = s_zoom + d;
    if (z < s_zLo || z > s_zHi) return;
    double f = d > 0 ? 2.0 : 0.5;
    s_cx *= f; s_cy *= f;
    s_zoom = z;
    _clampCentre();
    _requestDraw();
}

static void _mapKey(lv_event_t* e)
{
    uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_ESC) { ScreenLauncher::show(); return; }
    if (k == '=' || k == '+') _zoomBy(+1);
    else if (k == '-' || k == '_') _zoomBy(-1);
    else if (k == 'c' || k == 'C') {
        double lat, lon;
        if (_selfPos(lat, lon)) _centreOn(lat, lon);
        else _centreOnCardTiles();
        _requestDraw();
    }
}

// ── ScreenMap API ─────────────────────────────────────────────────────
void ScreenMap::show()
{
    if (!s_screen) {
        s_screen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
        lv_obj_set_style_pad_all(s_screen, 0, 0);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

        // Hidden, so focusing it never makes LVGL paint over the tiles;
        // UIScreen sends keys straight to the focused object.
        s_keyObj = lv_obj_create(s_screen);
        lv_obj_set_size(s_keyObj, 1, 1);
        lv_obj_add_flag(s_keyObj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_keyObj, _mapKey, LV_EVENT_KEY, nullptr);
    }
    _scanZoomRange();                      // the card may have changed
    while (s_zoom < s_zLo) _zoomBy(+1);
    while (s_zoom > s_zHi) _zoomBy(-1);
    if (!s_placed) {
        double lat = 51.5, lon = -0.12;    // same default as the T-Deck map
        if (_selfPos(lat, lon)) _centreOn(lat, lon);
        else if (!_centreOnCardTiles()) _centreOn(lat, lon);
        s_placed = true;
    }
    if (!s_work) s_work = (uint8_t*)ops::psram::alloc(JD_POOL + MCU_PX * 2);
    if (!s_work) OPS_LOG("Map", "no memory for the JPEG decoder");

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, s_keyObj);
        lv_group_focus_obj(s_keyObj);
    }
    s_jobNext = -1;
    _requestDraw();
    lv_scr_load(s_screen);
    OPS_LOG("Map", "shown: z%d", s_zoom);
}

bool ScreenMap::isActive()
{
    return s_screen && lv_scr_act() == s_screen;
}

void ScreenMap::navigate(int dxPx, int dyPx)
{
    // UIScreen passes 8 px per key step; pan a quarter screen per step.
    int sx = dxPx > 0 ? 1 : dxPx < 0 ? -1 : 0;
    int sy = dyPx > 0 ? 1 : dyPx < 0 ? -1 : 0;
    if (!sx && !sy) return;
    s_cx += sx * PAN_X;
    s_cy += sy * PAN_Y;
    _clampCentre();
    _requestDraw();
}

void ScreenMap::tick()
{
    if (!isActive()) {
        if (s_work) { free(s_work); s_work = nullptr; }   // ~4.4 KB back
        s_jobNext = -1;
        return;
    }
    uint32_t now = millis();
    if (s_dirty) {
        // Let LVGL finish painting, and wait out anything on top of the map.
        if (now - s_lastFlushMs < 80 || _overlayShowing()) return;
        s_dirty = false;
        if (!sdcard::isMounted()) { _drawNoCard(); s_jobNext = -1; return; }
        _planView();
    }
    if (s_jobNext < 0) return;
    if (s_jobNext < s_jobCount) {
        _drawTile(s_jobs[s_jobNext++]);
        return;
    }
    _drawMarkers();
    _drawInfo();
    s_jobNext = -1;
}

namespace maplite {
void noteLvglFlush()
{
    if (!ScreenMap::isActive()) return;
    s_lastFlushMs = millis();
    _requestDraw();    // LVGL painted over (part of) the map
}
}  // namespace maplite

}}  // namespace ops::ui

#endif  // OPS_MAP_LITE
