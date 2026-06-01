// Saitama — ScreenFileManager.h
// Copyright 2026 Saitama — MIT License

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenFileManager {
public:
    static void show();

private:
    // LVGL objects (rebuilt on each directory change)
    static lv_obj_t* _screen;
    static lv_obj_t* _pathLbl;
    static lv_obj_t* _listBox;
    static lv_obj_t* _copyBtn;
    static lv_obj_t* _pasteBtn;
    static lv_obj_t* _deleteBtn;
    static lv_obj_t* _renameBtn;
    static lv_obj_t* _openBtn;
    static lv_obj_t* _viewScreen;
    static lv_obj_t* s_renameOverlay;   // modal rename dialog (child of _screen)
    static lv_obj_t* s_renameInput;     // textarea inside the dialog

    // Directory listing state (persists across rebuilds)
    static constexpr int MAX_ENTRIES = 64;
    static char    s_curPath[128];
    static char    s_clipboard[128];     // empty = nothing copied
    static char    s_entries[MAX_ENTRIES][64];
    static bool    s_isDir[MAX_ENTRIES];
    static size_t  s_sizes[MAX_ENTRIES];
    static int     s_entryCount;
    static int     s_selectedIdx;        // -1 = none; only files (not dirs)
    static lv_obj_t* _rows[MAX_ENTRIES]; // row widgets, aligned with s_entries[]
    static char*   s_viewBuf;            // ps_malloc'd 4 KB for file viewer

    static void _buildScreen();
    static void _scanDir();
    static void _rebuild();
    static void _selectRow(int idx);
    static void _updateActionBtns();
    static void _openViewer(const char* path);
    static void _openRenameDialog();
    static bool _isViewable(const char* name);
    static void _buildPasteName(const char* src, const char* dstDir,
                                char* out, size_t outSz);
    static void _getFullPath(int idx, char* out, size_t outSz);

    // Callbacks
    static void _onHomeClick    (lv_event_t* e);
    static void _onMountClick   (lv_event_t* e);
    static void _onUnmountClick (lv_event_t* e);
    static void _onRescanClick  (lv_event_t* e);
    static void _onRowClick     (lv_event_t* e);
    static void _onCopyClick    (lv_event_t* e);
    static void _onPasteClick   (lv_event_t* e);
    static void _onDeleteClick  (lv_event_t* e);
    static void _onRenameClick  (lv_event_t* e);
    static void _onRenameSave   (lv_event_t* e);
    static void _onRenameCancel (lv_event_t* e);
    static void _onOpenClick    (lv_event_t* e);
    static void _onViewKey      (lv_event_t* e);
    static void _onViewBackClick(lv_event_t* e);
};

}}  // namespace ops::ui
