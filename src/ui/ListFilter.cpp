// Saitama — ListFilter.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ListFilter.h"
#include "Theme.h"
#include <cctype>
#include <cstdint>
#include <cstring>

namespace ops { namespace ui { namespace listfilter {

// Row key is stored as key + 1 so that 0 (no user data) means "not a row".
void setRowKey(lv_obj_t* row, int key)
{
    lv_obj_set_user_data(row, (void*)(intptr_t)(key + 1));
}

static bool _containsNoCase(const char* hay, const char* needle)
{
    if (!needle[0]) return true;
    size_t n = strlen(needle);
    for (const char* h = hay; *h; h++) {
        size_t i = 0;
        while (i < n && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

void apply(State& st)
{
    if (!st.list || !st.rowText) return;
    uint32_t n = lv_obj_get_child_cnt(st.list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* row = lv_obj_get_child(st.list, (int32_t)i);
        intptr_t k = (intptr_t)lv_obj_get_user_data(row);
        if (k <= 0) continue;
        char txt[64];
        st.rowText((int)(k - 1), txt, sizeof(txt));
        if (_containsNoCase(txt, st.text)) lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
        else                               lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_scroll_to_y(st.list, 0, LV_ANIM_OFF);
}

static void _onChanged(lv_event_t* e)
{
    State* st = static_cast<State*>(lv_event_get_user_data(e));
    const char* t = lv_textarea_get_text(st->ta);
    strncpy(st->text, t ? t : "", sizeof(st->text) - 1);
    st->text[sizeof(st->text) - 1] = '\0';
    apply(*st);
}

static void _onKey(lv_event_t* e)
{
    State* st = static_cast<State*>(lv_event_get_user_data(e));
    if (lv_event_get_key(e) == LV_KEY_ESC && st->onEsc) st->onEsc();
}

lv_obj_t* create(lv_obj_t* bar, State& st)
{
    // Pushes the box to the right; it stops short of the right edge (clear of
    // the panel's dead pixels, like the Chat list's + button).
    lv_obj_t* spacer = lv_obj_create(bar);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* ta = lv_textarea_create(bar);
    st.ta = ta;
    lv_obj_set_size(ta, 110, 22);
    lv_obj_set_style_translate_x(ta, -41, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, sizeof(st.text) - 1);
    lv_textarea_set_placeholder_text(ta, LV_SYMBOL_EYE_OPEN " filter");
    lv_obj_set_style_bg_color(ta, theme::BG, 0);
    lv_obj_set_style_text_color(ta, theme::TEXT, 0);
    lv_obj_set_style_border_color(ta, theme::BORDER, 0);
    lv_obj_set_style_border_color(ta, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 4, 0);
    lv_obj_set_style_pad_hor(ta, 4, 0);
    lv_obj_set_style_pad_ver(ta, 3, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(ta, theme::TEXT_MUTED, LV_PART_TEXTAREA_PLACEHOLDER);
    if (st.text[0]) lv_textarea_set_text(ta, st.text);
    lv_obj_add_event_cb(ta, _onChanged, LV_EVENT_VALUE_CHANGED, &st);
    lv_obj_add_event_cb(ta, _onKey,     LV_EVENT_KEY,           &st);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, ta);
        lv_group_focus_obj(ta);
    }
    return ta;
}

}}}  // namespace ops::ui::listfilter
