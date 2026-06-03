// Saitama — ScreenMP3Player.h
// Copyright 2026 Saitama — MIT License

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenMP3Player {
public:
    // Show the player screen.  If path is non-null, immediately start that file.
    static void show(const char* path = nullptr);

    // Call from UIScreen::tick() so the timer updates even while this is active.
    // (Not strictly needed — lv_timer runs from lv_task_handler — but kept for
    //  symmetry with other screens.)
    static void tick();

private:
    static lv_obj_t*   _screen;
    static lv_obj_t*   _dropdown;
    static lv_obj_t*   _bar;
    static lv_obj_t*   _barPctLbl;
    static lv_obj_t*   _nowPlayingLbl;
    static lv_obj_t*   _statusLbl;
    static lv_obj_t*   _playBtn;
    static lv_obj_t*   _playBtnLbl;
    static lv_timer_t* _timer;

    // File list built from SD root scan
    static constexpr int MAX_FILES = 64;
    static char  (*_filePaths)[128];  // ps_malloc'd in _scanFiles(); full SD paths
    static char  (*_fileNames)[64];   // ps_malloc'd in _scanFiles(); basenames
    static int   _fileCount;

    static void _buildScreen();
    static void _scanFiles();
    static void _populateDropdown();

    static void _startSelected();
    static void _updatePlayPauseLabel();

    static void _onHome     (lv_event_t* e);
    static void _onPlayPause(lv_event_t* e);
    static void _onStop     (lv_event_t* e);
    static void _onRescan   (lv_event_t* e);
    static void _timerCb    (lv_timer_t* t);
};

}}  // namespace ops::ui
