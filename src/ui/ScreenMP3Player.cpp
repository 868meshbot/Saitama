// Saitama — ScreenMP3Player.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Full-screen hidden MP3 player.  Reached via /play in the terminal.
// Lists .mp3 and .ogg files found in the SD card root directory.
// OGG files appear in the list but show a "not supported" message when selected.
//
// Layout (320 × 240):
//
//   ┌──────────────────────────────────────┐  y=0
//   │ [⌂ Home]    ♫ MP3 Player             │  top bar  28 px
//   ├──────────────────────────────────────┤  y=28
//   │ File:                                │
//   │ [▼ Select a file…                  ] │  dropdown 36 px
//   │ [████████████████░░░░░░]    64 %     │  bar + %
//   │ Now Playing: filename.mp3            │
//   │ ● Playing...                         │  status
//   │                                      │
//   │     [ ▶ PLAY / ⏸ PAUSE ]  [ ■ STOP ] │  buttons
//   │                                      │
//   │ Select a file then press Play.       │  hint
//   └──────────────────────────────────────┘  y=240

#include "ScreenMP3Player.h"
#include "ScreenLauncher.h"
#include "UIScreen.h"
#include "Theme.h"
#include "../utils/MP3Player.h"
#include "../utils/SDCard.h"
#include "../utils/Log.h"
#include <SD.h>
#include <cstring>
#include <cstdio>

namespace ops { namespace ui {

// ── Static members ────────────────────────────────────────────────────────────
lv_obj_t*   ScreenMP3Player::_screen        = nullptr;
lv_obj_t*   ScreenMP3Player::_dropdown      = nullptr;
lv_obj_t*   ScreenMP3Player::_bar           = nullptr;
lv_obj_t*   ScreenMP3Player::_barPctLbl     = nullptr;
lv_obj_t*   ScreenMP3Player::_nowPlayingLbl = nullptr;
lv_obj_t*   ScreenMP3Player::_statusLbl     = nullptr;
lv_obj_t*   ScreenMP3Player::_playBtn       = nullptr;
lv_obj_t*   ScreenMP3Player::_playBtnLbl    = nullptr;
lv_timer_t* ScreenMP3Player::_timer         = nullptr;

char (*ScreenMP3Player::_filePaths)[128] = nullptr;
char (*ScreenMP3Player::_fileNames)[64]  = nullptr;
int  ScreenMP3Player::_fileCount = 0;

// ── File scanning ─────────────────────────────────────────────────────────────
static bool _hasAudioExt(const char* name)
{
    size_t n = strlen(name);
    if (n < 5) return false;
    const char* ext = name + n - 4;
    char buf[5];
    for (int i = 0; i < 4; i++)
        buf[i] = (char)(ext[i] >= 'A' && ext[i] <= 'Z' ? ext[i] + 32 : ext[i]);
    buf[4] = '\0';
    return strcmp(buf, ".mp3") == 0;
}

void ScreenMP3Player::_scanFiles()
{
    if (!_filePaths) {
        _filePaths = (char (*)[128])ps_malloc((size_t)MAX_FILES * 128);
        _fileNames = (char (*)[64]) ps_malloc((size_t)MAX_FILES * 64);
        if (!_filePaths || !_fileNames) { OPS_LOG("MP3Player", "ps_malloc failed"); return; }
    }
    _fileCount = 0;
    if (!ops::sdcard::isMounted()) {
        OPS_LOG("MP3Player", "SD not mounted — no files found");
        return;
    }

    File root = SD.open("/");
    if (!root) { OPS_LOG("MP3Player", "Cannot open SD root"); return; }

    File entry;
    while (_fileCount < MAX_FILES && (entry = root.openNextFile())) {
        if (!entry.isDirectory()) {
            const char* name = entry.name();
            // Log every entry so we can see exactly what the SD library returns
            OPS_LOG("MP3Player", "entry: [%s]", name ? name : "(null)");

            if (name && _hasAudioExt(name)) {
                // entry.name() may be a full path ("/song.mp3") or bare name
                // ("song.mp3") depending on SD lib version — normalise both.
                const char* base = strrchr(name, '/');
                base = base ? base + 1 : name;

                // Skip macOS dot-underscore sidecar files (e.g. ._Song.mp3)
                if (base[0] == '.' && base[1] == '_') {
                    entry.close();
                    continue;
                }

                strncpy(_fileNames[_fileCount], base, sizeof(_fileNames[0]) - 1);
                _fileNames[_fileCount][sizeof(_fileNames[0]) - 1] = '\0';

                // Store the exact path the SD library gave us (with leading /)
                snprintf(_filePaths[_fileCount], sizeof(_filePaths[0]), "/%s", base);
                OPS_LOG("MP3Player", "  -> stored path: [%s]", _filePaths[_fileCount]);
                _fileCount++;
            }
        }
        entry.close();
    }
    root.close();
    OPS_LOG("MP3Player", "%d MP3 file(s) found in SD root", _fileCount);
}

// ── Dropdown population ───────────────────────────────────────────────────────
void ScreenMP3Player::_populateDropdown()
{
    if (!_dropdown) return;

    if (_fileCount == 0) {
        lv_dropdown_set_options(_dropdown, "(no audio files found)");
        lv_obj_add_state(_dropdown, LV_STATE_DISABLED);
        return;
    }

    static constexpr size_t OPTS_SIZE = (size_t)MAX_FILES * 64 + 1;
    static char* opts = nullptr;
    if (!opts) opts = (char*)ps_malloc(OPTS_SIZE);
    if (!opts) return;
    opts[0] = '\0';
    for (int i = 0; i < _fileCount; i++) {
        strncat(opts, _fileNames[i], OPTS_SIZE - strlen(opts) - 2);
        if (i < _fileCount - 1) strncat(opts, "\n", OPTS_SIZE - strlen(opts) - 1);
    }
    lv_dropdown_set_options(_dropdown, opts);
    lv_obj_clear_state(_dropdown, LV_STATE_DISABLED);
}

// ── UI build ──────────────────────────────────────────────────────────────────
static constexpr int TOP_H = 28;
static constexpr int PAD   = 6;

void ScreenMP3Player::_buildScreen()
{
    _screen = lv_obj_create(nullptr);
    lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_style_bg_color(_screen, theme::BG, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top bar ───────────────────────────────────────────────────────
    lv_obj_t* bar = lv_obj_create(_screen);
    lv_obj_set_size(bar, OPS_SCREEN_W, TOP_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_set_style_pad_ver(bar, 2, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* homeBtn = lv_btn_create(bar);
    lv_group_remove_obj(homeBtn);
    lv_obj_set_height(homeBtn, TOP_H - 6);
    lv_obj_set_style_bg_color(homeBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(homeBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(homeBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(homeBtn, 1, 0);
    lv_obj_set_style_radius(homeBtn, 4, 0);
    lv_obj_set_style_shadow_width(homeBtn, 0, 0);
    lv_obj_set_style_pad_hor(homeBtn, 5, 0);
    lv_obj_add_event_cb(homeBtn, _onHome, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, LV_SYMBOL_AUDIO " MP3 Player");
    lv_obj_set_style_text_color(title, theme::TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, 0);

    // ── Content area (y = TOP_H, height = OPS_SCREEN_H - TOP_H) ──────
    int cy = TOP_H + PAD;   // current y cursor

    // "File:" label
    lv_obj_t* fileLbl = lv_label_create(_screen);
    lv_label_set_text(fileLbl, "File:");
    lv_obj_set_style_text_color(fileLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(fileLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(fileLbl, PAD, cy);
    cy += 14;

    // Dropdown
    _dropdown = lv_dropdown_create(_screen);
    lv_obj_set_size(_dropdown, OPS_SCREEN_W - PAD * 2, 34);
    lv_obj_set_pos(_dropdown, PAD, cy);
    lv_obj_set_style_text_font(_dropdown, &lv_font_montserrat_10, 0);
    lv_obj_set_style_bg_color(_dropdown, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(_dropdown, theme::BORDER, 0);
    lv_obj_set_style_text_color(_dropdown, theme::TEXT, 0);
    // Style the list that drops down
    lv_obj_t* list = lv_dropdown_get_list(_dropdown);
    lv_obj_set_style_bg_color(list, theme::BG_CARD, 0);
    lv_obj_set_style_text_color(list, theme::TEXT, 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(list, theme::BORDER, 0);
    lv_dropdown_set_options(_dropdown, "Scanning...");
    cy += 34 + PAD;

    // Progress bar
    _bar = lv_bar_create(_screen);
    lv_obj_set_size(_bar, OPS_SCREEN_W - PAD * 2 - 48, 12);
    lv_obj_set_pos(_bar, PAD, cy + 1);
    lv_bar_set_range(_bar, 0, 100);
    lv_bar_set_value(_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_bar, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(_bar, theme::ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(_bar, 3, 0);
    lv_obj_set_style_radius(_bar, 3, LV_PART_INDICATOR);

    // % label to the right of bar
    _barPctLbl = lv_label_create(_screen);
    lv_label_set_text(_barPctLbl, "  0%");
    lv_obj_set_style_text_color(_barPctLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_barPctLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(_barPctLbl, OPS_SCREEN_W - PAD - 44, cy);
    cy += 16 + PAD;

    // "Now Playing: …" label
    _nowPlayingLbl = lv_label_create(_screen);
    lv_label_set_long_mode(_nowPlayingLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_nowPlayingLbl, OPS_SCREEN_W - PAD * 2);
    lv_label_set_text(_nowPlayingLbl, "Now Playing: —");
    lv_obj_set_style_text_color(_nowPlayingLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(_nowPlayingLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(_nowPlayingLbl, PAD, cy);
    cy += 14;

    // Status label
    _statusLbl = lv_label_create(_screen);
    lv_label_set_text(_statusLbl, LV_SYMBOL_STOP " Stopped");
    lv_obj_set_style_text_color(_statusLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(_statusLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_pos(_statusLbl, PAD, cy);
    cy += 18;

    // ── Buttons ───────────────────────────────────────────────────────
    // Buttons row: centred horizontally
    static constexpr int BTN_H  = 44;
    static constexpr int BTN_W1 = 150;  // play/pause
    static constexpr int BTN_W2 = 100;  // stop
    static constexpr int BTN_GAP = 8;
    int btnsW  = BTN_W1 + BTN_GAP + BTN_W2;
    int btnsX  = (OPS_SCREEN_W - btnsW) / 2;
    int btnsY  = OPS_SCREEN_H - BTN_H - 22;  // leave room for hint at bottom

    _playBtn = lv_btn_create(_screen);
    lv_obj_set_size(_playBtn, BTN_W1, BTN_H);
    lv_obj_set_pos(_playBtn, btnsX, btnsY);
    lv_obj_set_style_bg_color(_playBtn, theme::PRIMARY, 0);
    lv_obj_set_style_bg_color(_playBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(_playBtn, 6, 0);
    lv_obj_set_style_shadow_width(_playBtn, 0, 0);
    lv_obj_add_event_cb(_playBtn, _onPlayPause, LV_EVENT_CLICKED, nullptr);

    _playBtnLbl = lv_label_create(_playBtn);
    lv_label_set_text(_playBtnLbl, LV_SYMBOL_PLAY " Play");
    lv_obj_set_style_text_color(_playBtnLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(_playBtnLbl, &lv_font_montserrat_12, 0);
    lv_obj_center(_playBtnLbl);

    lv_obj_t* stopBtn = lv_btn_create(_screen);
    lv_obj_set_size(stopBtn, BTN_W2, BTN_H);
    lv_obj_set_pos(stopBtn, btnsX + BTN_W1 + BTN_GAP, btnsY);
    lv_obj_set_style_bg_color(stopBtn, LV_COLOR_MAKE(150, 0, 0), 0);
    lv_obj_set_style_bg_color(stopBtn, LV_COLOR_MAKE(200, 30, 30), LV_STATE_PRESSED);
    lv_obj_set_style_radius(stopBtn, 6, 0);
    lv_obj_set_style_shadow_width(stopBtn, 0, 0);
    lv_obj_add_event_cb(stopBtn, _onStop, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* stopLbl = lv_label_create(stopBtn);
    lv_label_set_text(stopLbl, LV_SYMBOL_STOP " Stop");
    lv_obj_set_style_text_color(stopLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(stopLbl, &lv_font_montserrat_12, 0);
    lv_obj_center(stopLbl);

    // Bottom row: hint label + Rescan button
    lv_obj_t* rescanBtn = lv_btn_create(_screen);
    lv_obj_set_size(rescanBtn, 66, 18);
    lv_obj_align(rescanBtn, LV_ALIGN_BOTTOM_RIGHT, -4, -3);
    lv_obj_set_style_bg_color(rescanBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(rescanBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(rescanBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(rescanBtn, 1, 0);
    lv_obj_set_style_radius(rescanBtn, 3, 0);
    lv_obj_set_style_shadow_width(rescanBtn, 0, 0);
    lv_obj_set_style_pad_all(rescanBtn, 2, 0);
    lv_obj_add_event_cb(rescanBtn, _onRescan, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* rescanLbl = lv_label_create(rescanBtn);
    lv_label_set_text(rescanLbl, LV_SYMBOL_REFRESH " Rescan");
    lv_obj_set_style_text_font(rescanLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(rescanLbl, theme::ACCENT, 0);
    lv_obj_center(rescanLbl);

    lv_obj_t* hint = lv_label_create(_screen);
    lv_label_set_text(hint, "Select a file then press Play");
    lv_obj_set_style_text_color(hint, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, PAD, -5);

    // Focus / key routing: add play and stop buttons to default group
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, _dropdown);
        lv_group_add_obj(g, _playBtn);
        lv_group_add_obj(g, stopBtn);
        lv_group_focus_obj(_dropdown);
    }

    // Poll timer: 400 ms
    _timer = lv_timer_create(_timerCb, 400, nullptr);
}

// ── show() ────────────────────────────────────────────────────────────────────
void ScreenMP3Player::show(const char* path)
{
    if (!_screen) {
        _buildScreen();
        // Scan and populate file list on first open
        _scanFiles();
        _populateDropdown();
    }
    lv_scr_load(_screen);

    // Reset group focus to the play button on every visit so trackball/keyboard
    // press activates Play immediately rather than hitting a dead object from a
    // previously shown screen (e.g. Terminal textarea, Settings slider).
    {
        lv_group_t* g = lv_group_get_default();
        if (g && _playBtn) lv_group_focus_obj(_playBtn);
    }

    // If a specific path was requested, find it in the list and start it
    if (path && path[0]) {
        // Try to find it in our list
        int found = -1;
        for (int i = 0; i < _fileCount; i++) {
            if (strcmp(_filePaths[i], path) == 0) { found = i; break; }
        }
        if (found >= 0) {
            lv_dropdown_set_selected(_dropdown, (uint16_t)found);
            _startSelected();
        } else {
            // File not in scanned list — start it directly
            if (ops::mp3player::play(path)) {
                const char* base = strrchr(path, '/');
                char buf[72];
                snprintf(buf, sizeof(buf), "Now Playing: %s", base ? base + 1 : path);
                lv_label_set_text(_nowPlayingLbl, buf);
                lv_label_set_text(_statusLbl, LV_SYMBOL_AUDIO " Starting...");
                lv_obj_set_style_text_color(_statusLbl, theme::GREEN, 0);
                if (_playBtnLbl) lv_label_set_text(_playBtnLbl, LV_SYMBOL_PAUSE " Pause");
            }
        }
    }

    OPS_LOG("UI", "MP3Player shown");
}

// ── tick() ────────────────────────────────────────────────────────────────────
void ScreenMP3Player::tick() { /* driven by lv_timer */ }

// ── _startSelected() ─────────────────────────────────────────────────────────
void ScreenMP3Player::_startSelected()
{
    if (_fileCount == 0 || !_dropdown) return;
    uint16_t idx = lv_dropdown_get_selected(_dropdown);
    if ((int)idx >= _fileCount) return;

    const char* path = _filePaths[idx];
    const char* name = _fileNames[idx];

    // Stop any current playback first
    ops::mp3player::stop();

    if (ops::mp3player::play(path)) {
        char buf[72];
        snprintf(buf, sizeof(buf), "Now Playing: %s", name);
        lv_label_set_text(_nowPlayingLbl, buf);
        lv_label_set_text(_statusLbl, LV_SYMBOL_AUDIO " Starting...");
        lv_obj_set_style_text_color(_statusLbl, theme::GREEN, 0);
        // State is still Idle (task just spawned) so set button label directly.
        if (_playBtnLbl)
            lv_label_set_text(_playBtnLbl, LV_SYMBOL_PAUSE " Pause");
    } else {
        lv_label_set_text(_statusLbl, LV_SYMBOL_WARNING " Could not open file");
        lv_obj_set_style_text_color(_statusLbl, LV_COLOR_MAKE(220, 80, 0), 0);
    }
}

// ── _updatePlayPauseLabel() ───────────────────────────────────────────────────
void ScreenMP3Player::_updatePlayPauseLabel()
{
    if (!_playBtnLbl) return;
    using namespace ops::mp3player;
    State st = state();
    if (st == State::Playing) {
        lv_label_set_text(_playBtnLbl, LV_SYMBOL_PAUSE " Pause");
    } else {
        lv_label_set_text(_playBtnLbl, LV_SYMBOL_PLAY " Play");
    }
}

// ── Event callbacks ───────────────────────────────────────────────────────────
void ScreenMP3Player::_onHome(lv_event_t* /*e*/)
{
    ops::mp3player::stop();
    ScreenLauncher::show();
}

void ScreenMP3Player::_onRescan(lv_event_t* /*e*/)
{
    _scanFiles();
    _populateDropdown();
}

void ScreenMP3Player::_onPlayPause(lv_event_t* /*e*/)
{
    using namespace ops::mp3player;
    State st = state();
    OPS_LOG("MP3UI", "PlayPause tap: state=%d", (int)st);
    if (st == State::Playing) {
        pause();
        // Anticipatory update — task transitions async; timer corrects within 400 ms.
        if (_playBtnLbl) lv_label_set_text(_playBtnLbl, LV_SYMBOL_PLAY " Play");
    } else if (st == State::Paused) {
        resume();
        if (_playBtnLbl) lv_label_set_text(_playBtnLbl, LV_SYMBOL_PAUSE " Pause");
    } else {
        // Idle / Done / Error — start the selected file.
        // _startSelected sets the button label directly; don't overwrite it here.
        _startSelected();
    }
}

void ScreenMP3Player::_onStop(lv_event_t* /*e*/)
{
    OPS_LOG("MP3UI", "Stop tap: state=%d", (int)ops::mp3player::state());
    ops::mp3player::stop();
    if (_bar)       lv_bar_set_value(_bar, 0, LV_ANIM_OFF);
    if (_barPctLbl) lv_label_set_text(_barPctLbl, "  0%");
    if (_statusLbl) {
        lv_label_set_text(_statusLbl, LV_SYMBOL_STOP " Stopped");
        lv_obj_set_style_text_color(_statusLbl, theme::TEXT_MUTED, 0);
    }
    if (_playBtnLbl) lv_label_set_text(_playBtnLbl, LV_SYMBOL_PLAY " Play");
}

// ── Timer callback ────────────────────────────────────────────────────────────
void ScreenMP3Player::_timerCb(lv_timer_t* /*t*/)
{
    using namespace ops::mp3player;
    State st  = state();
    float pct = progress();
    int   pctI = (int)(pct * 100.0f);

    if (_bar)
        lv_bar_set_value(_bar, pctI, LV_ANIM_OFF);

    if (_barPctLbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%3d%%", pctI);
        lv_label_set_text(_barPctLbl, buf);
    }

    if (_statusLbl) {
        switch (st) {
            case State::Playing:
                lv_label_set_text(_statusLbl, LV_SYMBOL_AUDIO " Playing");
                lv_obj_set_style_text_color(_statusLbl, theme::GREEN, 0);
                break;
            case State::Paused:
                lv_label_set_text(_statusLbl, LV_SYMBOL_PAUSE " Paused");
                lv_obj_set_style_text_color(_statusLbl, theme::ACCENT, 0);
                break;
            case State::Done:
                lv_label_set_text(_statusLbl, LV_SYMBOL_OK " Done");
                lv_obj_set_style_text_color(_statusLbl, theme::TEXT_MUTED, 0);
                break;
            case State::Error:
                lv_label_set_text(_statusLbl, LV_SYMBOL_WARNING " Error");
                lv_obj_set_style_text_color(_statusLbl, LV_COLOR_MAKE(220, 80, 0), 0);
                break;
            case State::Stopping:
                // Task is winding down — preserve the "Stopped" text set by _onStop.
                break;
            default:
                break;
        }
    }

    _updatePlayPauseLabel();
}

}}  // namespace ops::ui
