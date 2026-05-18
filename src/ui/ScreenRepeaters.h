// Saitama — ScreenRepeaters.h
// Copyright 2026 Saitama — MIT License
//
// Repeater directory.  Shows saved repeater nodes: name, key prefix,
// RSSI, last-heard time.  Tapping a row opens an action popup.

#pragma once
#include <lvgl.h>
#include <cstdint>

namespace ops { namespace ui {

class ScreenRepeaters {
public:
    static void show();

    // Called from UIScreen::tick() every frame.
    // Polls MeshService::pollLoginResult() and handles a 15-second timeout.
    static void tickLoginResult();

    // Called from UIScreen::tick() for every incoming contact response line.
    // Appends to the admin panel response area when the panel is open.
    static void onContactResponse(const char* line);

private:
    static lv_obj_t* _screen;

    // ── Repeater list ─────────────────────────────────────────────────
    static void _build();
    static void _onHomeClick     (lv_event_t* e);
    static void _onRowClick      (lv_event_t* e);

    // Popup action callbacks (user_data = overlay obj)
    static void _onPopupAdmin     (lv_event_t* e);
    static void _onPopupFavourite (lv_event_t* e);
    static void _onPopupSetPath   (lv_event_t* e);
    static void _onPopupResetPath (lv_event_t* e);
    static void _onPopupDelete    (lv_event_t* e);
    static void _onPopupClose     (lv_event_t* e);

    // Secondary dialogs
    static void _showAdminDialog  ();
    static void _showSetPathDialog();

    // Admin dialog callbacks (user_data = overlay obj)
    static void _onAdminOk      (lv_event_t* e);
    static void _onAdminCancel  (lv_event_t* e);
    static void _onRetryLogin   (lv_event_t* e);

    // Admin panel action button callbacks
    static void _onAdminClockSync(lv_event_t* e);
    static void _onAdminAdvert   (lv_event_t* e);
    static void _onAdminNbrs     (lv_event_t* e);

    // Set-path dialog callbacks (user_data = overlay obj)
    static void _onSetPathSave  (lv_event_t* e);
    static void _onSetPathCancel(lv_event_t* e);
    static void _onHashSz1Click (lv_event_t* e);
    static void _onHashSz2Click (lv_event_t* e);

    // ── Admin login flow ──────────────────────────────────────────────
    // Active target for the current admin session
    static uint8_t s_adminPrefix[4];
    static char    s_adminName[32];
    static char    s_adminPass[64];   // saved so Retry can re-use it

    // Login-await state (set in _onAdminOk, cleared in onLoginResult)
    static bool     s_awaitingLoginResult;
    static uint32_t s_loginSentMs;

    // Debounce for Status button (millis() of last send)
    static uint32_t s_lastStatusMs;

    // "Logging in…" waiting overlay (lives on _screen while awaiting)
    static lv_obj_t* s_waitOverlay;

    static void _showAdminWaiting();
    static void onLoginResult(bool ok);
    static void _onWaitCancel(lv_event_t* e);

    // ── Admin panel (new LVGL screen shown after successful login) ────
    static lv_obj_t* s_adminScreen;
    static lv_obj_t* s_adminRespLbl;
    static char      s_adminRespBuf[640];

    static void _showAdminPanel();
    static void _onAdminStatus(lv_event_t* e);
    static void _onAdminClose (lv_event_t* e);
    static void _onAdminKey   (lv_event_t* e);  // ESC/backspace on admin panel
};

}}  // namespace ops::ui
