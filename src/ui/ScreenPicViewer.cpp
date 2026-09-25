// Saitama — ScreenPicViewer.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Layout (320 × 240, image fills the whole screen, letterboxed in black):
//
//   ┌──────────────────────────────────────┐
//   │ [📁 Browse]                     [ X ]│  buttons float over the image
//   │                                      │
//   │              (image)                 │
//   │                                      │
//   │ photo.jpg  1024×768                  │  info strip
//   └──────────────────────────────────────┘
//
// Decoding writes straight into a 320×240 lv_canvas buffer in PSRAM:
//   JPEG — TJpgDec (bundled with LVGL 8.3, enabled by LV_USE_SJPG) decodes
//          at 1/1, 1/2, 1/4 or 1/8 scale, then nearest-neighbour to fit.
//   PNG  — PNGdec delivers one row at a time; rows that map to no output
//          line are skipped without colour conversion. PNGdec's row buffer
//          is a compile-time size (~640 px RGBA) shared with NavBoxLib's
//          static decoder in DRAM, so rather than raise it, wider PNGs fall
//          back to LodePNG (compiled via LV_USE_PNG), which decodes the whole
//          image into PSRAM — fine up to roughly a megapixel.
// Decoding runs on the UI task; progressive JPEGs are not supported by
// TJpgDec and are reported as such.

#include "ScreenPicViewer.h"
#include "ScreenFileManager.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../utils/SDCard.h"
#include "../utils/Log.h"
#include <SD.h>
#include <PNGdec.h>
#include <src/extra/libs/sjpg/tjpgd.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>

// LodePNG is compiled via LV_USE_PNG=1; allocates through lv_mem (ps_malloc).
extern "C" {
unsigned lodepng_decode24(unsigned char** out, unsigned* w, unsigned* h,
                          const unsigned char* in, size_t insize);
const char* lodepng_error_text(unsigned code);
}

namespace ops { namespace ui {

static constexpr int CANVAS_W = OPS_SCREEN_W;
static constexpr int CANVAS_H = OPS_SCREEN_H;
static constexpr size_t JPG_POOL_SZ = 8192;   // TJpgDec work area (needs ~3.5 KB)
static constexpr int PNG_LINE_MAX = 1024;     // > PNGdec's own row limit
static constexpr size_t PSRAM_HEADROOM = 512 * 1024;  // left free for the rest of the app

lv_obj_t* ScreenPicViewer::_screen    = nullptr;
lv_obj_t* ScreenPicViewer::_canvas    = nullptr;
lv_obj_t* ScreenPicViewer::_browseBtn = nullptr;
lv_obj_t* ScreenPicViewer::_msgLbl    = nullptr;
lv_obj_t* ScreenPicViewer::_infoLbl   = nullptr;

static lv_color_t* s_canvasBuf   = nullptr;   // CANVAS_W × CANVAS_H, PSRAM
static uint8_t*    s_jpgPool     = nullptr;   // TJpgDec work pool, PSRAM
static PNG*        s_png         = nullptr;   // PNGdec state (~50 KB), PSRAM
static uint16_t*   s_pngLine     = nullptr;   // one RGB565 row, PSRAM
static File        s_file;
static bool        s_fromFileMgr = false;
static bool        s_hasImage    = false;
static char        s_info[96]    = {};

// ── Scaling context shared by both decoders ─────────────────────────────

struct FitCtx {
    int srcW, srcH;   // size of the decoder's output (after JPEG pre-scale)
    int dstW, dstH;   // fitted size on screen
    int offX, offY;   // letterbox offset
    uint32_t lastYieldMs;
};
static FitCtx s_fit;

static void _computeFit(int w, int h)
{
    int dw = CANVAS_W;
    int dh = (int)((int64_t)h * CANVAS_W / w);
    if (dh > CANVAS_H) {
        dh = CANVAS_H;
        dw = (int)((int64_t)w * CANVAS_H / h);
    }
    s_fit.dstW = dw < 1 ? 1 : dw;
    s_fit.dstH = dh < 1 ? 1 : dh;
    s_fit.offX = (CANVAS_W - s_fit.dstW) / 2;
    s_fit.offY = (CANVAS_H - s_fit.dstH) / 2;
}

// First output line/column whose nearest-neighbour source is >= src.
static inline int _firstDst(int src, int dstN, int srcN)
{
    return (int)(((int64_t)src * dstN + srcN - 1) / srcN);
}

static inline int _srcOf(int dst, int srcN, int dstN)
{
    return (int)((int64_t)dst * srcN / dstN);
}

// Big images take seconds; let IDLE run so the task watchdog stays fed.
static void _maybeYield()
{
    uint32_t now = millis();
    if (now - s_fit.lastYieldMs > 200) {
        s_fit.lastYieldMs = now;
        vTaskDelay(1);
    }
}

// ── JPEG (TJpgDec) ───────────────────────────────────────────────────────

static size_t _jpgIn(JDEC* /*jd*/, uint8_t* buf, size_t n)
{
    if (buf) return s_file.read(buf, n);
    return s_file.seek(s_file.position() + n) ? n : 0;
}

static int _jpgOut(JDEC* /*jd*/, void* bitmap, JRECT* r)
{
    const uint8_t* px = (const uint8_t*)bitmap;
    const int bw = r->right - r->left + 1;

    for (int dy = _firstDst(r->top, s_fit.dstH, s_fit.srcH); dy < s_fit.dstH; dy++) {
        int sy = _srcOf(dy, s_fit.srcH, s_fit.dstH);
        if (sy > r->bottom) break;
        lv_color_t* out = s_canvasBuf + (s_fit.offY + dy) * CANVAS_W + s_fit.offX;
        for (int dx = _firstDst(r->left, s_fit.dstW, s_fit.srcW); dx < s_fit.dstW; dx++) {
            int sx = _srcOf(dx, s_fit.srcW, s_fit.dstW);
            if (sx > r->right) break;
            const uint8_t* p = px + ((sy - r->top) * bw + (sx - r->left)) * 3;
            out[dx] = lv_color_make(p[0], p[1], p[2]);
        }
    }
    _maybeYield();
    return 1;
}

static bool _decodeJpeg(const char* path, char* err, int errMax)
{
    if (!s_jpgPool) s_jpgPool = (uint8_t*)ps_malloc(JPG_POOL_SZ);
    if (!s_jpgPool) { snprintf(err, errMax, "Out of memory"); return false; }

    s_file = SD.open(path, FILE_READ);
    if (!s_file) { snprintf(err, errMax, "Cannot open file"); return false; }

    JDEC jd;
    JRESULT res = jd_prepare(&jd, _jpgIn, s_jpgPool, JPG_POOL_SZ, nullptr);
    if (res != JDR_OK) {
        s_file.close();
        if (res == JDR_FMT3)
            snprintf(err, errMax, "Progressive JPEG not supported");
        else
            snprintf(err, errMax, "JPEG error %d", (int)res);
        return false;
    }

    _computeFit(jd.width, jd.height);
    // Largest decode-time reduction that still leaves at least the fitted size.
    uint8_t scale = 0;
    while (scale < 3 && (jd.width  >> (scale + 1)) >= s_fit.dstW
                     && (jd.height >> (scale + 1)) >= s_fit.dstH)
        scale++;
    s_fit.srcW = (jd.width  + (1 << scale) - 1) >> scale;
    s_fit.srcH = (jd.height + (1 << scale) - 1) >> scale;

    res = jd_decomp(&jd, _jpgOut, scale);
    s_file.close();
    if (res != JDR_OK) { snprintf(err, errMax, "JPEG decode error %d", (int)res); return false; }

    snprintf(s_info, sizeof(s_info), "%ux%u", (unsigned)jd.width, (unsigned)jd.height);
    return true;
}

// ── PNG (PNGdec) ─────────────────────────────────────────────────────────

static void* _pngOpen(const char* name, int32_t* size)
{
    s_file = SD.open(name, FILE_READ);
    if (!s_file) return nullptr;
    *size = (int32_t)s_file.size();
    return &s_file;
}

static void _pngClose(void* h)
{
    if (h) ((File*)h)->close();
}

static int32_t _pngRead(PNGFILE* f, uint8_t* buf, int32_t len)
{
    return (int32_t)((File*)f->fHandle)->read(buf, len);
}

static int32_t _pngSeek(PNGFILE* f, int32_t pos)
{
    return ((File*)f->fHandle)->seek(pos) ? pos : -1;
}

static int _pngDraw(PNGDRAW* d)
{
    int dy = _firstDst(d->y, s_fit.dstH, s_fit.srcH);
    if (dy >= s_fit.dstH || _srcOf(dy, s_fit.srcH, s_fit.dstH) != d->y) {
        _maybeYield();
        return 1;   // row not sampled — skip colour conversion
    }

    s_png->getLineAsRGB565(d, s_pngLine, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
    for (; dy < s_fit.dstH && _srcOf(dy, s_fit.srcH, s_fit.dstH) == d->y; dy++) {
        lv_color_t* out = s_canvasBuf + (s_fit.offY + dy) * CANVAS_W + s_fit.offX;
        for (int dx = 0; dx < s_fit.dstW; dx++) {
            uint16_t c = s_pngLine[_srcOf(dx, s_fit.srcW, s_fit.dstW)];
            out[dx] = lv_color_make((c >> 8) & 0xF8, (c >> 3) & 0xFC, (c << 3) & 0xF8);
        }
    }
    _maybeYield();
    return 1;
}

// Whole-image decode for PNGs too wide for PNGdec. Peak PSRAM is the file,
// LodePNG's inflated scanlines (up to 4 B/px + 1 B/row) and the RGB output.
static bool _decodePngLodepng(const char* path, int w, int h, char* err, int errMax)
{
    File f = SD.open(path, FILE_READ);
    if (!f) { snprintf(err, errMax, "Cannot open file"); return false; }
    size_t fsz = (size_t)f.size();

    size_t need = fsz + (size_t)h * ((size_t)w * 4 + 1) + (size_t)w * h * 3;
    size_t freeB = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (need + PSRAM_HEADROOM > freeB) {
        f.close();
        snprintf(err, errMax, "PNG too large (%dx%d)", w, h);
        return false;
    }

    uint8_t* data = (uint8_t*)ps_malloc(fsz);
    if (!data) { f.close(); snprintf(err, errMax, "Out of memory"); return false; }
    size_t got = f.read(data, fsz);
    f.close();
    if (got != fsz) { free(data); snprintf(err, errMax, "Read error"); return false; }

    unsigned char* rgb = nullptr;
    unsigned ow = 0, oh = 0;
    unsigned rc = lodepng_decode24(&rgb, &ow, &oh, data, fsz);
    free(data);
    if (rc || !rgb) {
        if (rgb) lv_mem_free(rgb);
        snprintf(err, errMax, "PNG: %s", lodepng_error_text(rc));
        return false;
    }

    _computeFit((int)ow, (int)oh);
    s_fit.srcW = (int)ow;
    s_fit.srcH = (int)oh;
    for (int dy = 0; dy < s_fit.dstH; dy++) {
        const uint8_t* row = rgb + (size_t)_srcOf(dy, oh, s_fit.dstH) * ow * 3;
        lv_color_t* out = s_canvasBuf + (s_fit.offY + dy) * CANVAS_W + s_fit.offX;
        for (int dx = 0; dx < s_fit.dstW; dx++) {
            const uint8_t* p = row + _srcOf(dx, ow, s_fit.dstW) * 3;
            out[dx] = lv_color_make(p[0], p[1], p[2]);
        }
    }
    lv_mem_free(rgb);
    return true;
}

// Width/height from the IHDR chunk (always first, at bytes 16..23).
static bool _pngSize(const char* path, int* w, int* h)
{
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint8_t hdr[24];
    size_t n = f.read(hdr, sizeof(hdr));
    f.close();
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (n != sizeof(hdr) || memcmp(hdr, SIG, 8) != 0 || memcmp(hdr + 12, "IHDR", 4) != 0)
        return false;
    *w = (int)((uint32_t)hdr[16] << 24 | hdr[17] << 16 | hdr[18] << 8 | hdr[19]);
    *h = (int)((uint32_t)hdr[20] << 24 | hdr[21] << 16 | hdr[22] << 8 | hdr[23]);
    return *w > 0 && *h > 0;
}

static bool _decodePng(const char* path, char* err, int errMax)
{
    int w = 0, h = 0;
    if (!_pngSize(path, &w, &h)) { snprintf(err, errMax, "Not a valid PNG"); return false; }
    if (w > PNG_LINE_MAX) {
        if (!_decodePngLodepng(path, w, h, err, errMax)) return false;
        snprintf(s_info, sizeof(s_info), "%dx%d", w, h);
        return true;
    }

    if (!s_png) {
        void* mem = ps_malloc(sizeof(PNG));
        if (mem) s_png = new (mem) PNG();
    }
    if (!s_pngLine) s_pngLine = (uint16_t*)ps_malloc(PNG_LINE_MAX * sizeof(uint16_t));
    if (!s_png || !s_pngLine) { snprintf(err, errMax, "Out of memory"); return false; }

    // PNGdec rejects rows wider than its buffer (~640 px RGBA) in open().
    int rc = s_png->open(path, _pngOpen, _pngClose, _pngRead, _pngSeek, _pngDraw);
    if (rc == PNG_TOO_BIG) {
        if (!_decodePngLodepng(path, w, h, err, errMax)) return false;
        snprintf(s_info, sizeof(s_info), "%dx%d", w, h);
        return true;
    }
    if (rc != PNG_SUCCESS) {
        snprintf(err, errMax, "PNG error %d", rc);
        return false;
    }

    _computeFit(w, h);
    s_fit.srcW = w;
    s_fit.srcH = h;

    rc = s_png->decode(nullptr, 0);
    s_png->close();
    if (rc != PNG_SUCCESS) {
        snprintf(err, errMax, "PNG decode error %d", rc);
        return false;
    }

    snprintf(s_info, sizeof(s_info), "%dx%d", w, h);
    return true;
}

// ── Public API ───────────────────────────────────────────────────────────

bool ScreenPicViewer::isImageFile(const char* name)
{
    const char* dot = name ? strrchr(name, '.') : nullptr;
    if (!dot) return false;
    char ext[6] = {};
    for (int i = 0; i < 5 && dot[i + 1]; i++)
        ext[i] = (char)tolower((unsigned char)dot[i + 1]);
    if (strlen(dot + 1) > 4) return false;
    return strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0 || strcmp(ext, "png") == 0;
}

void ScreenPicViewer::show()
{
    s_fromFileMgr = false;
    if (!_screen) _build();
    if (!s_hasImage)
        _showMessage("No image loaded\n\nTap Browse to pick a JPG or PNG\nfrom the SD card");
    _activate();
}

void ScreenPicViewer::openFile(const char* path, bool fromFileManager)
{
    s_fromFileMgr = fromFileManager;
    if (!_screen) _build();
    if (!_screen || !s_canvasBuf) return;

    _activate();
    _showMessage("Loading...");
    lv_refr_now(nullptr);   // paint "Loading..." before the blocking decode

    char err[64] = {};
    s_hasImage = _decode(path, err, sizeof(err));
    if (!s_hasImage) {
        OPS_LOG("PicView", "%s: %s", path, err);
        char msg[160];
        const char* slash = strrchr(path, '/');
        snprintf(msg, sizeof(msg), "%s\n\n%s", slash ? slash + 1 : path, err);
        _showMessage(msg);
        return;
    }

    lv_obj_add_flag(_msgLbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(_canvas);

    const char* slash = strrchr(path, '/');
    char info[128];
    snprintf(info, sizeof(info), "%s  %s", slash ? slash + 1 : path, s_info);
    lv_label_set_text(_infoLbl, info);
    lv_obj_clear_flag(_infoLbl, LV_OBJ_FLAG_HIDDEN);
}

// ── Internals ────────────────────────────────────────────────────────────

bool ScreenPicViewer::_decode(const char* path, char* err, int errMax)
{
    if (!sdcard::isMounted()) { snprintf(err, errMax, "SD card not mounted"); return false; }

    lv_canvas_fill_bg(_canvas, lv_color_black(), LV_OPA_COVER);
    s_fit.lastYieldMs = millis();

    const char* dot = strrchr(path, '.');
    bool isPng = dot && (tolower((unsigned char)dot[1]) == 'p');
    uint32_t t0 = millis();
    bool ok = isPng ? _decodePng(path, err, errMax) : _decodeJpeg(path, err, errMax);
    OPS_LOG("PicView", "%s %s in %lu ms", path, ok ? "decoded" : "failed",
            (unsigned long)(millis() - t0));
    if (!ok) lv_canvas_fill_bg(_canvas, lv_color_black(), LV_OPA_COVER);
    return ok;
}

void ScreenPicViewer::_showMessage(const char* msg)
{
    if (!_msgLbl) return;
    lv_label_set_text(_msgLbl, msg);
    lv_obj_clear_flag(_msgLbl, LV_OBJ_FLAG_HIDDEN);
    if (!s_hasImage && _infoLbl) lv_obj_add_flag(_infoLbl, LV_OBJ_FLAG_HIDDEN);
}

void ScreenPicViewer::_activate()
{
    lv_group_t* g = lv_group_get_default();
    if (g && _browseBtn) {
        lv_group_add_obj(g, _browseBtn);
        lv_group_focus_obj(_browseBtn);
    }
    lv_scr_load(_screen);
}

void ScreenPicViewer::_build()
{
    if (!s_canvasBuf)
        s_canvasBuf = (lv_color_t*)ps_malloc(CANVAS_W * CANVAS_H * sizeof(lv_color_t));
    if (!s_canvasBuf) {
        OPS_LOG("PicView", "ps_malloc failed for canvas");
        return;
    }

    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    _canvas = lv_canvas_create(_screen);
    lv_canvas_set_buffer(_canvas, s_canvasBuf, CANVAS_W, CANVAS_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(_canvas, 0, 0);
    lv_canvas_fill_bg(_canvas, lv_color_black(), LV_OPA_COVER);

    _msgLbl = lv_label_create(_screen);
    lv_label_set_long_mode(_msgLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_msgLbl, OPS_SCREEN_W - 40);
    lv_obj_set_style_text_align(_msgLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(_msgLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_msgLbl, &lv_font_montserrat_14, 0);
    lv_obj_center(_msgLbl);
    lv_label_set_text(_msgLbl, "");

    _infoLbl = lv_label_create(_screen);
    lv_label_set_long_mode(_infoLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_infoLbl, OPS_SCREEN_W);
    lv_obj_align(_infoLbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(_infoLbl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_infoLbl, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(_infoLbl, 4, 0);
    lv_obj_set_style_pad_ver(_infoLbl, 2, 0);
    lv_obj_set_style_text_color(_infoLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(_infoLbl, &lv_font_montserrat_10, 0);
    lv_obj_add_flag(_infoLbl, LV_OBJ_FLAG_HIDDEN);

    auto mkBtn = [](const char* text, int w, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_btn_create(_screen);
        lv_obj_set_size(btn, w, 28);
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, theme::ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(btn, 1, LV_STATE_FOCUSED);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_group_remove_obj(btn);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);
        return btn;
    };

    _browseBtn = mkBtn(LV_SYMBOL_DIRECTORY " Browse", 84, _onBrowse);
    lv_obj_align(_browseBtn, LV_ALIGN_TOP_LEFT, 4, 4);
    // Browse holds focus: Enter / trackball click browses, backspace closes.
    lv_obj_add_event_cb(_browseBtn, _onKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* closeBtn = mkBtn(LV_SYMBOL_CLOSE, 32, _onClose);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, -4, 4);
}

// ── Callbacks ────────────────────────────────────────────────────────────

void ScreenPicViewer::_onBrowse(lv_event_t* /*e*/)
{
    lv_group_t* g = lv_group_get_default();
    if (g && _browseBtn) lv_group_remove_obj(_browseBtn);
    ScreenFileManager::showPicker();
}

void ScreenPicViewer::_onClose(lv_event_t* /*e*/)
{
    lv_group_t* g = lv_group_get_default();
    if (g && _browseBtn) lv_group_remove_obj(_browseBtn);
    if (s_fromFileMgr) ScreenFileManager::resume();
    else               ScreenLauncher::show();
}

void ScreenPicViewer::_onKey(lv_event_t* e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC) _onClose(e);
}

}}  // namespace ops::ui
