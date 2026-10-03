// Saitama — ScopePicker.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "ScopePicker.h"
#include "Theme.h"
#include "../utils/Config.h"
#include "../utils/Regions.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace ops { namespace ui { namespace scopepick {

const char* nameError(const char* txt, char* out, size_t outMax)
{
    out[0] = '\0';
    if (!txt) txt = "";
    while (*txt == ' ' || *txt == '#') txt++;   // legacy '#' form
    size_t n = strlen(txt);
    while (n && txt[n - 1] == ' ') n--;
    if (n == 0 || (n == 1 && txt[0] == '*')) return nullptr;
    if (txt[0] == '$') return "Private ($) regions aren't supported.";
    if (n < 2)         return "Region names are 2+ characters.";
    if (n >= outMax || n >= ops::regions::NAME_LEN) return "Region name is too long.";
    for (size_t i = 0; i < n; i++) {
        char c = txt[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_')
            return "Use letters, digits, - or _ only.";
    }
    memcpy(out, txt, n);
    out[n] = '\0';
    return nullptr;
}

void fill(lv_obj_t* dd, const char* current)
{
    if (!dd) return;
    if (!current) current = "";
    const char* def = ops::config::get().scopeTag;
    char opts[(ops::regions::MAX_REGIONS + 2) * 20];
    snprintf(opts, sizeof(opts), "Default (%s)", def[0] ? def : "*");
    int sel = 0, n = 1;
    bool listed = !current[0];
    for (int i = 0; i < ops::regions::count(); i++) {
        const char* nm = ops::regions::get(i);
        strncat(opts, "\n", sizeof(opts) - strlen(opts) - 1);
        strncat(opts, nm,   sizeof(opts) - strlen(opts) - 1);
        if (current[0] && strcmp(current, nm) == 0) { sel = n; listed = true; }
        n++;
    }
    if (!listed) {   // keep a scope that isn't saved, so saving doesn't drop it
        strncat(opts, "\n",    sizeof(opts) - strlen(opts) - 1);
        strncat(opts, current, sizeof(opts) - strlen(opts) - 1);
        sel = n;
    }
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, (uint16_t)sel);
}

void selected(lv_obj_t* dd, char* out, size_t outMax)
{
    out[0] = '\0';
    if (!dd || lv_dropdown_get_selected(dd) == 0) return;
    lv_dropdown_get_selected_str(dd, out, (uint32_t)outMax);
}

static void _onAddClick(lv_event_t* e)
{
    openNewScope(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
}

lv_obj_t* createRow(lv_obj_t* parent, const char* current, bool inGroup, lv_obj_t** addBtnOut)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);

    lv_obj_t* dd = lv_dropdown_create(row);
    lv_obj_set_flex_grow(dd, 1);
    lv_obj_set_height(dd, 28);
    lv_obj_set_style_pad_ver(dd, 4, 0);
    lv_obj_set_style_bg_color(dd, theme::BG, 0);
    lv_obj_set_style_text_color(dd, theme::TEXT, 0);
    lv_obj_set_style_border_color(dd, theme::BORDER, 0);
    lv_obj_set_style_border_color(dd, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_10, 0);
    fill(dd, current);
    if (lv_obj_t* list = lv_dropdown_get_list(dd)) {
        lv_obj_set_style_text_font(list, &lv_font_montserrat_12, 0);
        lv_obj_set_style_bg_color(list, theme::BG_CARD, 0);
        lv_obj_set_style_text_color(list, theme::TEXT, 0);
    }

    lv_obj_t* add = lv_btn_create(row);
    lv_obj_set_size(add, 28, 26);
    lv_obj_set_style_bg_color(add, theme::BG, 0);
    lv_obj_set_style_bg_color(add, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(add, theme::BORDER, 0);
    lv_obj_set_style_border_color(add, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(add, 1, 0);
    lv_obj_set_style_radius(add, 4, 0);
    lv_obj_set_style_shadow_width(add, 0, 0);
    lv_obj_set_style_pad_all(add, 0, 0);
    lv_obj_add_event_cb(add, _onAddClick, LV_EVENT_CLICKED, dd);
    lv_obj_t* lbl = lv_label_create(add);
    lv_label_set_text(lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(lbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        if (inGroup) {
            lv_group_add_obj(g, dd);
            lv_group_add_obj(g, add);
        } else {
            lv_group_remove_obj(dd);
            lv_group_remove_obj(add);
        }
    }
    if (addBtnOut) *addBtnOut = add;
    return dd;
}

// ── New Scope box ─────────────────────────────────────────────────────

struct NewScopeCtx { lv_obj_t* modal; lv_obj_t* ta; lv_obj_t* status; lv_obj_t* dd; };
static NewScopeCtx s_ctx;

// Called from the box's own buttons/keys, so the delete is deferred.
static void _close()
{
    lv_obj_t* dd = s_ctx.dd;
    if (s_ctx.modal) lv_obj_del_async(s_ctx.modal);
    s_ctx = NewScopeCtx{};
    lv_group_t* g = lv_group_get_default();
    if (g && dd && lv_obj_get_group(dd) == g) lv_group_focus_obj(dd);
}

static void _onSave(lv_event_t* /*e*/)
{
    if (!s_ctx.modal) return;
    char name[ops::regions::NAME_LEN];
    const char* err = nameError(s_ctx.ta ? lv_textarea_get_text(s_ctx.ta) : "", name, sizeof(name));
    if (!err && !name[0]) err = "Enter a region name.";
    if (!err) {
        bool known = false;
        for (int i = 0; i < ops::regions::count(); i++)
            if (strcmp(ops::regions::get(i), name) == 0) known = true;
        if (!known && ops::regions::count() >= ops::regions::MAX_REGIONS)
            err = "Saved scopes are full (9). Remove one in Settings > Region Scope.";
        else if (!known)
            ops::regions::add(name);
    }
    if (err) {
        lv_label_set_text(s_ctx.status, err);
        lv_obj_set_style_text_color(s_ctx.status, theme::RED, 0);
        return;
    }
    if (s_ctx.dd) fill(s_ctx.dd, name);
    _close();
}

static void _onExit(lv_event_t* /*e*/) { _close(); }
static void _onKey(lv_event_t* e) { if (lv_event_get_key(e) == LV_KEY_ESC) _close(); }

void openNewScope(lv_obj_t* dd)
{
    if (s_ctx.modal) return;
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_ctx = NewScopeCtx{};
    s_ctx.modal = modal;
    s_ctx.dd    = dd;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);   // absorb taps meant for the dialog below

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::ACCENT, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 5, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "New Scope");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    s_ctx.ta = lv_textarea_create(panel);
    lv_obj_set_size(s_ctx.ta, 220, 28);
    lv_textarea_set_one_line(s_ctx.ta, true);
    lv_textarea_set_max_length(s_ctx.ta, ops::regions::NAME_LEN - 1);
    lv_textarea_set_placeholder_text(s_ctx.ta, "e.g. AU");
    lv_obj_set_style_bg_color(s_ctx.ta, theme::BG, 0);
    lv_obj_set_style_text_color(s_ctx.ta, theme::TEXT, 0);
    lv_obj_set_style_border_color(s_ctx.ta, theme::BORDER, 0);
    lv_obj_set_style_border_width(s_ctx.ta, 1, 0);
    lv_obj_set_style_text_font(s_ctx.ta, &lv_font_montserrat_12, 0);
    lv_obj_add_event_cb(s_ctx.ta, _onKey,  LV_EVENT_KEY,   nullptr);
    lv_obj_add_event_cb(s_ctx.ta, _onSave, LV_EVENT_READY, nullptr);   // Enter adds

    s_ctx.status = lv_label_create(panel);
    lv_obj_set_width(s_ctx.status, 220);
    lv_label_set_long_mode(s_ctx.status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ctx.status, "Added to the saved scopes.");
    lv_obj_set_style_text_color(s_ctx.status, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_ctx.status, &lv_font_montserrat_10, 0);

    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, 220, 30);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* btns[2];
    const char*   txt[2] = { LV_SYMBOL_OK " Add", LV_SYMBOL_CLOSE " Cancel" };
    lv_event_cb_t cbs[2] = { _onSave, _onExit };
    for (int i = 0; i < 2; i++) {
        lv_obj_t* b = lv_btn_create(row);
        btns[i] = b;
        lv_obj_set_size(b, 96, 26);
        lv_obj_set_style_bg_color(b, i == 0 ? theme::ACCENT : theme::BG, 0);
        lv_obj_set_style_bg_color(b, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b, theme::BORDER, 0);
        lv_obj_set_style_border_width(b, i == 0 ? 0 : 1, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, cbs[i], LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(b, _onKey, LV_EVENT_KEY,     nullptr);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt[i]);
        lv_obj_set_style_text_color(l, i == 0 ? theme::BG : theme::TEXT, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_center(l);
    }

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, s_ctx.ta);
        lv_group_add_obj(g, btns[0]);
        lv_group_add_obj(g, btns[1]);
        lv_group_focus_obj(s_ctx.ta);
    }
}

}}}  // namespace ops::ui::scopepick
