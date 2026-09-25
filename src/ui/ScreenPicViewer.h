// Saitama — ScreenPicViewer.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Pic Viewer — shows a JPG or PNG from the SD card scaled to fit the
// 320×240 screen. Images are decoded straight into a screen-sized canvas
// (JPEG via LVGL's bundled TJpgDec with decode-time 1/2–1/8 scaling, PNG via
// PNGdec line by line), so a full-resolution photo never has to fit in RAM.
//
// Entry points: the launcher tile (show), and the File Manager's View button
// (openFile). Browse opens the File Manager in image-picker mode.

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenPicViewer {
public:
    // Opens the viewer; shows the last image, or a Browse prompt if none.
    // X / backspace return to the launcher.
    static void show();

    // Decodes and shows `path`. `fromFileManager` makes X / backspace return
    // to the File Manager instead of the launcher.
    static void openFile(const char* path, bool fromFileManager);

    // True for .jpg / .jpeg / .png (case-insensitive).
    static bool isImageFile(const char* name);

private:
    static lv_obj_t* _screen;
    static lv_obj_t* _canvas;
    static lv_obj_t* _browseBtn;
    static lv_obj_t* _msgLbl;
    static lv_obj_t* _infoLbl;

    static void _build();
    static void _activate();
    static bool _decode(const char* path, char* err, int errMax);
    static void _showMessage(const char* msg);

    static void _onBrowse(lv_event_t* e);
    static void _onClose (lv_event_t* e);
    static void _onKey   (lv_event_t* e);
};

}}  // namespace ops::ui
