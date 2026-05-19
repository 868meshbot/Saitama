// Saitama — ScreenContacts.h
// Copyright 2026 Saitama — MIT License
//
// Contacts directory.  Shows saved stations: name, 6-char address prefix,
// last-heard date/time.  Unread DMs are flagged with a red dot.
// Tapping a contact opens an action popup (DM / Set Path / Reset Path / Delete).

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenContacts {
public:
    static void show();

private:
    static lv_obj_t* _screen;

    static void _build();
    static void _onHomeClick    (lv_event_t* e);
    static void _onRowClick     (lv_event_t* e);  // user_data = contact index (int)

    // Popup action callbacks (user_data = overlay obj)
    static void _onPopupDM        (lv_event_t* e);
    static void _onPopupFavourite (lv_event_t* e);
    static void _onPopupShareQR   (lv_event_t* e);
    static void _onPopupSetPath   (lv_event_t* e);
    static void _onPopupResetPath (lv_event_t* e);
    static void _onPopupDelete    (lv_event_t* e);
    static void _onPopupClose     (lv_event_t* e);

    // Set-path dialog
    static void _showSetPathDialog();
    static void _onSetPathSave  (lv_event_t* e);
    static void _onSetPathCancel(lv_event_t* e);
    static void _onHashSz1Click (lv_event_t* e);
    static void _onHashSz2Click (lv_event_t* e);
};

}}  // namespace ops::ui
