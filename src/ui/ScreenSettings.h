// Saitama — ScreenSettings.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenSettings {
public:
    static void show();

private:
    static lv_obj_t* _screen;
    static lv_obj_t* _list;

    static void _buildTopBar(lv_obj_t* parent);
    static void _buildList  (lv_obj_t* parent);
    static void _refreshList();  // redraws all list items from Config

    // Event handlers
public:
    static void _onItemClick    (lv_event_t* e);  // user_data = item index (needs public for file-scope helper)
private:
    static void _onHomeClick    (lv_event_t* e);
};

}}  // namespace ops::ui
