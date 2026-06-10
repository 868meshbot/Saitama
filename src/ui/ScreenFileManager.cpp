// Saitama — ScreenFileManager.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// File manager screen layout (320 × 240 landscape):
//
//   ┌──────────────────────────────────────┐  y = 0
//   │ [⌂] /ops/msgs          [Mnt][Umnt][↺]│  top bar  28 px
//   ├──────────────────────────────────────┤  y = 28
//   │ < ..                                 │
//   │ > folder                             │  scrollable list  180 px
//   │   file.json               1.2 KB     │
//   │   file.txt                  400 B    │
//   ├──────────────────────────────────────┤  y = 208
//   │  [Copy]  [Paste]  [Delete]  [Open]   │  action bar  32 px
//   └──────────────────────────────────────┘  y = 240

#include "ScreenFileManager.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../utils/SDCard.h"
#include "../utils/Log.h"
#include <SD.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <esp_heap_caps.h>

namespace ops { namespace ui {

// ── Layout constants ──────────────────────────────────────────────────────────
static constexpr int TOP_H   = 28;
static constexpr int ACT_H   = 32;
static constexpr int BODY_H  = OPS_SCREEN_H - TOP_H - ACT_H;  // 180
static constexpr int ROW_H   = 26;

// ── Static member definitions ─────────────────────────────────────────────────
lv_obj_t* ScreenFileManager::_screen      = nullptr;
lv_obj_t* ScreenFileManager::_pathLbl     = nullptr;
lv_obj_t* ScreenFileManager::_listBox     = nullptr;
lv_obj_t* ScreenFileManager::_copyBtn       = nullptr;
lv_obj_t* ScreenFileManager::_pasteBtn     = nullptr;
lv_obj_t* ScreenFileManager::_deleteBtn    = nullptr;
lv_obj_t* ScreenFileManager::_renameBtn    = nullptr;
lv_obj_t* ScreenFileManager::_openBtn      = nullptr;
lv_obj_t* ScreenFileManager::_viewScreen   = nullptr;
lv_obj_t* ScreenFileManager::s_renameOverlay = nullptr;
lv_obj_t* ScreenFileManager::s_renameInput  = nullptr;

char    ScreenFileManager::s_curPath[128]                    = "/";
char    ScreenFileManager::s_clipboard[128]                  = {};
char    ScreenFileManager::s_entries[MAX_ENTRIES][64]        = {};
bool    ScreenFileManager::s_isDir[MAX_ENTRIES]              = {};
size_t  ScreenFileManager::s_sizes[MAX_ENTRIES]              = {};
int     ScreenFileManager::s_entryCount                      = 0;
int     ScreenFileManager::s_selectedIdx                     = -1;
lv_obj_t* ScreenFileManager::_rows[MAX_ENTRIES]              = {};
char*   ScreenFileManager::s_viewBuf                         = nullptr;

// ── File-scope helpers ────────────────────────────────────────────────────────

static void styleSmallBtn(lv_obj_t* btn)
{
    lv_obj_set_style_bg_color(btn, theme::BG, 0);
    lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, theme::BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
}

static void fmtSize(size_t bytes, char* buf, size_t bufSz)
{
    if (bytes < 1024)
        snprintf(buf, bufSz, "%u B", (unsigned)bytes);
    else if (bytes < 1024UL * 1024UL)
        snprintf(buf, bufSz, "%.1f KB", bytes / 1024.0);
    else
        snprintf(buf, bufSz, "%.1f MB", bytes / (1024.0 * 1024.0));
}

// ── show() ────────────────────────────────────────────────────────────────────
void ScreenFileManager::show()
{
    // Reset to root on fresh entry
    strncpy(s_curPath, "/", sizeof(s_curPath));
    s_clipboard[0]   = '\0';
    s_selectedIdx    = -1;
    memset(_rows, 0, sizeof(_rows));

    lv_obj_t* old = _screen;
    _screen = nullptr;
    _buildScreen();
    if (old) lv_obj_del(old);
}

// ── _scanDir() ────────────────────────────────────────────────────────────────
void ScreenFileManager::_scanDir()
{
    s_entryCount  = 0;
    s_selectedIdx = -1;

    if (!sdcard::isMounted()) return;

    File dir = SD.open(s_curPath);
    if (!dir || !dir.isDirectory()) {
        dir.close();
        // Path no longer valid — fall back to root
        strncpy(s_curPath, "/", sizeof(s_curPath));
        dir = SD.open("/");
        if (!dir || !dir.isDirectory()) { dir.close(); return; }
    }

    while (s_entryCount < MAX_ENTRIES) {
        File entry = dir.openNextFile();
        if (!entry) break;

        const char* full = entry.name();
        const char* base = strrchr(full, '/');
        base = base ? base + 1 : full;

        if (!base[0] || base[0] == '.') { entry.close(); continue; }

        strncpy(s_entries[s_entryCount], base, sizeof(s_entries[0]) - 1);
        s_entries[s_entryCount][sizeof(s_entries[0]) - 1] = '\0';
        s_isDir[s_entryCount]  = entry.isDirectory();
        s_sizes[s_entryCount]  = entry.isDirectory() ? 0 : (size_t)entry.size();
        s_entryCount++;
        entry.close();
    }
    dir.close();
}

// ── _getFullPath() ────────────────────────────────────────────────────────────
void ScreenFileManager::_getFullPath(int idx, char* out, size_t outSz)
{
    if (strcmp(s_curPath, "/") == 0)
        snprintf(out, outSz, "/%s", s_entries[idx]);
    else
        snprintf(out, outSz, "%s/%s", s_curPath, s_entries[idx]);
}

// ── _selectRow() ─────────────────────────────────────────────────────────────
void ScreenFileManager::_selectRow(int idx)
{
    // De-highlight previous
    if (s_selectedIdx >= 0 && s_selectedIdx < MAX_ENTRIES && _rows[s_selectedIdx]) {
        lv_obj_set_style_bg_color(_rows[s_selectedIdx], theme::BG, 0);
        lv_obj_set_style_bg_opa(_rows[s_selectedIdx], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_side(_rows[s_selectedIdx], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(_rows[s_selectedIdx], theme::BORDER, 0);
        lv_obj_set_style_border_width(_rows[s_selectedIdx], 1, 0);
    }
    s_selectedIdx = idx;
    // Highlight new
    if (idx >= 0 && idx < MAX_ENTRIES && _rows[idx]) {
        lv_obj_set_style_bg_color(_rows[idx], theme::BG_CARD, 0);
        lv_obj_set_style_bg_opa(_rows[idx], LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(_rows[idx],
            LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(_rows[idx], theme::ACCENT, 0);
        lv_obj_set_style_border_width(_rows[idx], 2, 0);
    }
    _updateActionBtns();
}

// ── _updateActionBtns() ───────────────────────────────────────────────────────
void ScreenFileManager::_updateActionBtns()
{
    bool hasSel  = (s_selectedIdx >= 0 && s_selectedIdx < s_entryCount
                    && !s_isDir[s_selectedIdx]);
    bool hasClip = (s_clipboard[0] != '\0' && sdcard::isMounted());
    bool canOpen = hasSel && _isViewable(s_entries[s_selectedIdx]);

    auto setEn = [](lv_obj_t* btn, bool en) {
        if (!btn) return;
        if (en) {
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_state(btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_state(btn, LV_STATE_DISABLED);
        }
    };

    setEn(_copyBtn,   hasSel);
    setEn(_pasteBtn,  hasClip);
    setEn(_deleteBtn, hasSel);
    setEn(_renameBtn, hasSel);
    setEn(_openBtn,   canOpen);
}

// ── _isViewable() ─────────────────────────────────────────────────────────────
bool ScreenFileManager::_isViewable(const char* name)
{
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcmp(dot, ".json") == 0
         || strcmp(dot, ".txt")  == 0
         || strcmp(dot, ".log")  == 0);
}

// ── _buildPasteName() ─────────────────────────────────────────────────────────
void ScreenFileManager::_buildPasteName(const char* src, const char* dstDir,
                                        char* out, size_t outSz)
{
    const char* slash = strrchr(src, '/');
    const char* name  = slash ? slash + 1 : src;

    // Split into base + ext
    const char* dot = strrchr(name, '.');
    char base[56] = {};
    char ext[16]  = {};
    if (dot && dot != name) {
        size_t blen = (size_t)(dot - name);
        if (blen >= sizeof(base)) blen = sizeof(base) - 1;
        memcpy(base, name, blen);
        strncpy(ext, dot, sizeof(ext) - 1);
    } else {
        strncpy(base, name, sizeof(base) - 1);
    }

    bool trailingSlash = (dstDir[strlen(dstDir) - 1] == '/');

    auto buildPath = [&](int n) {
        if (n == 0) {
            if (trailingSlash)
                snprintf(out, outSz, "%s%s%s", dstDir, base, ext);
            else
                snprintf(out, outSz, "%s/%s%s", dstDir, base, ext);
        } else {
            if (trailingSlash)
                snprintf(out, outSz, "%s%s_%d%s", dstDir, base, n, ext);
            else
                snprintf(out, outSz, "%s/%s_%d%s", dstDir, base, n, ext);
        }
    };

    buildPath(0);
    if (!SD.exists(out)) return;
    for (int i = 1; i <= 9; i++) {
        buildPath(i);
        if (!SD.exists(out)) return;
    }
    buildPath(10);  // fallback — will overwrite if exists
}

// ── _buildScreen() ────────────────────────────────────────────────────────────
void ScreenFileManager::_buildScreen()
{
    _scanDir();

    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ───────────────────────────────────────────────────────────────
    // Flex row: [⌂(26)][path(180)][Mnt(32)][Umnt(36)][↺(26)]
    // pad_hor=4, pad_col=3 → 4+26+3+180+3+32+3+36+3+26+4 = 320 ✓
    lv_obj_t* topBar = lv_obj_create(_screen);
    lv_obj_set_size(topBar, OPS_SCREEN_W, TOP_H);
    lv_obj_align(topBar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(topBar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(topBar, 0, 0);
    lv_obj_set_style_radius(topBar, 0, 0);
    lv_obj_set_style_pad_hor(topBar, 4, 0);
    lv_obj_set_style_pad_ver(topBar, 2, 0);
    lv_obj_set_style_pad_column(topBar, 3, 0);
    lv_obj_clear_flag(topBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(topBar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topBar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Home button
    lv_obj_t* homeBtn = lv_btn_create(topBar);
    lv_obj_set_size(homeBtn, 26, 22);
    styleSmallBtn(homeBtn);
    lv_obj_add_event_cb(homeBtn, _onHomeClick, LV_EVENT_CLICKED, nullptr);
    lv_group_remove_obj(homeBtn);
    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    // Path label (180 px, truncates with …)
    _pathLbl = lv_label_create(topBar);
    lv_label_set_long_mode(_pathLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_pathLbl, 180);
    lv_obj_set_style_text_color(_pathLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_pathLbl, &lv_font_montserrat_10, 0);
    lv_label_set_text(_pathLbl, s_curPath);

    // Mount button
    lv_obj_t* mntBtn = lv_btn_create(topBar);
    lv_obj_set_size(mntBtn, 32, 22);
    styleSmallBtn(mntBtn);
    lv_obj_add_event_cb(mntBtn, _onMountClick, LV_EVENT_CLICKED, nullptr);
    lv_group_remove_obj(mntBtn);
    lv_obj_t* mntLbl = lv_label_create(mntBtn);
    lv_label_set_text(mntLbl, "Mnt");
    lv_obj_set_style_text_color(mntLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(mntLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(mntLbl);

    // Unmount button
    lv_obj_t* unmntBtn = lv_btn_create(topBar);
    lv_obj_set_size(unmntBtn, 36, 22);
    styleSmallBtn(unmntBtn);
    lv_obj_add_event_cb(unmntBtn, _onUnmountClick, LV_EVENT_CLICKED, nullptr);
    lv_group_remove_obj(unmntBtn);
    lv_obj_t* unmntLbl = lv_label_create(unmntBtn);
    lv_label_set_text(unmntLbl, "Umnt");
    lv_obj_set_style_text_color(unmntLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(unmntLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(unmntLbl);

    // Rescan button
    lv_obj_t* rescanBtn = lv_btn_create(topBar);
    lv_obj_set_size(rescanBtn, 26, 22);
    styleSmallBtn(rescanBtn);
    lv_obj_add_event_cb(rescanBtn, _onRescanClick, LV_EVENT_CLICKED, nullptr);
    lv_group_remove_obj(rescanBtn);
    lv_obj_t* rescanLbl = lv_label_create(rescanBtn);
    lv_label_set_text(rescanLbl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(rescanLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(rescanLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(rescanLbl);

    // ── List body ─────────────────────────────────────────────────────────────
    _listBox = lv_obj_create(_screen);
    lv_obj_set_size(_listBox, OPS_SCREEN_W, BODY_H);
    lv_obj_align(_listBox, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(_listBox, theme::BG, 0);
    lv_obj_set_style_border_width(_listBox, 0, 0);
    lv_obj_set_style_radius(_listBox, 0, 0);
    lv_obj_set_style_pad_all(_listBox, 0, 0);
    lv_obj_set_style_pad_row(_listBox, 0, 0);
    lv_obj_set_scroll_dir(_listBox, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_listBox, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(_listBox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_listBox, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Add to default group so backspace exits to launcher
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, _listBox);
        lv_group_focus_obj(_listBox);
    }
    lv_obj_add_event_cb(_listBox, [](lv_event_t* e) {
        if (lv_event_get_key(e) == LV_KEY_ESC) ScreenLauncher::show();
    }, LV_EVENT_KEY, nullptr);

    // ".." row — shown when not at root
    bool atRoot = (strcmp(s_curPath, "/") == 0);
    if (!atRoot) {
        lv_obj_t* upRow = lv_obj_create(_listBox);
        lv_obj_set_size(upRow, OPS_SCREEN_W, ROW_H);
        lv_obj_set_style_bg_color(upRow, theme::BG, 0);
        lv_obj_set_style_border_width(upRow, 1, 0);
        lv_obj_set_style_border_side(upRow, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(upRow, theme::BORDER, 0);
        lv_obj_set_style_radius(upRow, 0, 0);
        lv_obj_set_style_pad_all(upRow, 0, 0);
        lv_obj_clear_flag(upRow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(upRow, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(upRow, _onRowClick, LV_EVENT_CLICKED, nullptr);
        lv_obj_set_user_data(upRow, (void*)(intptr_t)(-1));

        lv_obj_t* upLbl = lv_label_create(upRow);
        lv_label_set_long_mode(upLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_size(upLbl, OPS_SCREEN_W - 8, ROW_H - 2);
        lv_obj_set_pos(upLbl, 4, 1);
        lv_obj_set_style_text_color(upLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(upLbl, &lv_font_montserrat_10, 0);
        lv_label_set_text(upLbl, "< ..");
    }

    // Empty state
    if (s_entryCount == 0) {
        lv_obj_t* emptyLbl = lv_label_create(_listBox);
        lv_obj_set_style_pad_all(emptyLbl, 8, 0);
        lv_obj_set_style_text_color(emptyLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(emptyLbl, &lv_font_montserrat_10, 0);
        lv_label_set_text(emptyLbl,
            sdcard::isMounted() ? "(empty)" : "SD not mounted — tap Mnt");
    }

    // File/dir rows
    for (int i = 0; i < s_entryCount; i++) {
        lv_obj_t* row = lv_obj_create(_listBox);
        lv_obj_set_size(row, OPS_SCREEN_W, ROW_H);
        lv_obj_set_style_bg_color(row, theme::BG, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, theme::BORDER, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, _onRowClick, LV_EVENT_CLICKED, nullptr);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        _rows[i] = row;

        // Name label: 260 px wide (4px left gap, 56px reserved for size)
        char nameBuf[68];
        if (s_isDir[i])
            snprintf(nameBuf, sizeof(nameBuf), "> %s", s_entries[i]);
        else
            snprintf(nameBuf, sizeof(nameBuf), "  %s", s_entries[i]);

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_size(nameLbl, OPS_SCREEN_W - 60, ROW_H - 2);
        lv_obj_set_pos(nameLbl, 4, 1);
        lv_obj_set_style_text_color(nameLbl,
            s_isDir[i] ? theme::ACCENT : theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, theme::bodyFont10(), 0);
        lv_label_set_text(nameLbl, nameBuf);

        // Size label: 56 px wide, right of name
        lv_obj_t* sizeLbl = lv_label_create(row);
        lv_obj_set_size(sizeLbl, 56, ROW_H - 2);
        lv_obj_set_pos(sizeLbl, OPS_SCREEN_W - 56, 1);
        lv_obj_set_style_text_color(sizeLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(sizeLbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_align(sizeLbl, LV_TEXT_ALIGN_RIGHT, 0);
        if (!s_isDir[i]) {
            char sizeStr[16];
            fmtSize(s_sizes[i], sizeStr, sizeof(sizeStr));
            lv_label_set_text(sizeLbl, sizeStr);
        } else {
            lv_label_set_text(sizeLbl, "");
        }
    }

    // ── Action bar ────────────────────────────────────────────────────────────
    // 4 buttons absolute-positioned within the bar (pad_all=0)
    // Copy@4, Paste@81, Delete@158, Open@242  (widths 73, 73, 80, 74)
    lv_obj_t* actBar = lv_obj_create(_screen);
    lv_obj_set_size(actBar, OPS_SCREEN_W, ACT_H);
    lv_obj_align(actBar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(actBar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(actBar, 0, 0);
    lv_obj_set_style_radius(actBar, 0, 0);
    lv_obj_set_style_pad_all(actBar, 0, 0);
    lv_obj_clear_flag(actBar, LV_OBJ_FLAG_SCROLLABLE);

    auto makeActBtn = [&](const char* label, int x, int w,
                          lv_event_cb_t cb, lv_color_t bgCol) -> lv_obj_t* {
        lv_obj_t* btn = lv_btn_create(actBar);
        lv_obj_set_size(btn, w, 26);
        lv_obj_set_pos(btn, x, 3);
        lv_obj_set_style_bg_color(btn, bgCol, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_group_remove_obj(btn);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
        return btn;
    };

    // 5 buttons × 60 px + 4 gaps × 3 px + 8 px side pad = 320 ✓
    // x positions: Copy=4, Paste=67, Delete=130, Rename=193, Open=256
    _copyBtn   = makeActBtn("Copy",   4,   60, _onCopyClick,   theme::PRIMARY);
    _pasteBtn  = makeActBtn("Paste",  67,  60, _onPasteClick,  theme::PRIMARY);
    _deleteBtn = makeActBtn("Delete", 130, 60, _onDeleteClick, theme::RED);
    _renameBtn = makeActBtn("Rename", 193, 60, _onRenameClick, theme::ORANGE);
    _openBtn   = makeActBtn("Open",   256, 60, _onOpenClick,   theme::PRIMARY);

    _updateActionBtns();

    lv_scr_load(_screen);
}

// ── _rebuild() ────────────────────────────────────────────────────────────────
void ScreenFileManager::_rebuild()
{
    // Null dialog pointers before deleting the screen they live in
    s_renameOverlay = nullptr;
    s_renameInput   = nullptr;

    lv_obj_t* old = _screen;
    _screen = nullptr;
    memset(_rows, 0, sizeof(_rows));
    _buildScreen();
    // Async delete so any widget that triggered this rebuild has already
    // returned from its event callback before the screen is freed.
    if (old) lv_obj_del_async(old);
}

// ── _openViewer() ─────────────────────────────────────────────────────────────
void ScreenFileManager::_openViewer(const char* path)
{
    static constexpr size_t VIEW_BUF_SZ = 4096;

    if (!s_viewBuf) {
        s_viewBuf = (char*)ps_malloc(VIEW_BUF_SZ);
        if (!s_viewBuf) {
            OPS_LOG("FileMgr", "ps_malloc failed for viewer");
            return;
        }
    }

    File f = SD.open(path, FILE_READ);
    if (!f) {
        OPS_LOG("FileMgr", "Viewer: open failed %s", path);
        return;
    }
    size_t fileSize = (size_t)f.size();
    bool   truncated = (fileSize >= VIEW_BUF_SZ - 1);
    size_t toRead    = truncated ? VIEW_BUF_SZ - 1 : fileSize;
    size_t n         = f.read((uint8_t*)s_viewBuf, toRead);
    s_viewBuf[n] = '\0';
    f.close();

    if (_viewScreen) { lv_obj_del(_viewScreen); _viewScreen = nullptr; }

    _viewScreen = lv_obj_create(nullptr);
    lv_obj_set_size(_viewScreen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_viewScreen, theme::BG, 0);
    lv_obj_set_style_pad_all(_viewScreen, 0, 0);
    lv_obj_clear_flag(_viewScreen, LV_OBJ_FLAG_SCROLLABLE);

    // Top bar
    lv_obj_t* vBar = lv_obj_create(_viewScreen);
    lv_obj_set_size(vBar, OPS_SCREEN_W, TOP_H);
    lv_obj_align(vBar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(vBar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(vBar, 0, 0);
    lv_obj_set_style_radius(vBar, 0, 0);
    lv_obj_set_style_pad_all(vBar, 0, 0);
    lv_obj_clear_flag(vBar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* backBtn = lv_btn_create(vBar);
    lv_obj_set_size(backBtn, 26, 22);
    lv_obj_set_pos(backBtn, 2, 3);
    styleSmallBtn(backBtn);
    lv_obj_add_event_cb(backBtn, _onViewBackClick, LV_EVENT_CLICKED, nullptr);
    lv_group_remove_obj(backBtn);
    lv_obj_t* backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(backLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(backLbl);

    // Filename title
    const char* slash = strrchr(path, '/');
    const char* fname = slash ? slash + 1 : path;
    char title[72];
    if (truncated)
        snprintf(title, sizeof(title), "%s  [first 4 KB]", fname);
    else
        snprintf(title, sizeof(title), "%s", fname);

    lv_obj_t* titleLbl = lv_label_create(vBar);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_size(titleLbl, OPS_SCREEN_W - 36, 16);
    lv_obj_set_pos(titleLbl, 32, 6);
    lv_obj_set_style_text_color(titleLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(titleLbl, &lv_font_montserrat_10, 0);
    lv_label_set_text(titleLbl, title);

    // Scrollable content area
    lv_obj_t* body = lv_obj_create(_viewScreen);
    lv_obj_set_size(body, OPS_SCREEN_W, OPS_SCREEN_H - TOP_H);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(body, theme::BG, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_all(body, 4, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);

    lv_obj_t* contentLbl = lv_label_create(body);
    lv_label_set_long_mode(contentLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(contentLbl, OPS_SCREEN_W - 8);
    lv_obj_set_style_text_color(contentLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(contentLbl, theme::bodyFont10(), 0);
    lv_label_set_text(contentLbl, s_viewBuf);

    // Focus body so trackball scrolls and backspace exits
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, body);
        lv_group_focus_obj(body);
    }
    lv_obj_add_event_cb(body, _onViewKey, LV_EVENT_KEY, nullptr);

    lv_scr_load(_viewScreen);
}

// ── Callbacks ─────────────────────────────────────────────────────────────────

void ScreenFileManager::_onHomeClick(lv_event_t*)
{
    ScreenLauncher::show();
}

void ScreenFileManager::_onMountClick(lv_event_t*)
{
    sdcard::tryMount();
    _rebuild();
}

void ScreenFileManager::_onUnmountClick(lv_event_t*)
{
    sdcard::unmount();
    _rebuild();
}

void ScreenFileManager::_onRescanClick(lv_event_t*)
{
    _rebuild();
}

void ScreenFileManager::_onRowClick(lv_event_t* e)
{
    lv_obj_t*  row = lv_event_get_target(e);
    intptr_t   idx = (intptr_t)lv_obj_get_user_data(row);

    if (idx == (intptr_t)(-1)) {
        // Navigate up — strip last path component
        char* last = strrchr(s_curPath, '/');
        if (last && last != s_curPath)
            *last = '\0';
        else
            strncpy(s_curPath, "/", sizeof(s_curPath));
        _rebuild();
        return;
    }

    if (idx < 0 || idx >= s_entryCount) return;

    if (s_isDir[(int)idx]) {
        // Navigate into directory
        char newPath[128];
        if (strcmp(s_curPath, "/") == 0)
            snprintf(newPath, sizeof(newPath), "/%s", s_entries[idx]);
        else
            snprintf(newPath, sizeof(newPath), "%s/%s", s_curPath, s_entries[idx]);
        strncpy(s_curPath, newPath, sizeof(s_curPath) - 1);
        s_curPath[sizeof(s_curPath) - 1] = '\0';
        _rebuild();
    } else {
        // Toggle file selection
        _selectRow((s_selectedIdx == (int)idx) ? -1 : (int)idx);
    }
}

void ScreenFileManager::_onCopyClick(lv_event_t*)
{
    if (s_selectedIdx < 0 || s_selectedIdx >= s_entryCount) return;
    if (s_isDir[s_selectedIdx]) return;
    _getFullPath(s_selectedIdx, s_clipboard, sizeof(s_clipboard));
    OPS_LOG("FileMgr", "Clipboard: %s", s_clipboard);
    _updateActionBtns();
}

void ScreenFileManager::_onPasteClick(lv_event_t*)
{
    if (!s_clipboard[0] || !sdcard::isMounted()) return;

    char dest[192];
    _buildPasteName(s_clipboard, s_curPath, dest, sizeof(dest));

    File src = SD.open(s_clipboard, FILE_READ);
    if (!src) { OPS_LOG("FileMgr", "Paste src open failed: %s", s_clipboard); return; }
    File dst = SD.open(dest, FILE_WRITE);
    if (!dst) {
        src.close();
        OPS_LOG("FileMgr", "Paste dst create failed: %s", dest);
        return;
    }

    uint8_t buf[512];
    while (true) {
        int n = (int)src.read(buf, sizeof(buf));
        if (n <= 0) break;
        dst.write(buf, (size_t)n);
    }
    src.close();
    dst.close();

    OPS_LOG("FileMgr", "Pasted: %s -> %s", s_clipboard, dest);
    _rebuild();
}

void ScreenFileManager::_onDeleteClick(lv_event_t*)
{
    if (s_selectedIdx < 0 || s_selectedIdx >= s_entryCount) return;
    if (s_isDir[s_selectedIdx]) return;

    char path[192];
    _getFullPath(s_selectedIdx, path, sizeof(path));

    if (SD.remove(path)) {
        OPS_LOG("FileMgr", "Deleted: %s", path);
        if (strcmp(s_clipboard, path) == 0) s_clipboard[0] = '\0';
    } else {
        OPS_LOG("FileMgr", "Delete failed: %s", path);
    }
    _rebuild();
}

void ScreenFileManager::_onOpenClick(lv_event_t*)
{
    if (s_selectedIdx < 0 || s_selectedIdx >= s_entryCount) return;
    if (s_isDir[s_selectedIdx]) return;
    if (!_isViewable(s_entries[s_selectedIdx])) return;

    char path[192];
    _getFullPath(s_selectedIdx, path, sizeof(path));
    _openViewer(path);
}

// ── _openRenameDialog() ───────────────────────────────────────────────────────
void ScreenFileManager::_openRenameDialog()
{
    if (s_selectedIdx < 0 || s_selectedIdx >= s_entryCount) return;
    if (s_isDir[s_selectedIdx]) return;
    if (s_renameOverlay) return;  // already open

    // Full-screen dim layer that also blocks touch-through
    s_renameOverlay = lv_obj_create(_screen);
    lv_obj_set_size(s_renameOverlay, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_renameOverlay, 0, 0);
    lv_obj_set_style_bg_color(s_renameOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_renameOverlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_renameOverlay, 0, 0);
    lv_obj_set_style_radius(s_renameOverlay, 0, 0);
    lv_obj_set_style_pad_all(s_renameOverlay, 0, 0);
    lv_obj_clear_flag(s_renameOverlay, LV_OBJ_FLAG_SCROLLABLE);

    // Dialog box — centered, auto height
    lv_obj_t* box = lv_obj_create(s_renameOverlay);
    lv_obj_set_width(box, 284);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(box, theme::BORDER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(box);
    lv_label_set_text(title, "Rename File");
    lv_obj_set_style_text_color(title, theme::TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    // Textarea — pre-filled with current filename
    s_renameInput = lv_textarea_create(box);
    lv_obj_set_width(s_renameInput, 260);
    lv_textarea_set_one_line(s_renameInput, true);
    lv_textarea_set_text(s_renameInput, s_entries[s_selectedIdx]);
    lv_obj_set_style_bg_color(s_renameInput, theme::BG, 0);
    lv_obj_set_style_text_color(s_renameInput, theme::TEXT, 0);
    lv_obj_set_style_border_color(s_renameInput, theme::BORDER, 0);
    lv_obj_set_style_border_color(s_renameInput, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(s_renameInput, theme::bodyFont10(), 0);

    // Button row
    lv_obj_t* btnRow = lv_obj_create(box);
    lv_obj_set_size(btnRow, 260, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_style_pad_column(btnRow, 8, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto makeDialogBtn = [&](const char* label, lv_color_t bg, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_btn_create(btnRow);
        lv_obj_set_size(btn, 82, 28);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_group_remove_obj(btn);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
    };

    makeDialogBtn("Save",   theme::PRIMARY, _onRenameSave);
    makeDialogBtn("Cancel", theme::BG,      _onRenameCancel);

    // Focus textarea so BBQ10 keyboard types into it
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, s_renameInput);
        lv_group_focus_obj(s_renameInput);
    }
}

void ScreenFileManager::_onRenameClick(lv_event_t*)
{
    _openRenameDialog();
}

void ScreenFileManager::_onRenameSave(lv_event_t*)
{
    if (!s_renameOverlay || !s_renameInput) return;
    if (s_selectedIdx < 0 || s_selectedIdx >= s_entryCount) return;

    const char* newName = lv_textarea_get_text(s_renameInput);
    if (!newName || !newName[0]) return;

    // Reject names containing path separators
    for (const char* p = newName; *p; p++) {
        if (*p == '/') return;
    }

    if (strcmp(newName, s_entries[s_selectedIdx]) != 0) {
        char oldPath[192], newPath[192];
        _getFullPath(s_selectedIdx, oldPath, sizeof(oldPath));
        if (strcmp(s_curPath, "/") == 0)
            snprintf(newPath, sizeof(newPath), "/%s", newName);
        else
            snprintf(newPath, sizeof(newPath), "%s/%s", s_curPath, newName);

        if (SD.rename(oldPath, newPath))
            OPS_LOG("FileMgr", "Renamed: %s -> %s", oldPath, newPath);
        else
            OPS_LOG("FileMgr", "Rename failed: %s", oldPath);
    }

    // Remove textarea from group before the screen is torn down
    lv_group_t* g = lv_group_get_default();
    if (g && s_renameInput) lv_group_remove_obj(s_renameInput);
    s_renameOverlay = nullptr;
    s_renameInput   = nullptr;

    // _rebuild() uses lv_obj_del_async for the old screen, so it's safe to
    // call directly here even though we're inside a callback on that screen.
    _rebuild();
}

void ScreenFileManager::_onRenameCancel(lv_event_t*)
{
    if (!s_renameOverlay) return;

    lv_group_t* g = lv_group_get_default();
    if (g && s_renameInput) {
        lv_group_remove_obj(s_renameInput);
        if (_listBox) lv_group_focus_obj(_listBox);
    }

    lv_obj_t* ov  = s_renameOverlay;
    s_renameOverlay = nullptr;
    s_renameInput   = nullptr;
    if (ov) lv_obj_del_async(ov);
}

void ScreenFileManager::_onViewKey(lv_event_t* e)
{
    if (lv_event_get_key(e) != LV_KEY_ESC) return;

    // Remove the focused body widget from the group before it disappears.
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_obj_t* focused = lv_group_get_focused(g);
        if (focused) lv_group_remove_obj(focused);
        if (_listBox) lv_group_focus_obj(_listBox);
    }

    lv_obj_t* vs = _viewScreen;
    _viewScreen = nullptr;
    if (_screen) lv_scr_load(_screen);

    // Defer delete: the body that owns this callback is a child of vs.
    // Freeing vs synchronously here would crash LVGL mid-dispatch.
    if (vs) lv_obj_del_async(vs);
}

void ScreenFileManager::_onViewBackClick(lv_event_t*)
{
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_obj_t* focused = lv_group_get_focused(g);
        if (focused) lv_group_remove_obj(focused);
        if (_listBox) lv_group_focus_obj(_listBox);
    }

    lv_obj_t* vs = _viewScreen;
    _viewScreen = nullptr;
    if (_screen) lv_scr_load(_screen);
    if (vs) lv_obj_del_async(vs);
}

}}  // namespace ops::ui
