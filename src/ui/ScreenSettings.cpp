// Saitama — ScreenSettings.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Settings screen layout (320 x 240):
//
//   ┌──────────────────────────────────────┐  y = 0
//   │ [⌂ Home]    Settings          12:34 │  top bar  28 px
//   ├──────────────────────────────────────┤  y = 28
//   │ Device Name          OMS-0001  >    │  \
//   │ Channel 1            Public    >    │   |
//   │ Channel 2            CH2       >    │   | scrollable
//   │ Channel 3            Disabled  >    │   |  list
//   │ Channel 4            Disabled  >    │   |
//   │ Channel 5            Disabled  >    │   |
//   │ Radio                EU868     >    │   |
//   │ Bluetooth            OFF       >    │   |
//   │ Speaker              ON        >    │   |
//   │ GPS                  ON        >    │   |
//   │ Date / Time          --:--     >    │   |
//   │ Firmware Update      v0.1      >    │  /
//   └──────────────────────────────────────┘

#include "ScreenSettings.h"
#include "UIScreen.h"
#include "ScreenLauncher.h"
#include "QRPopup.h"
#include "Theme.h"
#include "Emoji.h"
#include "../utils/Config.h"
#include "../utils/Lang.h"
#include "../utils/Contacts.h"
#include "../utils/Repeaters.h"
#include "../utils/Keymap.h"
#include "../utils/Log.h"
#include "../utils/Sound.h"
#include "../utils/SDCard.h"
#include <SD.h>
#include "../mesh/MeshService.h"
#include "../hardware/Board.h"
#include "../utils/GpsMgr.h"
#include "../version.h"
#include <cstring>
#include <cstdio>
#include <time.h>
#include <esp_ota_ops.h>
#include <Preferences.h>
#include <LittleFS.h>

// Extended-Latin body font (global symbol, defined in font_montserrat_12_ext.c).
extern const lv_font_t font_montserrat_12_ext;

namespace ops { namespace ui {

static constexpr int TOP_H = 28;

lv_obj_t* ScreenSettings::_screen = nullptr;
lv_obj_t* ScreenSettings::_list   = nullptr;

static lv_obj_t* s_listPtr = nullptr;  // mirrors _list for free-function access

// ── show() ───────────────────────────────────────────────────────────
void ScreenSettings::show() {
    if (!_screen) {
        _screen = lv_obj_create(nullptr);
        lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
        lv_obj_set_style_bg_color(_screen, theme::BG, 0);
        lv_obj_set_style_pad_all(_screen, 0, 0);
        lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

        _buildTopBar(_screen);
        _buildList(_screen);
    } else {
        _refreshList();
    }

    lv_scr_load(_screen);
    OPS_LOG("UI", "Settings shown");
}

// ── _buildTopBar() ───────────────────────────────────────────────────
void ScreenSettings::_buildTopBar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, OPS_SCREEN_W, TOP_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_set_style_pad_ver(bar, 2, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* homeBtn = lv_btn_create(bar);
    lv_obj_set_size(homeBtn, 56, 22);
    lv_obj_align(homeBtn, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(homeBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(homeBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(homeBtn, 1, 0);
    lv_obj_set_style_border_color(homeBtn, theme::BORDER, 0);
    lv_obj_set_style_radius(homeBtn, 4, 0);
    lv_obj_set_style_shadow_width(homeBtn, 0, 0);
    lv_obj_add_event_cb(homeBtn, _onHomeClick, LV_EVENT_CLICKED, nullptr);
    lv_group_remove_obj(homeBtn);  // top bar is not encoder-navigable

    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME " Home");
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS " Settings");
    lv_obj_set_style_text_color(title, theme::TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
}

// ── helpers ──────────────────────────────────────────────────────────

// Backspace/ESC → go back to launcher.
// Digit keys 1-9 / 0 → trigger the Nth visible settings row (phone layout).
static void _onRowKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_BACKSPACE || key == LV_KEY_ESC) {
        ScreenLauncher::show();
        return;
    }
    int pos = -1;
    if (key >= '1' && key <= '9') pos = (int)(key - '1');
    else if (key == '0')          pos = 9;
    if (pos >= 0 && s_listPtr) {
        lv_obj_t* row = lv_obj_get_child(s_listPtr, pos);
        if (row) lv_event_send(row, LV_EVENT_CLICKED, nullptr);
    }
}

// Add one row.  Uses lv_btn_create (not lv_list_add_btn) so we own the
// layout: name label left-aligned in a fixed 160 px column, value label
// in the remaining width with centered text.
static lv_obj_t* _addRow(lv_obj_t* list, const char* label,
                          const char* value, int idx) {
    lv_obj_t* btn = lv_btn_create(list);
    lv_obj_set_size(btn, OPS_SCREEN_W, 22);
    lv_obj_set_style_bg_color(btn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, theme::BG_CARD, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(btn, theme::ACCENT,  LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 8, 0);
    lv_obj_set_style_pad_ver(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Name: fixed 160 px, text left-aligned
    lv_obj_t* nameLbl = lv_label_create(btn);
    lv_label_set_text(nameLbl, label);
    lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(nameLbl, 160);
    lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_CLIP);

    // Value: fills remaining width, text centered
    lv_obj_t* valLbl = lv_label_create(btn);
    lv_label_set_text(valLbl, value);
    lv_obj_set_style_text_color(valLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(valLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_flex_grow(valLbl, 1);
    lv_obj_set_style_text_align(valLbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_add_event_cb(btn, ScreenSettings::_onItemClick,
                        LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(btn, _onRowKey, LV_EVENT_KEY, nullptr);
    return btn;
}

// Forward declarations — defined with their respective dialogs below.
static void _fmtTimeoutVal(char* buf, size_t len, int sec);
static void _fmtScreenOffVal(char* buf, size_t len, int sec);

// True when any app partition other than the one we're running from exists.
// The Launcher lives in its own slot (factory/test/ota); as long as there is
// at least one other app partition we can jump back.
static bool _hasLauncher()
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    bool found = false;
    while (it) {
        const esp_partition_t* p = esp_partition_get(it);
        if (p && (!running || p->address != running->address)) {
            found = true;
            break;
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    return found;
}

// ── _buildList() ─────────────────────────────────────────────────────
void ScreenSettings::_buildList(lv_obj_t* parent) {
    // Plain flex-column scroll container — we don't use lv_list_create
    // because lv_list_add_btn injects its own internal label with flex_grow=1
    // that fights our two-label layout.
    _list = lv_obj_create(parent);
    lv_obj_set_size(_list, OPS_SCREEN_W, OPS_SCREEN_H - TOP_H);
    lv_obj_align(_list, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(_list, theme::BG, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_radius(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 0, 0);
    lv_obj_set_style_pad_row(_list, 1, 0);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_list,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_ACTIVE);

    const auto& cfg = ops::config::get();

    const char* on  = lang::tr(lang::TR_ON);
    const char* off = lang::tr(lang::TR_OFF);

    // Item indices match _onItemClick switch
    _addRow(_list, lang::tr(lang::TR_DEVICE_NAME),    cfg.callsign, 0);
    _addRow(_list, lang::tr(lang::TR_SHARE_CONTACT),  "",           24);
    _addRow(_list, lang::tr(lang::TR_GEN_IDENTITY),   "",           35);
    _addRow(_list, lang::tr(lang::TR_CHANNELS),       "",           1);
    static const char* kRadioShort[] = {
        "AU", "AU-Vic", "EU NAR", "EU LON", "EU MED",
        "CZ NAR", "EU 433", "NZ", "NZ NAR", "PT 433",
        "PT 868", "CH", "US/CA", "VN"
    };
    const char* radioLabel = cfg.radioCustom
        ? "Custom"
        : kRadioShort[cfg.radioProfile < 14 ? cfg.radioProfile : 2];
    _addRow(_list, lang::tr(lang::TR_RADIO), radioLabel, 6);
    char pwrBuf[10];
    if (cfg.radioTX >= 10 && cfg.radioTX <= 22)
        snprintf(pwrBuf, sizeof(pwrBuf), "%d dBm", cfg.radioTX);
    else
        snprintf(pwrBuf, sizeof(pwrBuf), "%s", off);
    _addRow(_list, lang::tr(lang::TR_POWER), pwrBuf, 25);
    _addRow(_list, lang::tr(lang::TR_LORA_DUTY), cfg.loraDutyCycle ? on : off, 28);
    static const char* kGovNames[] = { "Power Save", "Medium", "Normal", "Turbo" };
    _addRow(_list, lang::tr(lang::TR_CPU_GOV),
            kGovNames[cfg.cpuGovernor < 4 ? cfg.cpuGovernor : 2], 29);
    char brightBuf[8];
    snprintf(brightBuf, sizeof(brightBuf), "%d%%", cfg.brightness * 100 / 255);
    _addRow(_list, lang::tr(lang::TR_BRIGHTNESS), brightBuf, 26);
    static const char* kThemeNames[] = {
        "Default", "Green", "Dracula", "Tokyo Night",
        "Catp Frappe", "Catp Mocha", "Synthwave 84",
        "Kaolin", "One Dark", "Neovim", "Nyx", "Rat Dark"
    };
    int themeIdx = (cfg.theme >= 0 && cfg.theme < theme::THEME_COUNT) ? cfg.theme : 0;
    _addRow(_list, lang::tr(lang::TR_THEME), kThemeNames[themeIdx], 31);
    _addRow(_list, lang::tr(lang::TR_FONT),
            cfg.fontExtLatin ? "Extended Latin" : "Standard", 33);
    _addRow(_list, lang::tr(lang::TR_BLUETOOTH), cfg.bluetoothEnabled ? on : off, 7);
    _addRow(_list, lang::tr(lang::TR_SPEAKER),   cfg.speakerEnabled   ? on : off, 8);
    static const char* gpsModeNames[] = { "Off", "Intermittent", "On" };
    _addRow(_list, lang::tr(lang::TR_GPS),
            gpsModeNames[cfg.gpsMode < 3 ? cfg.gpsMode : 2], 9);
    char kbBuf[8];
    if (cfg.kbBrightness == 0) snprintf(kbBuf, sizeof(kbBuf), "%s", off);
    else snprintf(kbBuf, sizeof(kbBuf), "%d", cfg.kbBrightness);
    _addRow(_list, lang::tr(lang::TR_KB_LIGHT), kbBuf, 21);
    static const char* kLayoutShort[] = { "English", "FR AZERTY", "DE QWERTZ" };
    _addRow(_list, lang::tr(lang::TR_KB_LAYOUT),
            kLayoutShort[cfg.kbLayout < ops::keymap::LAYOUT_COUNT ? cfg.kbLayout : 0], 23);

    // Date/Time row — display in local time
    time_t now = ops::config::localEpoch();
    char dtBuf[16] = "--:--";
    if (now > 1700000000UL) {
        struct tm t;
        gmtime_r(&now, &t);
        snprintf(dtBuf, sizeof(dtBuf), "%04d-%02d-%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    }
    _addRow(_list, lang::tr(lang::TR_DATE_TIME), dtBuf, 10);

    // Timezone row
    char tzBuf[8] = "UTC";
    int tzOff = cfg.timezoneOffsetHours;
    if (tzOff > 0)       snprintf(tzBuf, sizeof(tzBuf), "UTC+%d", tzOff);
    else if (tzOff < 0)  snprintf(tzBuf, sizeof(tzBuf), "UTC%d",  tzOff);
    _addRow(_list, lang::tr(lang::TR_TIMEZONE), tzBuf, 20);
    _addRow(_list, lang::tr(lang::TR_FW_UPDATE),    "v" OPS_VERSION_STRING, 11);
    _addRow(_list, lang::tr(lang::TR_AUTO_ADD),     "", 12);
    char toStr[12];
    _fmtTimeoutVal(toStr, sizeof(toStr), cfg.screenTimeoutSec);
    _addRow(_list, lang::tr(lang::TR_SCR_TIMEOUT),  toStr, 13);
    {
        char soStr[12];
        _fmtScreenOffVal(soStr, sizeof(soStr), (int)cfg.screenOffSec);
        _addRow(_list, lang::tr(lang::TR_SCR_OFF), soStr, 27);
    }
    {
        char volStr[8];
        snprintf(volStr, sizeof(volStr), "%d%%", (int)cfg.speakerVolume);
        _addRow(_list, lang::tr(lang::TR_VOLUME), volStr, 32);
    }
    _addRow(_list, lang::tr(lang::TR_NOTIFICATIONS), "", 16);
    static const char* kSndNames[] = { "Default", "Pluck", "Clear", "Whoosh" };
    _addRow(_list, lang::tr(lang::TR_NOTIFY_SOUND),
            kSndNames[cfg.notifySoundChoice < 4 ? cfg.notifySoundChoice : 0], 22);
    _addRow(_list, lang::tr(lang::TR_SAVE_MSGS),   cfg.saveMsgs        ? on : off, 14);
    _addRow(_list, lang::tr(lang::TR_SHOW_HOPS),   cfg.showHops        ? on : off, 15);
    _addRow(_list, lang::tr(lang::TR_SHOW_RSSI),   cfg.showRssi        ? on : off, 19);
    _addRow(_list, lang::tr(lang::TR_LOCATION),    cfg.locationSharing ? on : off, 17);
    _addRow(_list, lang::tr(lang::TR_BACKUP),      "", 30);
    {
        uint8_t li = cfg.uiLanguage < lang::LANG_COUNT ? cfg.uiLanguage : 0;
        _addRow(_list, lang::tr(lang::TR_LANGUAGE), lang::kLangNames[li], 36);
    }
    if (_hasLauncher())
        _addRow(_list, lang::tr(lang::TR_RETURN), "", 34);

    s_listPtr = _list;
}

// ── _refreshList() ───────────────────────────────────────────────────
void ScreenSettings::_refreshList() {
    if (!_list) return;
    lv_coord_t savedY = lv_obj_get_scroll_y(_list);

    // Save focused row index so we can restore it after rebuild
    int focusIdx = 0;
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_obj_t* foc = lv_group_get_focused(g);
        if (foc) {
            uint32_t n = lv_obj_get_child_cnt(_list);
            for (uint32_t i = 0; i < n; i++) {
                if (lv_obj_get_child(_list, (int32_t)i) == foc) {
                    focusIdx = (int)i;
                    break;
                }
            }
        }
    }

    lv_obj_del(_list);
    _list = nullptr;
    _buildList(_screen);
    lv_obj_scroll_to_y(_list, savedY, LV_ANIM_OFF);

    if (g) {
        lv_obj_t* toFocus = lv_obj_get_child(_list, focusIdx);
        if (toFocus) lv_group_focus_obj(toFocus);
    }
}

// ── _openNameDialog() ────────────────────────────────────────────────
static void _closeModal(lv_obj_t* modal) {
    lv_obj_del(modal);
}

static void _onModalSave(lv_event_t* e) {
    lv_obj_t* modal = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    // Find the textarea (second child of the content panel = child 1)
    lv_obj_t* content = lv_obj_get_child(modal, 0);
    lv_obj_t* ta = lv_obj_get_child(content, 1);  // title(0), textarea(1), btnrow(2)
    const char* txt = lv_textarea_get_text(ta);
    if (txt && txt[0] != '\0') {
        ops::config::setCallsign(txt);
        ops::MeshService::instance().setCallsign(txt);
        OPS_LOG("Settings", "Device name saved: %s", txt);
    }
    _closeModal(modal);
    ScreenSettings::show();   // refresh list
}

static void _onModalExit(lv_event_t* e) {
    lv_obj_t* modal = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    _closeModal(modal);
}

static void _onModalKey(lv_event_t* e) {
    if (lv_event_get_key(e) == LV_KEY_ESC) {
        lv_obj_t* modal = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
        _closeModal(modal);
    }
}

static void _openNameDialog() {
    // Semi-transparent overlay that blocks the list
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onModalKey, LV_EVENT_KEY, modal);

    // Centred dialog panel
    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 120);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title
    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Username");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    // Textarea pre-filled with current callsign
    lv_obj_t* ta = lv_textarea_create(panel);
    lv_obj_set_size(ta, 220, 30);
    lv_obj_set_style_bg_color(ta, theme::BG, 0);
    lv_obj_set_style_text_color(ta, theme::TEXT, 0);
    lv_obj_set_style_border_color(ta, theme::ACCENT, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_text_font(ta, theme::bodyFont10(), 0);
    lv_textarea_set_max_length(ta, 15);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, ops::config::get().callsign);

    // Button row
    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, 220, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Save button
    lv_obj_t* saveBtn = lv_btn_create(row);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onModalSave, LV_EVENT_CLICKED, modal);

    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    // Exit button
    lv_obj_t* exitBtn = lv_btn_create(row);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onModalExit, LV_EVENT_CLICKED, modal);

    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    // Add interactive elements to the LVGL group so encoder + keyboard reach them.
    // ESC on any focused element closes the modal.
    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, ta);
        lv_group_add_obj(g, saveBtn);
        lv_group_add_obj(g, exitBtn);
        lv_group_focus_obj(ta);
    }
    lv_obj_add_event_cb(ta,      _onModalSave, LV_EVENT_READY, modal);  // Enter = save
    lv_obj_add_event_cb(ta,      _onModalKey,  LV_EVENT_KEY,   modal);
    lv_obj_add_event_cb(saveBtn, _onModalKey,  LV_EVENT_KEY,   modal);
    lv_obj_add_event_cb(exitBtn, _onModalKey,  LV_EVENT_KEY,   modal);
}

// ── Channel sub-dialogs ───────────────────────────────────────────────
// Forward-declared so _openChannelsDialog can reference _openEditChannelDialog.
static void _openEditChannelDialog(int ch);

// Static context for the edit dialog — avoids heap allocation and pointer juggling.
struct EditChCtx {
    lv_obj_t* modal;
    int        ch;
    lv_obj_t* nameTa;
    lv_obj_t* pskTa;
    lv_obj_t* scopeTa;
};
static EditChCtx s_editCtx;

static void _closeEditCh(bool doSave) {
    if (doSave) {
        const char* rawName = lv_textarea_get_text(s_editCtx.nameTa);
        char cleanName[32] = {};
        if (rawName) {
            const char* src = (rawName[0] == '#') ? rawName + 1 : rawName;
            strncpy(cleanName, src, sizeof(cleanName) - 1);
        }
        const char* rawPsk = lv_textarea_get_text(s_editCtx.pskTa);
        char pskNorm[25] = {};
        ops::MeshService::normalizePsk(rawPsk ? rawPsk : "", pskNorm, sizeof(pskNorm));
        const char* scope = s_editCtx.scopeTa
                            ? lv_textarea_get_text(s_editCtx.scopeTa) : "";
        // Preserve existing shortname — field removed from UI
        const char* sn = ops::config::get().channels[s_editCtx.ch].shortname;
        ops::config::setChannel(s_editCtx.ch, cleanName,
                                pskNorm, sn ? sn : "",
                                scope ? scope : "");
        ops::MeshService::instance().syncChannel(s_editCtx.ch);
        OPS_LOG("Settings", "Channel %d saved: name=%s scope=%s",
                s_editCtx.ch + 1, cleanName, scope ? scope : "");
    }
    lv_obj_del(s_editCtx.modal);
    s_editCtx.modal = nullptr;
    ScreenSettings::show();
}

static void _onEditChSave(lv_event_t* /*e*/) { _closeEditCh(true);  }
static void _onEditChExit(lv_event_t* /*e*/) { _closeEditCh(false); }

static void _onEditChKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC)   _closeEditCh(false);
}

// Which 5-channel page is showing in the channel list dialog: 0=CH1-5, 1=CH6-10
static int       s_chPage  = 0;
static lv_obj_t* s_chModal = nullptr;

static void _openChannelsDialog();  // forward declaration — defined after _onChPageChange

static void _onChannelListClose(lv_event_t* /*e*/) {
    lv_obj_del(s_chModal);
    s_chModal = nullptr;
}

static void _onChannelRowClick(lv_event_t* e) {
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_del(s_chModal);
    s_chModal = nullptr;
    _openEditChannelDialog(ch);
}

static void _onChPageChange(lv_event_t* e) {
    s_chPage = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_del(s_chModal);
    s_chModal = nullptr;
    _openChannelsDialog();
}

static void _openChannelsDialog() {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_chModal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 300, 210);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 3, 0);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title shows which page is active: "Channels 1-5" or "Channels 6-10"
    char titleBuf[20];
    snprintf(titleBuf, sizeof(titleBuf), "Channels %d-%d",
             s_chPage * 5 + 1, s_chPage * 5 + 5);
    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, titleBuf);
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_bottom(title, 2, 0);

    const auto& cfg = ops::config::get();
    int base = s_chPage * 5;
    for (int i = base; i < base + 5; i++) {
        lv_obj_t* row = lv_btn_create(panel);
        lv_obj_set_size(row, 280, 24);
        lv_obj_set_style_bg_color(row, theme::BG, 0);
        lv_obj_set_style_bg_color(row, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, theme::BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
            LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        char rowBuf[40];
        if (cfg.channels[i].name[0])
            snprintf(rowBuf, sizeof(rowBuf), "CH%d: %s", i + 1, cfg.channels[i].name);
        else
            snprintf(rowBuf, sizeof(rowBuf), "CH%d: [disabled]", i + 1);
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, rowBuf);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, 240);
        lv_obj_set_style_text_color(lbl,
            cfg.channels[i].name[0] ? theme::TEXT : theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);

        lv_obj_t* arr = lv_label_create(row);
        lv_label_set_text(arr, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(arr, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(arr, &lv_font_montserrat_10, 0);

        lv_obj_add_event_cb(row, _onChannelRowClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    // Navigation row: [◄ 1-5]  [✕ Exit]  [6-10 ►]
    lv_obj_t* navRow = lv_obj_create(panel);
    lv_obj_set_size(navRow, 280, 30);
    lv_obj_set_style_bg_opa(navRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(navRow, 0, 0);
    lv_obj_set_style_pad_all(navRow, 0, 0);
    lv_obj_clear_flag(navRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(navRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(navRow,
        LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // [◄ 1-5] — highlighted when on page 0 (current), muted when on page 1
    lv_obj_t* prevBtn = lv_btn_create(navRow);
    lv_obj_set_size(prevBtn, 72, 26);
    lv_obj_set_style_bg_color(prevBtn, s_chPage == 0 ? theme::PRIMARY : theme::BG, 0);
    lv_obj_set_style_bg_color(prevBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(prevBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(prevBtn, 1, 0);
    lv_obj_set_style_radius(prevBtn, 4, 0);
    lv_obj_set_style_shadow_width(prevBtn, 0, 0);
    lv_obj_add_event_cb(prevBtn, _onChPageChange, LV_EVENT_CLICKED, (void*)(intptr_t)0);
    lv_obj_add_event_cb(prevBtn, _onModalKey, LV_EVENT_KEY, modal);
    lv_obj_t* prevLbl = lv_label_create(prevBtn);
    lv_label_set_text(prevLbl, LV_SYMBOL_LEFT " 1-5");
    lv_obj_set_style_text_color(prevLbl,
        s_chPage == 0 ? theme::TEXT : theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(prevLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(prevLbl);

    // [✕ Exit]
    lv_obj_t* exitBtn = lv_btn_create(navRow);
    lv_obj_set_size(exitBtn, 72, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onChannelListClose, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onModalKey, LV_EVENT_KEY, modal);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    // [6-10 ►] — highlighted when on page 1 (current), muted when on page 0
    lv_obj_t* nextBtn = lv_btn_create(navRow);
    lv_obj_set_size(nextBtn, 72, 26);
    lv_obj_set_style_bg_color(nextBtn, s_chPage == 1 ? theme::PRIMARY : theme::BG, 0);
    lv_obj_set_style_bg_color(nextBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(nextBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(nextBtn, 1, 0);
    lv_obj_set_style_radius(nextBtn, 4, 0);
    lv_obj_set_style_shadow_width(nextBtn, 0, 0);
    lv_obj_add_event_cb(nextBtn, _onChPageChange, LV_EVENT_CLICKED, (void*)(intptr_t)1);
    lv_obj_add_event_cb(nextBtn, _onModalKey, LV_EVENT_KEY, modal);
    lv_obj_t* nextLbl = lv_label_create(nextBtn);
    lv_label_set_text(nextLbl, "6-10 " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(nextLbl,
        s_chPage == 1 ? theme::TEXT : theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(nextLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(nextLbl);

    lv_group_t* gCh = lv_group_get_default();
    if (gCh) {
        // Add the 5 channel rows (children 1-5) then navRow's 3 buttons
        for (int ci = 1; ci <= 5; ci++) {
            lv_obj_t* ch = lv_obj_get_child(panel, ci);
            lv_group_add_obj(gCh, ch);
            lv_obj_add_event_cb(ch, _onModalKey, LV_EVENT_KEY, modal);
        }
        lv_group_add_obj(gCh, prevBtn);
        lv_group_add_obj(gCh, exitBtn);
        lv_group_add_obj(gCh, nextBtn);
        lv_group_focus_obj(lv_obj_get_child(panel, 1));
    }
}

// ── Helper to build a labeled textarea row ────────────────────────────
static lv_obj_t* _makeChField(lv_obj_t* parent, const char* labelText,
                               const char* initial, int maxLen, const char* placeholder)
{
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, labelText);
    lv_obj_set_style_text_color(lbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(lbl, LV_PCT(100));

    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, LV_PCT(100), 26);
    lv_obj_set_style_bg_color(ta, theme::BG, 0);
    lv_obj_set_style_text_color(ta, theme::TEXT, 0);
    lv_obj_set_style_border_color(ta, theme::BORDER, 0);
    lv_obj_set_style_border_color(ta, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_text_font(ta, theme::bodyFont10(), 0);
    lv_textarea_set_one_line(ta, true);
    if (maxLen > 0) lv_textarea_set_max_length(ta, maxLen);
    if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, initial ? initial : "");
    return ta;
}

// ── Random PSK generator ──────────────────────────────────────────────
static void _genRandomPsk(char* out, int outSize)
{
    if (outSize < 25) return;
    uint8_t rnd[16];
    for (int i = 0; i < 4; i++) {
        uint32_t r = (uint32_t)random();
        rnd[i*4+0] = (uint8_t)(r >>  0);
        rnd[i*4+1] = (uint8_t)(r >>  8);
        rnd[i*4+2] = (uint8_t)(r >> 16);
        rnd[i*4+3] = (uint8_t)(r >> 24);
    }
    static const char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int b = 0, j = 0; b < 15; b += 3, j += 4) {
        uint32_t v = ((uint32_t)rnd[b] << 16) | ((uint32_t)rnd[b+1] << 8) | rnd[b+2];
        out[j+0] = kB64[(v >> 18) & 63];
        out[j+1] = kB64[(v >> 12) & 63];
        out[j+2] = kB64[(v >>  6) & 63];
        out[j+3] = kB64[v & 63];
    }
    uint32_t v = (uint32_t)rnd[15] << 16;
    out[20] = kB64[(v >> 18) & 63];
    out[21] = kB64[(v >> 12) & 63];
    out[22] = '='; out[23] = '='; out[24] = '\0';
}

static void _onPskShuffle(lv_event_t* /*e*/)
{
    char psk[25];
    _genRandomPsk(psk, sizeof(psk));
    lv_textarea_set_text(s_editCtx.pskTa, psk);
}

static int _b64CharVal(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 0;
}

static void _urlEncodeSettings(const char* src, char* dst, int dstMax)
{
    int j = 0;
    for (int i = 0; src[i] && j < dstMax - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else if (c == ' ') {
            dst[j++] = '+';
        } else if (j < dstMax - 3) {
            snprintf(dst + j, 4, "%%%02X", c);
            j += 3;
        }
    }
    dst[j] = '\0';
}

static void _onEditChShareQR(lv_event_t* /*e*/)
{
    if (!s_editCtx.nameTa || !s_editCtx.pskTa) return;
    const char* rawName = lv_textarea_get_text(s_editCtx.nameTa);
    char cleanName[32] = {};
    if (rawName) {
        const char* src = (rawName[0] == '#') ? rawName + 1 : rawName;
        strncpy(cleanName, src, sizeof(cleanName) - 1);
    }
    if (!cleanName[0]) return;
    const char* rawPsk = lv_textarea_get_text(s_editCtx.pskTa);

    // Decode base64 PSK (24 chars → 16 bytes) then re-encode as 32 lowercase hex
    char secretHex[33] = {};
    if (rawPsk && rawPsk[0]) {
        uint8_t keyBytes[16] = {};
        int j = 0;
        const char* p = rawPsk;
        while (*p && *(p+1) && j < 16) {
            int a = _b64CharVal(p[0]), b = _b64CharVal(p[1]);
            int cv = (*(p+2) && *(p+2) != '=') ? _b64CharVal(p[2]) : -1;
            int d  = (*(p+3) && *(p+3) != '=') ? _b64CharVal(p[3]) : -1;
            keyBytes[j++] = (uint8_t)((a << 2) | (b >> 4));
            if (cv >= 0 && j < 16) keyBytes[j++] = (uint8_t)((b << 4) | (cv >> 2));
            if (d  >= 0 && j < 16) keyBytes[j++] = (uint8_t)((cv << 6) | d);
            p += 4;
        }
        for (int i = 0; i < 16; i++)
            snprintf(secretHex + i * 2, 3, "%02x", keyBytes[i]);
    }

    char encodedName[64];
    _urlEncodeSettings(cleanName, encodedName, sizeof(encodedName));

    char data[120];
    snprintf(data, sizeof(data), "meshcore://channel/add?name=%s&secret=%s",
             encodedName, secretHex);
    char title[48];
    snprintf(title, sizeof(title), "Channel: #%s", cleanName);
    showQrPopup(title, data);
}

// ── Edit a single channel ─────────────────────────────────────────────
static void _openEditChannelDialog(int ch) {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    s_editCtx.modal = modal;
    s_editCtx.ch    = ch;

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 300, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, OPS_SCREEN_H - 2, 0);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 3, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    char titleBuf[20];
    snprintf(titleBuf, sizeof(titleBuf), "Channel %d", ch + 1);
    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, titleBuf);
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_bottom(title, 2, 0);

    const auto& cfg = ops::config::get();

    // Name field — '#' is the default prefix (editable; stripped on save)
    s_editCtx.nameTa = _makeChField(panel, "Name",
                                    nullptr, 31,
                                    ch == 0 ? "#Public" : "#room name");
    {
        char initName[33];
        if (cfg.channels[ch].name[0])
            snprintf(initName, sizeof(initName), "#%s", cfg.channels[ch].name);
        else
            strncpy(initName, "#", sizeof(initName));
        lv_textarea_set_text(s_editCtx.nameTa, initName);
    }
    // PSK field: label above, shortened textarea + shuffle button side by side
    lv_obj_t* pskLbl = lv_label_create(panel);
    lv_label_set_text(pskLbl, "PSK  (empty = auto-derive from #name)");
    lv_obj_set_style_text_color(pskLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(pskLbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(pskLbl, LV_PCT(100));

    lv_obj_t* pskRow = lv_obj_create(panel);
    lv_obj_set_size(pskRow, LV_PCT(100), 26);
    lv_obj_set_style_bg_opa(pskRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pskRow, 0, 0);
    lv_obj_set_style_pad_all(pskRow, 0, 0);
    lv_obj_clear_flag(pskRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pskRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pskRow,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pskRow, 4, 0);

    lv_obj_t* pskTa = lv_textarea_create(pskRow);
    lv_obj_set_flex_grow(pskTa, 1);
    lv_obj_set_height(pskTa, 26);
    lv_obj_set_style_bg_color(pskTa, theme::BG, 0);
    lv_obj_set_style_text_color(pskTa, theme::TEXT, 0);
    lv_obj_set_style_border_color(pskTa, theme::BORDER, 0);
    lv_obj_set_style_border_color(pskTa, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(pskTa, 1, 0);
    lv_obj_set_style_text_font(pskTa, &lv_font_montserrat_10, 0);
    lv_textarea_set_one_line(pskTa, true);
    lv_textarea_set_max_length(pskTa, 32);
    lv_textarea_set_placeholder_text(pskTa, "auto (#name)");
    lv_textarea_set_text(pskTa, cfg.channels[ch].psk);
    s_editCtx.pskTa = pskTa;

    lv_obj_t* shuffleBtn = lv_btn_create(pskRow);
    lv_obj_set_size(shuffleBtn, 28, 26);
    lv_obj_set_style_bg_color(shuffleBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(shuffleBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(shuffleBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(shuffleBtn, 1, 0);
    lv_obj_set_style_radius(shuffleBtn, 4, 0);
    lv_obj_set_style_shadow_width(shuffleBtn, 0, 0);
    lv_obj_set_style_pad_all(shuffleBtn, 0, 0);
    lv_obj_add_event_cb(shuffleBtn, _onPskShuffle, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* shuffleLbl = lv_label_create(shuffleBtn);
    lv_label_set_text(shuffleLbl, LV_SYMBOL_SHUFFLE);
    lv_obj_set_style_text_color(shuffleLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(shuffleLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(shuffleLbl);
    s_editCtx.scopeTa = _makeChField(panel, "Scope (optional, e.g. AU)",
                                     cfg.channels[ch].scope, 15,
                                     "leave blank for no scope");

    // Button row: [Share QR]  [Save]  [Exit]
    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, LV_PCT(100), 30);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* qrBtn = lv_btn_create(btnRow);
    lv_obj_set_size(qrBtn, 80, 26);
    lv_obj_set_style_bg_color(qrBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(qrBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(qrBtn, theme::ACCENT, 0);
    lv_obj_set_style_border_width(qrBtn, 1, 0);
    lv_obj_set_style_radius(qrBtn, 4, 0);
    lv_obj_set_style_shadow_width(qrBtn, 0, 0);
    lv_obj_add_event_cb(qrBtn, _onEditChShareQR, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(qrBtn, _onEditChKey,     LV_EVENT_KEY,     nullptr);
    lv_obj_t* qrLbl = lv_label_create(qrBtn);
    lv_label_set_text(qrLbl, "Share QR");
    lv_obj_set_style_text_color(qrLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(qrLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(qrLbl);

    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, 100, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onEditChSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(saveBtn, _onEditChKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(btnRow);
    lv_obj_set_size(exitBtn, 80, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onEditChExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onEditChKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, s_editCtx.nameTa);
        lv_group_add_obj(g, s_editCtx.pskTa);
        lv_group_add_obj(g, shuffleBtn);
        lv_group_add_obj(g, s_editCtx.scopeTa);
        lv_group_add_obj(g, qrBtn);
        lv_group_add_obj(g, saveBtn);
        lv_group_add_obj(g, exitBtn);
        lv_group_focus_obj(s_editCtx.nameTa);
    }
    lv_obj_add_event_cb(s_editCtx.nameTa,  _onEditChKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_editCtx.pskTa,   _onEditChKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(shuffleBtn,         _onEditChKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_editCtx.scopeTa, _onEditChKey, LV_EVENT_KEY, nullptr);
    // Enter on scope (last text field) = save
    lv_obj_add_event_cb(s_editCtx.scopeTa, _onEditChSave, LV_EVENT_READY, nullptr);
}

// ── Auto Add Contacts dialog ──────────────────────────────────────────
struct AutoAddCtx {
    lv_obj_t* modal;
    lv_obj_t* clientValLbl;
    lv_obj_t* repValLbl;
    bool clientOn;
    bool repOn;
};
static AutoAddCtx s_aaCtx;

static void _onAAClientToggle(lv_event_t* /*e*/) {
    s_aaCtx.clientOn = !s_aaCtx.clientOn;
    lv_label_set_text(s_aaCtx.clientValLbl, s_aaCtx.clientOn ? "On" : "Off");
    lv_obj_set_style_text_color(s_aaCtx.clientValLbl,
        s_aaCtx.clientOn ? theme::GREEN : theme::TEXT_MUTED, 0);
}

static void _onAARepToggle(lv_event_t* /*e*/) {
    s_aaCtx.repOn = !s_aaCtx.repOn;
    lv_label_set_text(s_aaCtx.repValLbl, s_aaCtx.repOn ? "On" : "Off");
    lv_obj_set_style_text_color(s_aaCtx.repValLbl,
        s_aaCtx.repOn ? theme::GREEN : theme::TEXT_MUTED, 0);
}

static void _onAASave(lv_event_t* /*e*/) {
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.autoAddClient   = s_aaCtx.clientOn;
    cfg.autoAddRepeater = s_aaCtx.repOn;
    ops::config::save();
    lv_obj_del(s_aaCtx.modal);
    ScreenSettings::show();
}

static void _onAAExit(lv_event_t* /*e*/) {
    lv_obj_del(s_aaCtx.modal);
}

static void _onAAKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)
        lv_obj_del(s_aaCtx.modal);
}

static lv_obj_t* _makeToggleRow(lv_obj_t* parent, const char* label,
                                bool on, lv_event_cb_t toggleCb,
                                lv_obj_t* modal = nullptr) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, 220, 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* nameLbl = lv_label_create(row);
    lv_label_set_text(nameLbl, label);
    lv_obj_set_style_text_color(nameLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);

    lv_obj_t* btn = lv_btn_create(row);
    lv_obj_set_size(btn, 44, 22);
    lv_obj_set_style_bg_color(btn, on ? theme::PRIMARY : theme::BG, 0);
    lv_obj_set_style_border_color(btn, theme::BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, toggleCb, LV_EVENT_CLICKED, nullptr);
    lv_group_t* gTog = lv_group_get_default();
    if (gTog) lv_group_add_obj(gTog, btn);
    if (modal) lv_obj_add_event_cb(btn, _onModalKey, LV_EVENT_KEY, modal);

    lv_obj_t* valLbl = lv_label_create(btn);
    lv_label_set_text(valLbl, on ? "On" : "Off");
    lv_obj_set_style_text_color(valLbl, on ? theme::GREEN : theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(valLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(valLbl);

    return valLbl;  // caller stores pointer for live update
}

static void _openAutoAddDialog() {
    const auto& cfg = ops::config::get();
    s_aaCtx.clientOn = cfg.autoAddClient;
    s_aaCtx.repOn    = cfg.autoAddRepeater;

    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_aaCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onAAKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 140);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Auto Add Contacts");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    s_aaCtx.clientValLbl = _makeToggleRow(panel, "Client",    s_aaCtx.clientOn, _onAAClientToggle, modal);
    s_aaCtx.repValLbl    = _makeToggleRow(panel, "Repeaters", s_aaCtx.repOn,    _onAARepToggle,    modal);

    // Button row
    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, 220, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(row);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onAASave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(row);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onAAExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onAAKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* gAA = lv_group_get_default();
    if (gAA) {
        lv_group_add_obj(gAA, saveBtn);
        lv_group_add_obj(gAA, exitBtn);
        // toggle btns already added by _makeToggleRow; focus first one
        lv_group_focus_obj(lv_obj_get_child(panel, 1));  // client row btn
    }
    lv_obj_add_event_cb(saveBtn, _onAAKey, LV_EVENT_KEY, nullptr);
}

// ── Set Time dialog ───────────────────────────────────────────────────
// Five labelled textarea fields: HH, MM, DD, Mon, YYYY.
// Save reads the text, parses the numbers, calls settimeofday().

struct SetTimeCtx {
    lv_obj_t* modal;
    lv_obj_t* taHour;
    lv_obj_t* taMin;
    lv_obj_t* taDay;
    lv_obj_t* taMon;
    lv_obj_t* taYear;
};
static SetTimeCtx s_stCtx;

// Small 2-digit textarea used in the date/time dialog.
static lv_obj_t* _makeNumField2(lv_obj_t* parent, const char* placeholder, int val) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, 40, 26);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 2);
    lv_obj_set_style_bg_color(ta, theme::BG, 0);
    lv_obj_set_style_text_color(ta, theme::TEXT, 0);
    lv_obj_set_style_border_color(ta, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, theme::BORDER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_CENTER, 0);
    lv_textarea_set_placeholder_text(ta, placeholder);
    char buf[4];
    snprintf(buf, sizeof(buf), "%02d", val);
    lv_textarea_set_text(ta, buf);
    return ta;
}

static void _onSTSave(lv_event_t* /*e*/) {
    auto getVal = [](lv_obj_t* ta) { return atoi(lv_textarea_get_text(ta)); };
    struct tm t = {};
    t.tm_hour = getVal(s_stCtx.taHour);
    t.tm_min  = getVal(s_stCtx.taMin);
    t.tm_sec  = 0;
    t.tm_mday = getVal(s_stCtx.taDay);
    t.tm_mon  = getVal(s_stCtx.taMon) - 1;
    t.tm_year = (2000 + getVal(s_stCtx.taYear)) - 1900;  // YY → 20YY
    // User entered local time; convert to UTC by subtracting the offset before storing.
    time_t ts = mktime(&t) - (time_t)ops::config::get().timezoneOffsetHours * 3600;
    struct timeval tv = { ts, 0 };
    settimeofday(&tv, nullptr);
    OPS_LOG("Settings", "Time set: %04d-%02d-%02d %02d:%02d",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    lv_obj_del(s_stCtx.modal);
    ScreenLauncher::refreshClock();
    ScreenSettings::show();
}

static void _onSTExit(lv_event_t* /*e*/) { lv_obj_del(s_stCtx.modal); }
static void _onSTKey(lv_event_t* e) {
    if (lv_event_get_key(e) == LV_KEY_ESC) lv_obj_del(s_stCtx.modal);
}

static void _openSetTimeDialog() {
    // Pre-populate from GPS; fall back to RTC.
    uint8_t hr = 0, mi = 0, dy = 1, mo = 1;
    int     yy = 26;  // 2-digit year
    bool gpsTime = false;
#ifdef OPS_HAS_BUILTIN_GPS
    if (ops::config::get().gpsMode != GPS_MODE_OFF) {
        uint16_t yr4 = 0; uint8_t sc = 0;
        gpsTime = Board::instance().gpsDateTime(yr4, mo, dy, hr, mi, sc);
        if (gpsTime) yy = (int)(yr4 % 100);
    }
#endif
    if (!gpsTime) {
        time_t now = ops::config::localEpoch();
        struct tm cur;
        gmtime_r(&now, &cur);
        hr = (uint8_t)cur.tm_hour; mi = (uint8_t)cur.tm_min;
        dy = (uint8_t)cur.tm_mday; mo = (uint8_t)(cur.tm_mon + 1);
        yy = (cur.tm_year + 1900) % 100;
    }

    // ── Modal overlay ────────────────────────────────────────────────────
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_stCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onSTKey, LV_EVENT_KEY, nullptr);

    // ── Panel — same width as notifications window ───────────────────────
    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 160);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Title
    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Set Date / Time");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    // Helper: separator label (":" or "/")
    auto makeSep = [](lv_obj_t* parent, const char* text) {
        lv_obj_t* lbl = lv_label_create(parent);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    };

    // Helper: inline transparent row container
    auto makeRow = [](lv_obj_t* parent) -> lv_obj_t* {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        return row;
    };

    // ── Row 1: HH : MM ───────────────────────────────────────────────
    lv_obj_t* timeRow = makeRow(panel);
    s_stCtx.taHour = _makeNumField2(timeRow, "HH", (int)hr);
    makeSep(timeRow, ":");
    s_stCtx.taMin  = _makeNumField2(timeRow, "MM", (int)mi);

    // ── Row 2: DD / MM / YY ──────────────────────────────────────────
    lv_obj_t* dateRow = makeRow(panel);
    s_stCtx.taDay  = _makeNumField2(dateRow, "DD", (int)dy);
    makeSep(dateRow, "/");
    s_stCtx.taMon  = _makeNumField2(dateRow, "MM", (int)mo);
    makeSep(dateRow, "/");
    s_stCtx.taYear = _makeNumField2(dateRow, "YY", yy);

    // ── Button row ───────────────────────────────────────────────────
    lv_obj_t* btnRow = makeRow(panel);
    lv_obj_set_style_pad_column(btnRow, 12, 0);

    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onSTSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(saveBtn, _onSTKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(btnRow);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onSTExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onSTKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    // ── Focus group ──────────────────────────────────────────────────
    lv_group_t* gST = lv_group_get_default();
    if (gST) {
        lv_group_add_obj(gST, s_stCtx.taHour);
        lv_group_add_obj(gST, s_stCtx.taMin);
        lv_group_add_obj(gST, s_stCtx.taDay);
        lv_group_add_obj(gST, s_stCtx.taMon);
        lv_group_add_obj(gST, s_stCtx.taYear);
        lv_group_add_obj(gST, saveBtn);
        lv_group_add_obj(gST, exitBtn);
        lv_group_focus_obj(s_stCtx.taHour);
    }
    lv_obj_add_event_cb(s_stCtx.taHour, _onSTKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_stCtx.taMin,  _onSTKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_stCtx.taDay,  _onSTKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_stCtx.taMon,  _onSTKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_stCtx.taYear, _onSTKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_stCtx.taYear, _onSTSave, LV_EVENT_READY, nullptr);
}

// ── Notifications dialog ─────────────────────────────────────────────
// Two rows: Sound (notifySound) and Popup (notifyPopup), each toggleable.

struct NotifCtx { lv_obj_t* modal; lv_obj_t* soundLbl; lv_obj_t* popupLbl;
                  bool soundOn; bool popupOn; };
static NotifCtx s_nCtx;

static void _onNSoundToggle(lv_event_t* /*e*/) {
    s_nCtx.soundOn = !s_nCtx.soundOn;
    lv_label_set_text(s_nCtx.soundLbl, s_nCtx.soundOn ? "On" : "Off");
    lv_obj_set_style_text_color(s_nCtx.soundLbl,
        s_nCtx.soundOn ? theme::GREEN : theme::TEXT_MUTED, 0);
}
static void _onNPopupToggle(lv_event_t* /*e*/) {
    s_nCtx.popupOn = !s_nCtx.popupOn;
    lv_label_set_text(s_nCtx.popupLbl, s_nCtx.popupOn ? "On" : "Off");
    lv_obj_set_style_text_color(s_nCtx.popupLbl,
        s_nCtx.popupOn ? theme::GREEN : theme::TEXT_MUTED, 0);
}
static void _onNSave(lv_event_t* /*e*/) {
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.notifySound = s_nCtx.soundOn;
    cfg.notifyPopup = s_nCtx.popupOn;
    ops::config::save();
    lv_obj_del(s_nCtx.modal);
    ScreenSettings::show();
}
static void _onNExit(lv_event_t* /*e*/) { lv_obj_del(s_nCtx.modal); }
static void _onNKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) lv_obj_del(s_nCtx.modal);
}

static void _openNotificationsDialog() {
    const auto& cfg = ops::config::get();
    s_nCtx.soundOn = cfg.notifySound;
    s_nCtx.popupOn = cfg.notifyPopup;

    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_nCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onNKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 140);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Notifications");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    s_nCtx.soundLbl = _makeToggleRow(panel, "Sound",  s_nCtx.soundOn, _onNSoundToggle, modal);
    s_nCtx.popupLbl = _makeToggleRow(panel, "Popup",  s_nCtx.popupOn, _onNPopupToggle, modal);

    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, 220, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(row);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onNSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(row);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onNExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onNKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* gN = lv_group_get_default();
    if (gN) {
        lv_group_add_obj(gN, saveBtn);
        lv_group_add_obj(gN, exitBtn);
        // toggle btns already added by _makeToggleRow; focus first one
        lv_group_focus_obj(lv_obj_get_child(panel, 1));  // Sound row btn
    }
    lv_obj_add_event_cb(saveBtn, _onNKey, LV_EVENT_KEY, nullptr);
}

// ── Timezone slider dialog ────────────────────────────────────────────
// lv_slider from -11 to +11, representing UTC offset in whole hours.

struct TzCtx { lv_obj_t* modal; lv_obj_t* slider; lv_obj_t* valLbl; };
static TzCtx s_tzCtx;

static void _fmtTzVal(char* buf, size_t len, int off) {
    if (off == 0)       snprintf(buf, len, "UTC");
    else if (off > 0)   snprintf(buf, len, "UTC+%d", off);
    else                snprintf(buf, len, "UTC%d",  off);
}

static void _onTzSlide(lv_event_t* /*e*/) {
    int v = (int)lv_slider_get_value(s_tzCtx.slider);
    char buf[10];
    _fmtTzVal(buf, sizeof(buf), v);
    lv_label_set_text(s_tzCtx.valLbl, buf);
}

static void _onTzSave(lv_event_t* /*e*/) {
    int v = (int)lv_slider_get_value(s_tzCtx.slider);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.timezoneOffsetHours = (int8_t)v;
    ops::config::save();
    lv_obj_del(s_tzCtx.modal);
    ScreenSettings::show();
}
static void _onTzExit(lv_event_t* /*e*/) { lv_obj_del(s_tzCtx.modal); }
static void _onTzKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) lv_obj_del(s_tzCtx.modal);
}

static void _openTimezoneDialog() {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_tzCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onTzKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 165);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Timezone");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    // Current value label
    int curOff = (int)ops::config::get().timezoneOffsetHours;
    s_tzCtx.valLbl = lv_label_create(panel);
    char initBuf[10];
    _fmtTzVal(initBuf, sizeof(initBuf), curOff);
    lv_label_set_text(s_tzCtx.valLbl, initBuf);
    lv_obj_set_style_text_color(s_tzCtx.valLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(s_tzCtx.valLbl, &lv_font_montserrat_12, 0);

    // Slider: -11 to +11 (whole-hour UTC offsets)
    s_tzCtx.slider = lv_slider_create(panel);
    lv_obj_set_width(s_tzCtx.slider, 220);
    lv_slider_set_range(s_tzCtx.slider, -11, 11);
    lv_slider_set_value(s_tzCtx.slider, curOff, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_tzCtx.slider, theme::PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_tzCtx.slider, theme::ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_tzCtx.slider, theme::BORDER, LV_PART_MAIN);
    lv_obj_add_event_cb(s_tzCtx.slider, _onTzSlide, LV_EVENT_VALUE_CHANGED, nullptr);

    // Button row
    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, 220, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(row);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onTzSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(row);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onTzExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onTzKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* gTz = lv_group_get_default();
    if (gTz) {
        lv_group_add_obj(gTz, s_tzCtx.slider);
        lv_group_add_obj(gTz, saveBtn);
        lv_group_add_obj(gTz, exitBtn);
        lv_group_focus_obj(s_tzCtx.slider);
    }
    lv_obj_add_event_cb(s_tzCtx.slider, _onTzKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(saveBtn,        _onTzKey, LV_EVENT_KEY, nullptr);
}

// ── Screen Timeout slider dialog ──────────────────────────────────────
// Slider steps 2–12 (each step = 10 s) → 20 s – 2 min in 10-second intervals.

struct TimeoutCtx { lv_obj_t* modal; lv_obj_t* slider; lv_obj_t* valLbl; };
static TimeoutCtx s_toCtx;

static void _fmtTimeoutVal(char* buf, size_t len, int sec) {
    if (sec <= 0)        snprintf(buf, len, "Off");
    else if (sec < 60)  snprintf(buf, len, "%ds", sec);
    else if (sec == 60) snprintf(buf, len, "1m");
    else if (sec < 120) snprintf(buf, len, "1m %ds", sec - 60);
    else                snprintf(buf, len, "2m");
}

static void _onToSlide(lv_event_t* /*e*/) {
    int step = (int)lv_slider_get_value(s_toCtx.slider);
    char buf[16];
    _fmtTimeoutVal(buf, sizeof(buf), step * 10);
    lv_label_set_text(s_toCtx.valLbl, buf);
}

static void _onToSave(lv_event_t* /*e*/) {
    int step = (int)lv_slider_get_value(s_toCtx.slider);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.screenTimeoutSec = step * 10;
    ops::config::save();
    lv_obj_del(s_toCtx.modal);
    ScreenSettings::show();
}
static void _onToExit(lv_event_t* /*e*/) { lv_obj_del(s_toCtx.modal); }
static void _onToKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) lv_obj_del(s_toCtx.modal);
}

static void _openTimeoutDialog() {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_toCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onToKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 165);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Screen Timeout");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    // Map current value to step (each step = 10s, range 2–12 → 20s–120s)
    int curVal  = ops::config::get().screenTimeoutSec;
    int curStep = curVal / 10;
    if (curStep < 2)  curStep = 2;
    if (curStep > 12) curStep = 12;

    s_toCtx.valLbl = lv_label_create(panel);
    char initBuf[16];
    _fmtTimeoutVal(initBuf, sizeof(initBuf), curStep * 10);
    lv_label_set_text(s_toCtx.valLbl, initBuf);
    lv_obj_set_style_text_color(s_toCtx.valLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(s_toCtx.valLbl, &lv_font_montserrat_12, 0);

    // Slider: steps 2–12 (×10s = 20s–120s, 10-second intervals)
    s_toCtx.slider = lv_slider_create(panel);
    lv_obj_set_width(s_toCtx.slider, 220);
    lv_slider_set_range(s_toCtx.slider, 2, 12);
    lv_slider_set_value(s_toCtx.slider, curStep, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_toCtx.slider, theme::PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_toCtx.slider, theme::ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_toCtx.slider, theme::BORDER, LV_PART_MAIN);
    lv_obj_add_event_cb(s_toCtx.slider, _onToSlide, LV_EVENT_VALUE_CHANGED, nullptr);

    // Button row
    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, 220, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(row);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onToSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(row);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onToExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onToKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* gTo = lv_group_get_default();
    if (gTo) {
        lv_group_add_obj(gTo, s_toCtx.slider);
        lv_group_add_obj(gTo, saveBtn);
        lv_group_add_obj(gTo, exitBtn);
        lv_group_focus_obj(s_toCtx.slider);
    }
    lv_obj_add_event_cb(s_toCtx.slider, _onToKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(saveBtn,        _onToKey, LV_EVENT_KEY, nullptr);
}

// ── Screen Off slider dialog ──────────────────────────────────────────
// Slider steps 0–12: 0 = Off, 2–12 = 20s–120s (×10s, 10-second intervals).

struct ScreenOffCtx { lv_obj_t* modal; lv_obj_t* slider; lv_obj_t* valLbl; };
static ScreenOffCtx s_soCtx;

static void _fmtScreenOffVal(char* buf, size_t len, int sec) {
    if (sec < 20)        snprintf(buf, len, "Off");
    else if (sec < 60)  snprintf(buf, len, "%ds", sec);
    else if (sec == 60) snprintf(buf, len, "1m");
    else if (sec < 120) snprintf(buf, len, "1m %ds", sec - 60);
    else                snprintf(buf, len, "2m");
}

static void _onSoSlide(lv_event_t* /*e*/) {
    int step = (int)lv_slider_get_value(s_soCtx.slider);
    char buf[16];
    _fmtScreenOffVal(buf, sizeof(buf), step * 10);
    lv_label_set_text(s_soCtx.valLbl, buf);
}

static void _onSoSave(lv_event_t* /*e*/) {
    int step = (int)lv_slider_get_value(s_soCtx.slider);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.screenOffSec = (step < 2) ? 0 : (uint8_t)(step * 10);
    ops::config::save();
    lv_obj_del(s_soCtx.modal);
    ScreenSettings::show();
}
static void _onSoExit(lv_event_t* /*e*/) { lv_obj_del(s_soCtx.modal); }
static void _onSoKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) lv_obj_del(s_soCtx.modal);
}

static void _openScreenOffDialog() {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_soCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onSoKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 165);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Screen Off");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t* hint = lv_label_create(panel);
    lv_label_set_text(hint, "Backlight off after screensaver");
    lv_obj_set_style_text_color(hint, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);

    // Map stored seconds to step (0=Off, 2–12 = 20s–120s)
    int curSec  = (int)ops::config::get().screenOffSec;
    int curStep = (curSec < 20) ? 0 : (curSec / 10);
    if (curStep > 12) curStep = 12;

    s_soCtx.valLbl = lv_label_create(panel);
    char initBuf[16];
    _fmtScreenOffVal(initBuf, sizeof(initBuf), curStep * 10);
    lv_label_set_text(s_soCtx.valLbl, initBuf);
    lv_obj_set_style_text_color(s_soCtx.valLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(s_soCtx.valLbl, &lv_font_montserrat_12, 0);

    // Slider: 0 = Off, 2–12 (×10s = 20s–120s, 10-second intervals)
    s_soCtx.slider = lv_slider_create(panel);
    lv_obj_set_width(s_soCtx.slider, 220);
    lv_slider_set_range(s_soCtx.slider, 0, 12);
    lv_slider_set_value(s_soCtx.slider, curStep, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_soCtx.slider, theme::PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_soCtx.slider, theme::ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_soCtx.slider, theme::BORDER, LV_PART_MAIN);
    lv_obj_add_event_cb(s_soCtx.slider, _onSoSlide, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, 220, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(row);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onSoSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(row);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onSoExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onSoKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* gSo = lv_group_get_default();
    if (gSo) {
        lv_group_add_obj(gSo, s_soCtx.slider);
        lv_group_add_obj(gSo, saveBtn);
        lv_group_add_obj(gSo, exitBtn);
        lv_group_focus_obj(s_soCtx.slider);
    }
    lv_obj_add_event_cb(s_soCtx.slider, _onSoKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(saveBtn,        _onSoKey, LV_EVENT_KEY, nullptr);
}

// ── Keyboard brightness slider dialog ────────────────────────────────
// lv_slider from 0 (off) to 255. Applies immediately on Save.

struct KbBrightCtx { lv_obj_t* modal; lv_obj_t* slider; lv_obj_t* valLbl; uint8_t origVal; };
static KbBrightCtx s_kbCtx;

static void _onKbSlide(lv_event_t* /*e*/) {
    int v = (int)lv_slider_get_value(s_kbCtx.slider);
    char buf[8];
    if (v == 0) snprintf(buf, sizeof(buf), "Off");
    else        snprintf(buf, sizeof(buf), "%d", v);
    lv_label_set_text(s_kbCtx.valLbl, buf);
    // Live preview — lets user see the change immediately without pressing Save.
    ops::Board::instance().setKeyboardBacklight((uint8_t)v);
}

static void _onKbSave(lv_event_t* /*e*/) {
    uint8_t v = (uint8_t)lv_slider_get_value(s_kbCtx.slider);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.kbBrightness = v;
    ops::config::save();
    ops::Board::instance().setKeyboardBacklight(v);
    lv_obj_del(s_kbCtx.modal);
    ScreenSettings::show();
}
static void _onKbExit(lv_event_t* /*e*/) {
    ops::Board::instance().setKeyboardBacklight(s_kbCtx.origVal);
    lv_obj_del(s_kbCtx.modal);
}
static void _onKbKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
        ops::Board::instance().setKeyboardBacklight(s_kbCtx.origVal);
        lv_obj_del(s_kbCtx.modal);
    }
}

static void _openKbBrightDialog() {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_kbCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onKbKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 165);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Keyboard Light");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    uint8_t curVal = ops::config::get().kbBrightness;
    s_kbCtx.origVal = curVal;
    s_kbCtx.valLbl = lv_label_create(panel);
    char initBuf[8];
    if (curVal == 0) snprintf(initBuf, sizeof(initBuf), "Off");
    else             snprintf(initBuf, sizeof(initBuf), "%d", curVal);
    lv_label_set_text(s_kbCtx.valLbl, initBuf);
    lv_obj_set_style_text_color(s_kbCtx.valLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(s_kbCtx.valLbl, &lv_font_montserrat_12, 0);

    s_kbCtx.slider = lv_slider_create(panel);
    lv_obj_set_width(s_kbCtx.slider, 220);
    lv_slider_set_range(s_kbCtx.slider, 0, 255);
    lv_slider_set_value(s_kbCtx.slider, (int)curVal, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_kbCtx.slider, theme::PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_kbCtx.slider, theme::ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_kbCtx.slider, theme::BORDER, LV_PART_MAIN);
    lv_obj_add_event_cb(s_kbCtx.slider, _onKbSlide, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, 220, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(row);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onKbSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(row);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onKbExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onKbKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* gKb = lv_group_get_default();
    if (gKb) {
        lv_group_add_obj(gKb, s_kbCtx.slider);
        lv_group_add_obj(gKb, saveBtn);
        lv_group_add_obj(gKb, exitBtn);
        lv_group_focus_obj(s_kbCtx.slider);
    }
    lv_obj_add_event_cb(s_kbCtx.slider, _onKbKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(saveBtn,        _onKbKey, LV_EVENT_KEY, nullptr);
}

// ── TX Power picker ──────────────────────────────────────────────────

struct TxPowerCtx { lv_obj_t* modal; lv_obj_t* slider; lv_obj_t* valLbl;
                    lv_obj_t* boostBtn; lv_obj_t* boostLbl; bool boostOn; };
static TxPowerCtx s_txCtx;

static void _onTxPowerSlide(lv_event_t* /*e*/) {
    int v = (int)lv_slider_get_value(s_txCtx.slider);
    char buf[10];
    snprintf(buf, sizeof(buf), "%d dBm", v);
    lv_label_set_text(s_txCtx.valLbl, buf);
}

static void _onTxBoostToggle(lv_event_t* /*e*/) {
    s_txCtx.boostOn = !s_txCtx.boostOn;
    lv_label_set_text(s_txCtx.boostLbl,
        s_txCtx.boostOn ? "RX Boost: ON" : "RX Boost: OFF");
    lv_obj_set_style_bg_color(s_txCtx.boostBtn,
        s_txCtx.boostOn ? theme::PRIMARY : theme::BG, 0);
    lv_obj_set_style_text_color(s_txCtx.boostLbl,
        s_txCtx.boostOn ? theme::ACCENT : theme::TEXT_MUTED, 0);
}

static void _onTxPowerSave(lv_event_t* /*e*/) {
    int8_t v = (int8_t)lv_slider_get_value(s_txCtx.slider);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.radioTX   = v;
    cfg.rxBoost   = s_txCtx.boostOn;
    ops::config::save();
    ops::MeshService::instance().setTxPower(v);
    ops::MeshService::instance().setRxBoost(s_txCtx.boostOn);
    lv_obj_del(s_txCtx.modal);
    ScreenSettings::show();
}

static void _onTxPowerExit(lv_event_t* /*e*/) {
    lv_obj_del(s_txCtx.modal);
}

static void _onTxPowerKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)
        lv_obj_del(s_txCtx.modal);
}

static void _openTxPowerDialog() {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_txCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onTxPowerKey, LV_EVENT_KEY, nullptr);

    s_txCtx.boostOn = ops::config::get().rxBoost;

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 198);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "TX Power");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    int8_t curVal = ops::config::get().radioTX;
    if (curVal < 10 || curVal > 22) curVal = 22;

    s_txCtx.valLbl = lv_label_create(panel);
    char initBuf[10];
    snprintf(initBuf, sizeof(initBuf), "%d dBm", curVal);
    lv_label_set_text(s_txCtx.valLbl, initBuf);
    lv_obj_set_style_text_color(s_txCtx.valLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(s_txCtx.valLbl, &lv_font_montserrat_12, 0);

    s_txCtx.slider = lv_slider_create(panel);
    lv_obj_set_width(s_txCtx.slider, 220);
    lv_slider_set_range(s_txCtx.slider, 10, 22);
    lv_slider_set_value(s_txCtx.slider, (int)curVal, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_txCtx.slider, theme::PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_txCtx.slider, theme::ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_txCtx.slider, theme::BORDER, LV_PART_MAIN);
    lv_obj_add_event_cb(s_txCtx.slider, _onTxPowerSlide, LV_EVENT_VALUE_CHANGED, nullptr);

    // ── RX Boost toggle ──────────────────────────────────────────────
    lv_obj_t* boostBtn = lv_btn_create(panel);
    s_txCtx.boostBtn = boostBtn;
    lv_obj_set_size(boostBtn, 220, 26);
    lv_obj_set_style_bg_color(boostBtn,
        s_txCtx.boostOn ? theme::PRIMARY : theme::BG, 0);
    lv_obj_set_style_bg_color(boostBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(boostBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(boostBtn, 1, 0);
    lv_obj_set_style_radius(boostBtn, 4, 0);
    lv_obj_set_style_shadow_width(boostBtn, 0, 0);
    lv_obj_add_event_cb(boostBtn, _onTxBoostToggle, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(boostBtn, _onTxPowerKey,    LV_EVENT_KEY,     nullptr);
    lv_obj_t* boostLbl = lv_label_create(boostBtn);
    s_txCtx.boostLbl = boostLbl;
    lv_label_set_text(boostLbl, s_txCtx.boostOn ? "RX Boost: ON" : "RX Boost: OFF");
    lv_obj_set_style_text_color(boostLbl,
        s_txCtx.boostOn ? theme::ACCENT : theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(boostLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(boostLbl);

    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, 220, 32);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onTxPowerSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(btnRow);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onTxPowerExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onTxPowerKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* gTx = lv_group_get_default();
    if (gTx) {
        lv_group_add_obj(gTx, s_txCtx.slider);
        lv_group_add_obj(gTx, boostBtn);
        lv_group_add_obj(gTx, saveBtn);
        lv_group_add_obj(gTx, exitBtn);
        lv_group_focus_obj(s_txCtx.slider);
    }
    lv_obj_add_event_cb(s_txCtx.slider, _onTxPowerKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(saveBtn,        _onTxPowerKey, LV_EVENT_KEY, nullptr);
}

// ── Brightness picker ────────────────────────────────────────────────

struct BrightCtx { lv_obj_t* modal; lv_obj_t* slider; lv_obj_t* valLbl; uint8_t origVal; };
static BrightCtx s_brightCtx;

static void _onBrightSlide(lv_event_t* /*e*/) {
    int v = (int)lv_slider_get_value(s_brightCtx.slider);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v * 100 / 255);
    lv_label_set_text(s_brightCtx.valLbl, buf);
    ops::Board::instance().setDisplayBrightness((uint8_t)v);
}

static void _onBrightSave(lv_event_t* /*e*/) {
    uint8_t v = (uint8_t)lv_slider_get_value(s_brightCtx.slider);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.brightness = (int)v;
    ops::config::save();
    ops::Board::instance().setDisplayBrightness(v);
    lv_obj_del(s_brightCtx.modal);
    ScreenSettings::show();
}

static void _onBrightExit(lv_event_t* /*e*/) {
    ops::Board::instance().setDisplayBrightness(s_brightCtx.origVal);
    lv_obj_del(s_brightCtx.modal);
}

static void _onBrightKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
        ops::Board::instance().setDisplayBrightness(s_brightCtx.origVal);
        lv_obj_del(s_brightCtx.modal);
    }
}

static void _openBrightnessDialog() {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_brightCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onBrightKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 165);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Brightness");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    int curRaw = ops::config::get().brightness;
    if (curRaw < 128) curRaw = 200;   // clamp to minimum 50%
    s_brightCtx.origVal = (uint8_t)curRaw;

    s_brightCtx.valLbl = lv_label_create(panel);
    char initBuf[8];
    snprintf(initBuf, sizeof(initBuf), "%d%%", curRaw * 100 / 255);
    lv_label_set_text(s_brightCtx.valLbl, initBuf);
    lv_obj_set_style_text_color(s_brightCtx.valLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(s_brightCtx.valLbl, &lv_font_montserrat_12, 0);

    s_brightCtx.slider = lv_slider_create(panel);
    lv_obj_set_width(s_brightCtx.slider, 220);
    lv_slider_set_range(s_brightCtx.slider, 128, 255);
    lv_slider_set_value(s_brightCtx.slider, curRaw, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_brightCtx.slider, theme::PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_brightCtx.slider, theme::ACCENT, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_brightCtx.slider, theme::BORDER, LV_PART_MAIN);
    lv_obj_add_event_cb(s_brightCtx.slider, _onBrightSlide, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, 220, 32);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onBrightSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(btnRow);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onBrightExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onBrightKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* gBr = lv_group_get_default();
    if (gBr) {
        lv_group_add_obj(gBr, s_brightCtx.slider);
        lv_group_add_obj(gBr, saveBtn);
        lv_group_add_obj(gBr, exitBtn);
        lv_group_focus_obj(s_brightCtx.slider);
    }
    lv_obj_add_event_cb(s_brightCtx.slider, _onBrightKey, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(saveBtn,            _onBrightKey, LV_EVENT_KEY, nullptr);
}

// ── Radio profile picker ──────────────────────────────────────────────
static void _openCustomRadioDialog();  // forward declaration

struct RadioProfile {
    const char* name;
    const char* params;
};

static constexpr int kNumRadioProfiles = 15;

static const RadioProfile kRadioProfiles[kNumRadioProfiles] = {
    { "Australia",          "SF10 BW250 CR5" },  // 0  915.800 MHz
    { "Australia Vic.",     "SF7 BW62 CR8"   },  // 1  916.575 MHz
    { "EU/UK Narrow (Rec)","SF8 BW62 CR8"   },  // 2  869.618 MHz
    { "EU/UK Long Range",   "SF11 BW250 CR5" },  // 3  869.525 MHz
    { "EU/UK Medium",       "SF10 BW250 CR5" },  // 4  869.525 MHz
    { "Czech Narrow",       "SF7 BW62 CR5"   },  // 5  869.525 MHz
    { "EU 433 Long Range",  "SF11 BW250 CR5" },  // 6  433.650 MHz
    { "New Zealand",        "SF11 BW250 CR5" },  // 7  917.375 MHz
    { "NZ Narrow",          "SF7 BW62 CR5"   },  // 8  917.375 MHz
    { "Portugal 433",       "SF9 BW62 CR6"   },  // 9  433.375 MHz
    { "Portugal 868",       "SF7 BW62 CR6"   },  // 10 869.618 MHz
    { "Switzerland",        "SF8 BW62 CR8"   },  // 11 869.618 MHz
    { "USA/Canada",         "SF7 BW62 CR5"   },  // 12 910.525 MHz
    { "Vietnam",            "SF11 BW250 CR5" },  // 13 920.250 MHz
    { "Custom",             ""               },  // 14 user-defined
};

static void _onRadioRowClick(lv_event_t* e) {
    int* info = static_cast<int*>(lv_event_get_user_data(e));
    lv_obj_t* modal   = reinterpret_cast<lv_obj_t*>((uintptr_t)info[0]);
    uint8_t   profIdx = (uint8_t)info[1];
    delete[] info;

    if (profIdx == 14) {
        lv_obj_del(modal);
        _openCustomRadioDialog();
        return;
    }

    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.radioProfile = profIdx;
    cfg.radioCustom  = false;
    ops::config::save();
    ops::MeshService::instance().applyLoraProfile(profIdx);
    OPS_LOG("Settings", "Radio profile -> %d", profIdx);

    lv_obj_del(modal);
    ScreenSettings::show();
}

static void _onRadioClose(lv_event_t* e) {
    lv_obj_t* modal = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_obj_del(modal);
}

static void _openRadioDialog() {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, [](lv_event_t* e) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)
            lv_obj_del(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
    }, LV_EVENT_KEY, modal);

    // Panel: fixed height; title + scrollable row list + exit button
    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 210);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 4, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Radio Profile");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    // Scrollable container for the 14 profile rows
    lv_obj_t* scroll = lv_obj_create(panel);
    lv_obj_set_size(scroll, 222, 144);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_set_style_pad_row(scroll, 2, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const auto& radioCfg = ops::config::get();
    uint8_t curProfile = radioCfg.radioProfile;
    bool isCustomMode  = radioCfg.radioCustom;
    if (curProfile >= 14) curProfile = 2;

    for (int i = 0; i < kNumRadioProfiles; i++) {
        bool isSelected = isCustomMode ? (i == 14) : (i == (int)curProfile);

        lv_obj_t* row = lv_btn_create(scroll);
        lv_obj_set_size(row, 218, 26);
        lv_obj_set_style_bg_color(row, isSelected ? theme::PRIMARY : theme::BG, 0);
        lv_obj_set_style_bg_color(row, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, isSelected ? theme::ACCENT : theme::BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
            LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, kRadioProfiles[i].name);
        lv_obj_set_style_text_color(nameLbl,
            isSelected ? theme::ACCENT : theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);

        lv_obj_t* parLbl = lv_label_create(row);
        if (i == 14) {
            if (radioCfg.radioCustom && radioCfg.freqMHz > 0.0f) {
                static char s_customParBuf[32];
                static const char* kBWStr[] = { "", "62.5", "125", "250" };
                const char* bwStr = (radioCfg.radioBW >= 1 && radioCfg.radioBW <= 3)
                    ? kBWStr[radioCfg.radioBW] : "?";
                snprintf(s_customParBuf, sizeof(s_customParBuf), "%.3f SF%d BW%s CR%d",
                         (double)radioCfg.freqMHz, radioCfg.radioSF, bwStr, radioCfg.radioCR);
                lv_label_set_text(parLbl, s_customParBuf);
            } else {
                lv_label_set_text(parLbl, "Tap to configure");
            }
        } else {
            lv_label_set_text(parLbl, kRadioProfiles[i].params);
        }
        lv_obj_set_style_text_color(parLbl, theme::TEXT_MUTED, 0);
        lv_obj_set_style_text_font(parLbl, &lv_font_montserrat_10, 0);

        int* info = new int[2];
        info[0] = (int)(uintptr_t)modal;
        info[1] = i;
        lv_obj_add_event_cb(row, _onRadioRowClick, LV_EVENT_CLICKED, info);
        lv_obj_add_event_cb(row, [](lv_event_t* ev) {
            if (lv_event_get_key(ev) == LV_KEY_ESC)
                lv_obj_del(static_cast<lv_obj_t*>(lv_event_get_user_data(ev)));
        }, LV_EVENT_KEY, modal);
    }

    // Exit button (below the scroll container, inside panel)
    lv_obj_t* exitBtn = lv_btn_create(panel);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onRadioClose, LV_EVENT_CLICKED, modal);
    lv_obj_add_event_cb(exitBtn, [](lv_event_t* ev) {
        if (lv_event_get_key(ev) == LV_KEY_ESC)
            lv_obj_del(static_cast<lv_obj_t*>(lv_event_get_user_data(ev)));
    }, LV_EVENT_KEY, modal);

    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    // Add all profile rows and exit button to the default encoder group.
    // LVGL auto-scrolls the container to keep the focused row visible.
    lv_group_t* gR = lv_group_get_default();
    if (gR) {
        uint32_t nRows = lv_obj_get_child_cnt(scroll);
        for (uint32_t ci = 0; ci < nRows; ci++)
            lv_group_add_obj(gR, lv_obj_get_child(scroll, (int32_t)ci));
        lv_group_add_obj(gR, exitBtn);
        int32_t focusIdx = isCustomMode ? 14 : (int32_t)curProfile;
        lv_obj_t* focusRow = lv_obj_get_child(scroll, focusIdx);
        lv_group_focus_obj(focusRow);
        lv_obj_scroll_to_view(focusRow, LV_ANIM_OFF);
    }
}

// ── Custom radio parameter dialog ─────────────────────────────────────

struct RadioCustomCtx {
    lv_obj_t* modal;
    lv_obj_t* freqTa;
    lv_obj_t* sfDd;
    lv_obj_t* bwDd;
    lv_obj_t* crDd;
};
static RadioCustomCtx s_rcCtx;

static void _onCustomRadioSave(lv_event_t* /*e*/) {
    const char* freqStr = lv_textarea_get_text(s_rcCtx.freqTa);
    float freq = (float)atof(freqStr);
    if (freq < 400.0f || freq > 960.0f) return;

    uint8_t sfSel = (uint8_t)lv_dropdown_get_selected(s_rcCtx.sfDd);
    uint8_t bwSel = (uint8_t)lv_dropdown_get_selected(s_rcCtx.bwDd);
    uint8_t crSel = (uint8_t)lv_dropdown_get_selected(s_rcCtx.crDd);

    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.radioCustom = true;
    cfg.freqMHz  = freq;
    cfg.radioSF  = 7 + sfSel;   // 7..12
    cfg.radioBW  = 1 + bwSel;   // 1=62.5, 2=125, 3=250
    cfg.radioCR  = 5 + crSel;   // 5..8
    ops::config::save();
    ops::MeshService::instance().applyRadioOverrides();
    OPS_LOG("Settings", "Custom radio: %.3f MHz SF%d BW%d CR%d",
            (double)cfg.freqMHz, cfg.radioSF, cfg.radioBW, cfg.radioCR);
    lv_obj_del(s_rcCtx.modal);
    ScreenSettings::show();
}

static void _onCustomRadioCancel(lv_event_t* /*e*/) {
    lv_obj_del(s_rcCtx.modal);
}

static void _onCustomRadioKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)
        lv_obj_del(s_rcCtx.modal);
}

static lv_obj_t* _makeCustomRadioRow(lv_obj_t* parent, const char* label) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
        LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_width(lbl, 60);
    return row;
}

static lv_obj_t* _makeCustomRadioDd(lv_obj_t* parent, const char* opts, uint8_t sel) {
    lv_obj_t* dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_set_width(dd, 150);
    lv_obj_set_style_bg_color(dd, theme::BG, 0);
    lv_obj_set_style_text_color(dd, theme::TEXT, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(dd, theme::BORDER, 0);
    lv_obj_set_style_border_color(dd, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_t* list = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(list, theme::BG_CARD, 0);
    lv_obj_set_style_text_color(list, theme::TEXT, 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(list, theme::BORDER, 0);
    return dd;
}

static void _openCustomRadioDialog() {
    const auto& cfg = ops::config::get();

    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_rcCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onCustomRadioKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 250, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, OPS_SCREEN_H - 4, 0);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 5, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Custom Radio");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    // Freq MHz
    lv_obj_t* freqRow = _makeCustomRadioRow(panel, "Freq MHz");
    char freqBuf[12] = "";
    if (cfg.freqMHz > 0.0f) snprintf(freqBuf, sizeof(freqBuf), "%.3f", (double)cfg.freqMHz);
    lv_obj_t* freqTa = lv_textarea_create(freqRow);
    s_rcCtx.freqTa = freqTa;
    lv_obj_set_width(freqTa, 150);
    lv_obj_set_height(freqTa, 26);
    lv_obj_set_style_bg_color(freqTa, theme::BG, 0);
    lv_obj_set_style_text_color(freqTa, theme::TEXT, 0);
    lv_obj_set_style_border_color(freqTa, theme::BORDER, 0);
    lv_obj_set_style_border_color(freqTa, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(freqTa, 1, 0);
    lv_obj_set_style_text_font(freqTa, theme::bodyFont10(), 0);
    lv_textarea_set_one_line(freqTa, true);
    lv_textarea_set_max_length(freqTa, 9);
    lv_textarea_set_placeholder_text(freqTa, "e.g. 869.618");
    lv_textarea_set_text(freqTa, freqBuf);

    // SF (spreading factor 7-12)
    lv_obj_t* sfRow = _makeCustomRadioRow(panel, "SF");
    uint8_t sfIdx = (cfg.radioSF >= 7 && cfg.radioSF <= 12) ? (cfg.radioSF - 7) : 0;
    s_rcCtx.sfDd = _makeCustomRadioDd(sfRow, "SF7\nSF8\nSF9\nSF10\nSF11\nSF12", sfIdx);

    // BW (bandwidth)
    lv_obj_t* bwRow = _makeCustomRadioRow(panel, "BW");
    uint8_t bwIdx = (cfg.radioBW >= 1 && cfg.radioBW <= 3) ? (cfg.radioBW - 1) : 0;
    s_rcCtx.bwDd = _makeCustomRadioDd(bwRow, "62.5 kHz\n125 kHz\n250 kHz", bwIdx);

    // CR (coding rate 5-8)
    lv_obj_t* crRow = _makeCustomRadioRow(panel, "CR");
    uint8_t crIdx = (cfg.radioCR >= 5 && cfg.radioCR <= 8) ? (cfg.radioCR - 5) : 0;
    s_rcCtx.crDd = _makeCustomRadioDd(crRow, "CR5\nCR6\nCR7\nCR8", crIdx);

    // suppress unused-variable warnings — rows exist as children of panel
    (void)sfRow; (void)bwRow; (void)crRow;

    // Button row
    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, LV_PCT(100), 32);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto makeBtn = [&](const char* lbl, lv_color_t col, lv_event_cb_t cb) -> lv_obj_t* {
        lv_obj_t* btn = lv_btn_create(btnRow);
        lv_obj_set_size(btn, 100, 28);
        lv_obj_set_style_bg_color(btn, col, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, theme::BORDER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(btn, _onCustomRadioKey, LV_EVENT_KEY, nullptr);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, lbl);
        lv_obj_set_style_text_color(l, theme::TEXT, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_center(l);
        return btn;
    };

    lv_obj_t* saveBtn   = makeBtn(LV_SYMBOL_OK " Save",      theme::PRIMARY, _onCustomRadioSave);
    lv_obj_t* cancelBtn = makeBtn(LV_SYMBOL_CLOSE " Cancel",  theme::BG_CARD, _onCustomRadioCancel);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, freqTa);
        lv_group_add_obj(g, s_rcCtx.sfDd);
        lv_group_add_obj(g, s_rcCtx.bwDd);
        lv_group_add_obj(g, s_rcCtx.crDd);
        lv_group_add_obj(g, saveBtn);
        lv_group_add_obj(g, cancelBtn);
        lv_group_focus_obj(freqTa);
    }
}

// ── Keyboard Layout dropdown dialog ──────────────────────────────────

struct KbLayoutCtx { lv_obj_t* modal; lv_obj_t* dd; };
static KbLayoutCtx s_kbLCtx;

static void _onKbLSave(lv_event_t* /*e*/) {
    uint8_t sel = (uint8_t)lv_dropdown_get_selected(s_kbLCtx.dd);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.kbLayout = sel < ops::keymap::LAYOUT_COUNT ? sel : 0;
    ops::config::save();
    lv_obj_del(s_kbLCtx.modal);
    ScreenSettings::show();
}
static void _onKbLExit(lv_event_t* /*e*/) { lv_obj_del(s_kbLCtx.modal); }
static void _onKbLKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) lv_obj_del(s_kbLCtx.modal);
}

// ── Language picker dialog ────────────────────────────────────────────

struct LangCtx { lv_obj_t* modal; };
static LangCtx s_langCtx;

static void _onLangRowClick(lv_event_t* e) {
    int* info = static_cast<int*>(lv_event_get_user_data(e));
    lv_obj_t* modal = reinterpret_cast<lv_obj_t*>((uintptr_t)info[0]);
    uint8_t    lang = (uint8_t)info[1];
    delete[] info;

    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.uiLanguage = lang;
    ops::config::save();
    OPS_LOG("Settings", "Language -> %d", lang);
    lv_obj_del(modal);
    ScreenSettings::show();
}

static void _onLangClose(lv_event_t* e) {
    lv_obj_t* modal = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_obj_del(modal);
}

static void _openLangDialog() {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_langCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, [](lv_event_t* ev) {
        uint32_t key = lv_event_get_key(ev);
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)
            lv_obj_del(static_cast<lv_obj_t*>(lv_event_get_user_data(ev)));
    }, LV_EVENT_KEY, modal);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 230, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, OPS_SCREEN_H - 4, 0);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 4, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, lang::tr(lang::TR_LANGUAGE));
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    uint8_t curLang = ops::config::get().uiLanguage;
    if (curLang >= lang::LANG_COUNT) curLang = 0;

    for (uint8_t i = 0; i < lang::LANG_COUNT; i++) {
        bool isSelected = (i == curLang);

        lv_obj_t* row = lv_btn_create(panel);
        lv_obj_set_size(row, 212, 28);
        lv_obj_set_style_bg_color(row, isSelected ? theme::PRIMARY : theme::BG, 0);
        lv_obj_set_style_bg_color(row, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, isSelected ? theme::ACCENT : theme::BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* nameLbl = lv_label_create(row);
        lv_label_set_text(nameLbl, lang::kLangNames[i]);
        lv_obj_set_style_text_color(nameLbl, isSelected ? theme::ACCENT : theme::TEXT, 0);
        lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_10, 0);

        int* info = new int[2];
        info[0] = (int)(uintptr_t)modal;
        info[1] = i;
        lv_obj_add_event_cb(row, _onLangRowClick, LV_EVENT_CLICKED, info);
        lv_obj_add_event_cb(row, [](lv_event_t* ev) {
            if (lv_event_get_key(ev) == LV_KEY_ESC)
                lv_obj_del(static_cast<lv_obj_t*>(lv_event_get_user_data(ev)));
        }, LV_EVENT_KEY, modal);
    }

    lv_obj_t* exitBtn = lv_btn_create(panel);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onLangClose, LV_EVENT_CLICKED, modal);
    lv_obj_add_event_cb(exitBtn, [](lv_event_t* ev) {
        if (lv_event_get_key(ev) == LV_KEY_ESC)
            lv_obj_del(static_cast<lv_obj_t*>(lv_event_get_user_data(ev)));
    }, LV_EVENT_KEY, modal);

    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* gL = lv_group_get_default();
    if (gL) {
        uint32_t nRows = lv_obj_get_child_cnt(panel);
        // skip the title label (child 0) and exit button (last child)
        for (uint32_t ci = 1; ci < nRows - 1; ci++)
            lv_group_add_obj(gL, lv_obj_get_child(panel, (int32_t)ci));
        lv_group_add_obj(gL, exitBtn);
        lv_obj_t* focusRow = lv_obj_get_child(panel, (int32_t)(curLang + 1));
        lv_group_focus_obj(focusRow);
    }
}

static void _openKbLayoutDialog() {
    const auto& cfg = ops::config::get();

    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_kbLCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onKbLKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, OPS_SCREEN_H - 4, 0);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Keyboard Layout");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t* hint = lv_label_create(panel);
    lv_label_set_text(hint, "Tip: press same letter twice quickly to cycle accented variants");
    lv_obj_set_style_text_color(hint, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_width(hint, 210);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    lv_obj_t* dd = lv_dropdown_create(panel);
    s_kbLCtx.dd = dd;
    lv_dropdown_set_options(dd, ops::keymap::kLayoutNames);
    lv_dropdown_set_selected(dd,
        cfg.kbLayout < ops::keymap::LAYOUT_COUNT ? cfg.kbLayout : 0);
    lv_obj_set_width(dd, 210);
    lv_obj_set_style_bg_color(dd, theme::BG, 0);
    lv_obj_set_style_text_color(dd, theme::TEXT, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(dd, theme::BORDER, 0);

    lv_obj_t* ddList = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(ddList, theme::BG_CARD, 0);
    lv_obj_set_style_text_color(ddList, theme::TEXT, 0);
    lv_obj_set_style_text_font(ddList, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(ddList, theme::BORDER, 0);

    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, 220, 32);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onKbLSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(saveBtn, _onKbLKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(btnRow);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onKbLExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onKbLKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, dd);
        lv_group_add_obj(g, saveBtn);
        lv_group_add_obj(g, exitBtn);
        lv_group_focus_obj(dd);
    }
}

// ── Volume slider dialog ──────────────────────────────────────────────
// Slider 0–100 (percent). Scales I2S samples at playback time.

struct VolCtx { lv_obj_t* modal; lv_obj_t* slider; lv_obj_t* valLbl; };
static VolCtx s_volCtx;

static void _onVolSlide(lv_event_t* /*e*/) {
    int v = (int)lv_slider_get_value(s_volCtx.slider);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(s_volCtx.valLbl, buf);
}
static void _onVolSave(lv_event_t* /*e*/) {
    int v = (int)lv_slider_get_value(s_volCtx.slider);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.speakerVolume = (uint8_t)v;
    ops::config::save();
    lv_obj_del(s_volCtx.modal);
    ScreenSettings::show();
}
static void _onVolExit(lv_event_t* /*e*/) { lv_obj_del(s_volCtx.modal); }
static void _onVolKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) lv_obj_del(s_volCtx.modal);
}

static void _openVolumeDialog() {
    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_volCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onVolKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 155);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, LV_SYMBOL_VOLUME_MAX " Volume");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    int curVol = (int)ops::config::get().speakerVolume;
    s_volCtx.valLbl = lv_label_create(panel);
    char initBuf[8];
    snprintf(initBuf, sizeof(initBuf), "%d%%", curVol);
    lv_label_set_text(s_volCtx.valLbl, initBuf);
    lv_obj_set_style_text_color(s_volCtx.valLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(s_volCtx.valLbl, &lv_font_montserrat_12, 0);

    s_volCtx.slider = lv_slider_create(panel);
    lv_obj_set_width(s_volCtx.slider, 220);
    lv_slider_set_range(s_volCtx.slider, 0, 100);
    lv_slider_set_value(s_volCtx.slider, curVol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_volCtx.slider, theme::PRIMARY,  LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_volCtx.slider, theme::ACCENT,   LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_volCtx.slider, theme::BORDER,   LV_PART_MAIN);
    lv_obj_add_event_cb(s_volCtx.slider, _onVolSlide, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, 220, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto mkBtn = [&](const char* lbl, lv_color_t col, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_btn_create(row);
        lv_group_remove_obj(btn);
        lv_obj_set_size(btn, 96, 26);
        lv_obj_set_style_bg_color(btn, theme::BG, 0);
        lv_obj_set_style_bg_color(btn, theme::PRIMARY, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, col, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(btn, _onVolKey, LV_EVENT_KEY, nullptr);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, lbl);
        lv_obj_set_style_text_color(l, col, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_center(l);
    };
    mkBtn("Cancel", theme::TEXT_MUTED, _onVolExit);
    mkBtn("Save",   theme::ACCENT,     _onVolSave);

    lv_group_t* g = lv_group_get_default();
    if (g) { lv_group_add_obj(g, s_volCtx.slider); lv_group_focus_obj(s_volCtx.slider); }
}

// ── Notification Sound dropdown dialog ───────────────────────────────

struct NotifSndCtx { lv_obj_t* modal; lv_obj_t* dd; };
static NotifSndCtx s_nsCtx;

static void _onNSDSave(lv_event_t* /*e*/) {
    uint8_t sel = (uint8_t)lv_dropdown_get_selected(s_nsCtx.dd);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.notifySoundChoice = sel < 4 ? sel : 0;
    ops::config::save();
    lv_obj_del(s_nsCtx.modal);
    ScreenSettings::show();
}
static void _onNSDPreview(lv_event_t* /*e*/) {
    uint8_t sel = (uint8_t)lv_dropdown_get_selected(s_nsCtx.dd);
    ops::sound::playPreview(sel);
}
static void _onNSDExit(lv_event_t* /*e*/) { lv_obj_del(s_nsCtx.modal); }
static void _onNSDKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) lv_obj_del(s_nsCtx.modal);
}

static void _openNotifSoundDialog() {
    const auto& cfg = ops::config::get();

    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_nsCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onNSDKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_width(panel, 240);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Notification Sound");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t* dd = lv_dropdown_create(panel);
    s_nsCtx.dd = dd;
    lv_dropdown_set_options(dd, "Default\nPluck\nClear\nWhoosh");
    lv_dropdown_set_selected(dd, cfg.notifySoundChoice < 4 ? cfg.notifySoundChoice : 0);
    lv_obj_set_width(dd, 210);
    lv_obj_set_style_bg_color(dd, theme::BG, 0);
    lv_obj_set_style_text_color(dd, theme::TEXT, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(dd, theme::BORDER, 0);

    // Style the dropdown list (opens downward over the modal)
    lv_obj_t* list = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(list, theme::BG_CARD, 0);
    lv_obj_set_style_text_color(list, theme::TEXT, 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(list, theme::BORDER, 0);

    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, 220, 32);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_style_pad_column(btnRow, 4, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Preview — plays the currently selected sound without saving
    lv_obj_t* prevBtn = lv_btn_create(btnRow);
    lv_obj_set_size(prevBtn, 64, 26);
    lv_obj_set_style_bg_color(prevBtn, theme::BG, 0);
    lv_obj_set_style_bg_color(prevBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(prevBtn, theme::ACCENT, 0);
    lv_obj_set_style_border_width(prevBtn, 1, 0);
    lv_obj_set_style_radius(prevBtn, 4, 0);
    lv_obj_set_style_shadow_width(prevBtn, 0, 0);
    lv_obj_add_event_cb(prevBtn, _onNSDPreview, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(prevBtn, _onNSDKey,     LV_EVENT_KEY,     nullptr);
    lv_obj_t* prevLbl = lv_label_create(prevBtn);
    lv_label_set_text(prevLbl, LV_SYMBOL_PLAY " Play");
    lv_obj_set_style_text_color(prevLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(prevLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(prevLbl);

    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, 64, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onNSDSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(saveBtn, _onNSDKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(btnRow);
    lv_obj_set_size(exitBtn, 64, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onNSDExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onNSDKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, dd);
        lv_group_add_obj(g, prevBtn);
        lv_group_add_obj(g, saveBtn);
        lv_group_add_obj(g, exitBtn);
        lv_group_focus_obj(dd);
    }
}

// ── CPU Governor dropdown dialog ──────────────────────────────────────

struct CpuGovCtx { lv_obj_t* modal; lv_obj_t* dd; };
static CpuGovCtx s_cgCtx;

static void _onCGSave(lv_event_t* /*e*/) {
    uint8_t sel = (uint8_t)lv_dropdown_get_selected(s_cgCtx.dd);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.cpuGovernor = sel < 4 ? sel : 2;
    ops::config::save();
    ops::ui::applyGovernorNow();
    lv_obj_del(s_cgCtx.modal);
    ScreenSettings::show();
}
static void _onCGExit(lv_event_t* /*e*/) { lv_obj_del(s_cgCtx.modal); }
static void _onCGKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) lv_obj_del(s_cgCtx.modal);
}

static void _openCpuGovDialog() {
    const auto& cfg = ops::config::get();

    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_cgCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onCGKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_size(panel, 240, 150);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "CPU Governor");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    // Hint row: show freq table so user knows what they're choosing
    lv_obj_t* hint = lv_label_create(panel);
    lv_label_set_text(hint,
        "Active / Saver / Off\n"
        "PowerSave: 40/40/40 MHz\n"
        "Medium:    80/40/40 MHz\n"
        "Normal:   240/80/80 MHz\n"
        "Turbo:  240/240/240 MHz");
    lv_obj_set_style_text_color(hint, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);

    lv_obj_t* dd = lv_dropdown_create(panel);
    s_cgCtx.dd = dd;
    lv_dropdown_set_options(dd, "Power Save\nMedium\nNormal\nTurbo");
    lv_dropdown_set_selected(dd, cfg.cpuGovernor < 4 ? cfg.cpuGovernor : 2);
    lv_obj_set_width(dd, 210);
    lv_obj_set_style_bg_color(dd, theme::BG, 0);
    lv_obj_set_style_text_color(dd, theme::TEXT, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(dd, theme::BORDER, 0);

    lv_obj_t* list = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(list, theme::BG_CARD, 0);
    lv_obj_set_style_text_color(list, theme::TEXT, 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(list, theme::BORDER, 0);

    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, 220, 32);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT,  0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onCGSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(saveBtn, _onCGKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(btnRow);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED,     LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onCGExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onCGKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, dd);
        lv_group_add_obj(g, saveBtn);
        lv_group_add_obj(g, exitBtn);
        lv_group_focus_obj(dd);
    }
}

static void _openBackupRestoreDialog();  // defined after _onItemClick
static void _openThemeDialog();          // defined after _onItemClick
static void _openFontDialog();           // defined after _onItemClick
static void _openGenIdDialog();          // defined after _onItemClick

// ── _onItemClick() ───────────────────────────────────────────────────
void ScreenSettings::_onItemClick(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());

    switch (idx) {
        case 0:  // Device Name → open modal
            _openNameDialog();
            return;

        case 24: {  // Share My Contact → QR popup
            uint8_t pubKey[32];
            ops::MeshService::instance().getSelfPubKey(pubKey);
            char hexBuf[65] = {};
            for (int i = 0; i < 32; i++)
                snprintf(hexBuf + i * 2, 3, "%02x", pubKey[i]);
            char encodedCs[64];
            _urlEncodeSettings(cfg.callsign, encodedCs, sizeof(encodedCs));
            char data[160];
            snprintf(data, sizeof(data),
                     "meshcore://contact/add?name=%s&public_key=%s&type=1",
                     encodedCs, hexBuf);
            char title[40];
            snprintf(title, sizeof(title), "My Contact: %s", cfg.callsign);
            showQrPopup(title, data);
            return;
        }

        case 1:  // Channels → channel list dialog
            _openChannelsDialog();
            return;

        case 7:  // Bluetooth
            cfg.bluetoothEnabled = !cfg.bluetoothEnabled;
            ops::config::save();
            if (cfg.bluetoothEnabled)
                ops::MeshService::instance().startCompanionBLE();
            else
                ops::MeshService::instance().stopCompanionBLE();
            break;

        case 8:  // Speaker
            cfg.speakerEnabled = !cfg.speakerEnabled;
            ops::config::save();
            break;

        case 9:  // GPS — cycle Off → Intermittent → On
            cfg.gpsMode = (cfg.gpsMode + 1) % 3;
            ops::config::save();
            ops::GpsMgr::instance().applyMode(cfg.gpsMode);
            break;

        case 21:  // Keyboard brightness → slider dialog
            _openKbBrightDialog();
            return;

        case 10:  // Date/Time → set time dialog
            _openSetTimeDialog();
            return;

        case 20:  // Timezone → slider dialog
            _openTimezoneDialog();
            return;

        case 6:   // Radio → profile picker
            _openRadioDialog();
            return;

        case 25:  // Power → TX power slider
            _openTxPowerDialog();
            return;

        case 26:  // Brightness → backlight dimmer
            _openBrightnessDialog();
            return;

        case 11:  // Firmware Update — placeholder
            OPS_LOG("Settings", "Item %d: not yet implemented", idx);
            return;

        case 12:  // Auto Add Contacts → modal dialog
            _openAutoAddDialog();
            return;

        case 13:  // Screen Timeout → slider dialog
            _openTimeoutDialog();
            return;

        case 27:  // Screen Off → slider dialog
            _openScreenOffDialog();
            return;

        case 32:  // Volume → slider dialog
            _openVolumeDialog();
            return;

        case 16:  // Notifications → dialog
            _openNotificationsDialog();
            return;

        case 22:  // Notification Sound → dropdown dialog
            _openNotifSoundDialog();
            return;

        case 23:  // Keyboard Layout → dropdown dialog
            _openKbLayoutDialog();
            return;

        case 36:  // Language → picker dialog
            _openLangDialog();
            return;

        case 17:  // Location Sharing — direct toggle
            cfg.locationSharing = !cfg.locationSharing;
            ops::config::save();
            break;

        case 14:  // Save Messages — simple toggle
            cfg.saveMsgs = !cfg.saveMsgs;
            ops::config::save();
            break;

        case 15:  // Show Hops — simple toggle
            cfg.showHops = !cfg.showHops;
            ops::config::save();
            break;

        case 19:  // Show RSSI — simple toggle
            cfg.showRssi = !cfg.showRssi;
            ops::config::save();
            break;

        case 28:  // LoRa Duty Cycle — direct toggle
            cfg.loraDutyCycle = !cfg.loraDutyCycle;
            ops::config::save();
            break;

        case 29:  // CPU Governor → dropdown dialog
            _openCpuGovDialog();
            return;

        case 30:  // Backup & Restore
            _openBackupRestoreDialog();
            return;

        case 35:  // Generate Identity
            _openGenIdDialog();
            return;

        case 31:  // Theme
            _openThemeDialog();
            return;

        case 33:  // Font → dropdown dialog
            _openFontDialog();
            return;

        case 34: {  // Return to Launcher
            // Tell Launcher not to auto-relaunch us (covers factory/test Launcher slots
            // that always boot first and decide via this NVS flag).
            {
                Preferences prefs;
                prefs.begin("launcher", false);
                prefs.putBool("bootToApp", false);
                prefs.end();
            }
            // Also try OTA boot selection for Launcher builds where it lives in an OTA slot.
            const esp_partition_t* running = esp_ota_get_running_partition();
            esp_partition_iterator_t it = esp_partition_find(
                ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
            while (it) {
                const esp_partition_t* p = esp_partition_get(it);
                if (p && (!running || p->address != running->address)) {
                    esp_ota_set_boot_partition(p);  // ignore error — NVS flag is the primary path
                    break;
                }
                it = esp_partition_next(it);
            }
            esp_partition_iterator_release(it);
            esp_restart();
            return;
        }

        default: return;
    }

    _refreshList();
}

// ── Backup & Restore dialog ───────────────────────────────────────────
// Copies /oms/*.json  →  /oms/*.bak  (backup)
// ── Generate Identity dialog ─────────────────────────────────────────────────
// Generates a fresh keypair, saves to LittleFS/NVS/SD, then restarts.

static lv_obj_t* s_genIdModal = nullptr;

static void _genIdClose(lv_event_t* /*e*/)
{
    if (s_genIdModal) { lv_obj_del_async(s_genIdModal); s_genIdModal = nullptr; }
}

static void _genIdConfirm(lv_event_t* e)
{
    if (s_genIdModal) {
        lv_obj_t* statusLbl = (lv_obj_t*)lv_event_get_user_data(e);
        if (statusLbl) lv_label_set_text(statusLbl, "Generating... rebooting");
        lv_task_handler();
    }
    ops::MeshService::instance().regenerateIdentity();
    delay(1000);
    ESP.restart();
}

static void _openGenIdDialog()
{
    if (s_genIdModal) return;
    s_genIdModal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_genIdModal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_genIdModal, 0, 0);
    lv_obj_set_style_bg_color(s_genIdModal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_genIdModal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_genIdModal, 0, 0);
    lv_obj_set_style_pad_all(s_genIdModal, 0, 0);
    lv_obj_clear_flag(s_genIdModal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(s_genIdModal);
    lv_obj_set_width(panel, 260);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Generate Identity");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    // Show current pub key prefix so user knows what they're replacing
    {
        uint8_t pub[32] = {};
        ops::MeshService::instance().getSelfPubKey(pub);
        bool hasKey = false;
        for (int i = 0; i < 32; i++) if (pub[i]) { hasKey = true; break; }
        char curIdBuf[48] = {};
        if (hasKey) {
            snprintf(curIdBuf, sizeof(curIdBuf),
                     "Current: %02X%02X%02X%02X...",
                     pub[0], pub[1], pub[2], pub[3]);
        } else {
            snprintf(curIdBuf, sizeof(curIdBuf), "Current: (none)");
        }
        lv_obj_t* curLbl = lv_label_create(panel);
        lv_label_set_text(curLbl, curIdBuf);
        lv_obj_set_style_text_color(curLbl, theme::ACCENT, 0);
        lv_obj_set_style_text_font(curLbl, &lv_font_montserrat_10, 0);
    }

    lv_obj_t* warn = lv_label_create(panel);
    lv_label_set_text(warn,
        "This creates a new keypair.\n"
        "Existing DM threads will be lost.");
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, 236);
    lv_obj_set_style_text_color(warn, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_10, 0);

    lv_obj_t* statusLbl = lv_label_create(panel);
    lv_label_set_text(statusLbl, "");
    lv_obj_set_style_text_color(statusLbl, theme::GREEN, 0);
    lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_10, 0);

    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, 236, 32);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_style_pad_column(btnRow, 8, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto makeBtn = [&](const char* label, lv_color_t bg, lv_event_cb_t cb, void* ud) {
        lv_obj_t* btn = lv_btn_create(btnRow);
        lv_obj_set_size(btn, 108, 26);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
        lv_group_remove_obj(btn);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
    };

    makeBtn("Generate", lv_color_make(160, 30, 30), _genIdConfirm, statusLbl);
    makeBtn("Cancel",   theme::BG,                  _genIdClose,   nullptr);

    lv_obj_t* cancelBtn = lv_obj_get_child(btnRow, 1);
    lv_obj_set_style_border_color(cancelBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(cancelBtn, 1, 0);
}

// ── Backup & Restore dialog ──────────────────────────────────────────────────
// Copies /oms/*.bak   →  /oms/*.json (restore)
// Files: contacts, repeaters, settings (identity.bin is already auto-backed up)

static lv_obj_t* s_brModal   = nullptr;
static lv_obj_t* s_brStatus  = nullptr;

static void _brClose(lv_event_t* /*e*/)
{
    if (s_brModal) { lv_obj_del_async(s_brModal); s_brModal = nullptr; }
}

static bool _copyFile(const char* src, const char* dst)
{
    File in = SD.open(src, FILE_READ);
    if (!in) return false;
    SD.remove(dst);  // FILE_WRITE appends; remove first to get a clean overwrite
    File out = SD.open(dst, FILE_WRITE);
    if (!out) { in.close(); return false; }
    uint8_t buf[512];
    while (true) {
        int n = (int)in.read(buf, sizeof(buf));
        if (n <= 0) break;
        out.write(buf, (size_t)n);
    }
    in.close();
    out.close();
    return true;
}

static const char* _runBackup()
{
    if (!ops::sdcard::isMounted()) {
        if (!ops::sdcard::tryMount()) return "SD not mounted";
    }
    // Flush in-memory state to SD JSON before copying to .bak
    ops::config::save();
    ops::contacts::save();
    ops::repeaters::save();
    static const char* kSrc[] = {
        "/ops/contacts.json", "/ops/repeaters.json", "/ops/settings.json"
    };
    static const char* kDst[] = {
        "/ops/contacts.bak",  "/ops/repeaters.bak",  "/ops/settings.bak"
    };
    int ok = 0;
    for (int i = 0; i < 3; i++) {
        if (SD.exists(kSrc[i]))
            ok += _copyFile(kSrc[i], kDst[i]) ? 1 : 0;
    }
    // Also backup identity.bin → identity.bak
    if (SD.exists("/ops/identity.bin"))
        _copyFile("/ops/identity.bin", "/ops/identity.bak");
    if (ok == 0) return "No files to back up";
    static char msg[32];
    snprintf(msg, sizeof(msg), "Backed up %d file(s)", ok);
    return msg;
}

static const char* _runRestore()
{
    if (!ops::sdcard::isMounted()) {
        if (!ops::sdcard::tryMount()) return "SD not mounted";
    }
    static const char* kSrc[] = {
        "/ops/contacts.bak",  "/ops/repeaters.bak",  "/ops/settings.bak"
    };
    static const char* kDst[] = {
        "/ops/contacts.json", "/ops/repeaters.json", "/ops/settings.json"
    };
    int ok = 0;
    for (int i = 0; i < 3; i++) {
        if (SD.exists(kSrc[i]))
            ok += _copyFile(kSrc[i], kDst[i]) ? 1 : 0;
    }
    if (SD.exists("/ops/identity.bak")) {
        _copyFile("/ops/identity.bak", "/ops/identity.bin");
        // Identity restore requires clearing both LittleFS and NVS copies — both
        // take priority over SD in the boot-time load chain. SD wins only if both are absent.
        LittleFS.begin(false);
        LittleFS.remove("/mesh/self.id");
        Preferences idPrefs;
        if (idPrefs.begin("opsMesh", false)) { idPrefs.remove("selfId"); idPrefs.end(); }
    }
    if (ok == 0) return "No .bak files found";
    static char msg[40];
    snprintf(msg, sizeof(msg), "Restored %d file(s)", ok);
    return msg;
}

static void _onBRBackup(lv_event_t* /*e*/)
{
    const char* result = _runBackup();
    OPS_LOG("Backup", "%s", result);
    if (s_brStatus) lv_label_set_text(s_brStatus, result);
}

static void _onBRRestore(lv_event_t* /*e*/)
{
    const char* result = _runRestore();
    OPS_LOG("Backup", "%s", result);
    if (s_brStatus) lv_label_set_text(s_brStatus, result);
    ops::config::reloadFromSD();
    ops::contacts::reloadFromSD();
    ops::repeaters::reloadFromSD();
    if (s_brStatus) lv_label_set_text(s_brStatus, "Restored — rebooting...");
    lv_task_handler();
    delay(2500);
    ESP.restart();
}

static void _openBackupRestoreDialog()
{
    if (s_brModal) return;

    s_brModal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_brModal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_set_pos(s_brModal, 0, 0);
    lv_obj_set_style_bg_color(s_brModal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_brModal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_brModal, 0, 0);
    lv_obj_set_style_pad_all(s_brModal, 0, 0);
    lv_obj_clear_flag(s_brModal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(s_brModal);
    lv_obj_set_width(panel, 260);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Backup & Restore");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t* sub = lv_label_create(panel);
    lv_label_set_text(sub, "contacts / repeaters / settings");
    lv_obj_set_style_text_color(sub, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_10, 0);

    // Status line
    s_brStatus = lv_label_create(panel);
    lv_label_set_text(s_brStatus, "");
    lv_label_set_long_mode(s_brStatus, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_brStatus, 236);
    lv_obj_set_style_text_color(s_brStatus, theme::GREEN, 0);
    lv_obj_set_style_text_font(s_brStatus, &lv_font_montserrat_10, 0);

    // Button row
    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, 236, 32);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_style_pad_column(btnRow, 4, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto makeBtn = [&](const char* label, lv_color_t bg, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_btn_create(btnRow);
        lv_obj_set_size(btn, 72, 26);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_bg_color(btn, theme::ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_group_remove_obj(btn);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, theme::TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_center(lbl);
    };

    makeBtn("Backup",  theme::PRIMARY, _onBRBackup);
    makeBtn("Restore", theme::ORANGE,  _onBRRestore);
    makeBtn("Close",   theme::BG,      _brClose);

    // Style Close with border
    lv_obj_t* closeBtn = lv_obj_get_child(btnRow, 2);
    lv_obj_set_style_border_color(closeBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(closeBtn, 1, 0);
}

// ── Theme dialog ─────────────────────────────────────────────────────
// Saves cfg.theme and restarts so all LVGL objects redraw with new colours.

struct ThemeCtx { lv_obj_t* modal; lv_obj_t* dd; lv_obj_t* note; };
static ThemeCtx s_thCtx;

static void _onThemeSave(lv_event_t* /*e*/) {
    int sel = (int)lv_dropdown_get_selected(s_thCtx.dd);
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    if (sel != cfg.theme) {
        cfg.theme = sel;
        ops::config::save();
        if (s_thCtx.note) lv_label_set_text(s_thCtx.note, "Restarting...");
        lv_task_handler();
        delay(400);
        ESP.restart();
    }
    lv_obj_del(s_thCtx.modal);
}
static void _onThemeExit(lv_event_t* /*e*/) { lv_obj_del(s_thCtx.modal); }
static void _onThemeKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) lv_obj_del(s_thCtx.modal);
}

static void _openThemeDialog() {
    const auto& cfg = ops::config::get();

    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_thCtx.modal = modal;
    s_thCtx.note  = nullptr;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onThemeKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_width(panel, 240);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Theme");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t* dd = lv_dropdown_create(panel);
    s_thCtx.dd = dd;
    lv_dropdown_set_options(dd,
        "Default\nGreen\nDracula\nTokyo Night\n"
        "Catp Frappe\nCatp Mocha\nSynthwave 84\n"
        "Kaolin\nOne Dark\nNeovim\nNyx\nRat Dark");
    int cur = (cfg.theme >= 0 && cfg.theme < theme::THEME_COUNT) ? cfg.theme : 0;
    lv_dropdown_set_selected(dd, (uint16_t)cur);
    lv_obj_set_width(dd, 210);
    lv_obj_set_style_bg_color(dd, theme::BG, 0);
    lv_obj_set_style_text_color(dd, theme::TEXT, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(dd, theme::BORDER, 0);

    lv_obj_t* ddList = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(ddList, theme::BG_CARD, 0);
    lv_obj_set_style_text_color(ddList, theme::TEXT, 0);
    lv_obj_set_style_text_font(ddList, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(ddList, theme::BORDER, 0);

    s_thCtx.note = lv_label_create(panel);
    lv_label_set_text(s_thCtx.note, "Device restarts on save.");
    lv_obj_set_style_text_color(s_thCtx.note, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_thCtx.note, &lv_font_montserrat_10, 0);

    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, 220, 32);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Save — ACCENT background (bright, positive action; works across all themes)
    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT, 0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(saveBtn, 0, 0);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onThemeSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(saveBtn, _onThemeKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    // Cancel — muted card background
    lv_obj_t* exitBtn = lv_btn_create(btnRow);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onThemeExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onThemeKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Cancel");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, dd);
        lv_group_add_obj(g, saveBtn);
        lv_group_add_obj(g, exitBtn);
        lv_group_focus_obj(dd);
    }
}

// ── Font dialog ──────────────────────────────────────────────────────
// Chooses the body font used for chat messages.  Standard is the LVGL
// built-in Montserrat; Extended Latin adds accented glyphs (U+00A0-U+017E)
// for European languages.  Emoji work with either (global imgfont fallback).

struct FontCtx { lv_obj_t* modal; lv_obj_t* dd; lv_obj_t* preview; };
static FontCtx s_fontCtx;

static void _fontPreviewUpdate() {
    if (!s_fontCtx.preview || !s_fontCtx.dd) return;
    bool ext = lv_dropdown_get_selected(s_fontCtx.dd) == 1;
    lv_obj_set_style_text_font(s_fontCtx.preview,
        ops::emoji::emojiFont(ext ? &font_montserrat_12_ext : &lv_font_montserrat_12), 0);
}

static void _onFontDDChanged(lv_event_t* /*e*/) { _fontPreviewUpdate(); }

static void _onFontSave(lv_event_t* /*e*/) {
    auto& cfg = const_cast<ops::Config&>(ops::config::get());
    cfg.fontExtLatin = (lv_dropdown_get_selected(s_fontCtx.dd) == 1);
    ops::config::save();
    lv_obj_del(s_fontCtx.modal);
    ScreenSettings::show();
}
static void _onFontExit(lv_event_t* /*e*/) { lv_obj_del(s_fontCtx.modal); }
static void _onFontKey(lv_event_t* e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) lv_obj_del(s_fontCtx.modal);
}

static void _openFontDialog() {
    const auto& cfg = ops::config::get();

    lv_obj_t* modal = lv_obj_create(lv_scr_act());
    s_fontCtx.modal = modal;
    lv_obj_set_size(modal, OPS_SCREEN_W, OPS_SCREEN_H);
    lv_obj_align(modal, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, _onFontKey, LV_EVENT_KEY, nullptr);

    lv_obj_t* panel = lv_obj_create(modal);
    lv_obj_set_width(panel, 240);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, theme::BG_CARD, 0);
    lv_obj_set_style_border_color(panel, theme::BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "Font");
    lv_obj_set_style_text_color(title, theme::ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    lv_obj_t* dd = lv_dropdown_create(panel);
    s_fontCtx.dd = dd;
    lv_dropdown_set_options(dd, "Standard\nExtended Latin");
    lv_dropdown_set_selected(dd, cfg.fontExtLatin ? 1 : 0);
    lv_obj_set_width(dd, 210);
    lv_obj_set_style_bg_color(dd, theme::BG, 0);
    lv_obj_set_style_text_color(dd, theme::TEXT, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(dd, theme::BORDER, 0);
    lv_obj_add_event_cb(dd, _onFontDDChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* ddList = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(ddList, theme::BG_CARD, 0);
    lv_obj_set_style_text_color(ddList, theme::TEXT, 0);
    lv_obj_set_style_text_font(ddList, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(ddList, theme::BORDER, 0);

    // Live preview: accented Latin + an emoji (renders via global fallback)
    lv_obj_t* preview = lv_label_create(panel);
    s_fontCtx.preview = preview;
    lv_label_set_text(preview, "Café Żółć \xF0\x9F\x98\x80");  // ...😀
    lv_obj_set_style_text_color(preview, theme::TEXT, 0);
    _fontPreviewUpdate();

    lv_obj_t* note = lv_label_create(panel);
    lv_label_set_text(note, "Extended Latin adds accents.\nApplies to chats on reopen.");
    lv_obj_set_style_text_color(note, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_10, 0);

    lv_obj_t* btnRow = lv_obj_create(panel);
    lv_obj_set_size(btnRow, 220, 32);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow,
        LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, 90, 26);
    lv_obj_set_style_bg_color(saveBtn, theme::ACCENT, 0);
    lv_obj_set_style_bg_color(saveBtn, theme::PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(saveBtn, 0, 0);
    lv_obj_set_style_radius(saveBtn, 4, 0);
    lv_obj_set_style_shadow_width(saveBtn, 0, 0);
    lv_obj_add_event_cb(saveBtn, _onFontSave, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(saveBtn, _onFontKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_OK " Save");
    lv_obj_set_style_text_color(saveLbl, theme::BG, 0);
    lv_obj_set_style_text_font(saveLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(saveLbl);

    lv_obj_t* exitBtn = lv_btn_create(btnRow);
    lv_obj_set_size(exitBtn, 90, 26);
    lv_obj_set_style_bg_color(exitBtn, theme::BG_CARD, 0);
    lv_obj_set_style_bg_color(exitBtn, theme::RED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(exitBtn, theme::BORDER, 0);
    lv_obj_set_style_border_width(exitBtn, 1, 0);
    lv_obj_set_style_radius(exitBtn, 4, 0);
    lv_obj_set_style_shadow_width(exitBtn, 0, 0);
    lv_obj_add_event_cb(exitBtn, _onFontExit, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(exitBtn, _onFontKey,  LV_EVENT_KEY,     nullptr);
    lv_obj_t* exitLbl = lv_label_create(exitBtn);
    lv_label_set_text(exitLbl, LV_SYMBOL_CLOSE " Cancel");
    lv_obj_set_style_text_color(exitLbl, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(exitLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(exitLbl);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, dd);
        lv_group_add_obj(g, saveBtn);
        lv_group_add_obj(g, exitBtn);
        lv_group_focus_obj(dd);
    }
}

// ── _onHomeClick() ───────────────────────────────────────────────────
void ScreenSettings::_onHomeClick(lv_event_t* /*e*/) {
    ScreenLauncher::show();
}

}}  // namespace ops::ui
