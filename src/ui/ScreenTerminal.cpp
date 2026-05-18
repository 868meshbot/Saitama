// Saitama — ScreenTerminal.cpp
// Copyright 2026 Saitama — MIT License
//
// Terminal screen layout (320 x 240 landscape):
//
//   ┌──────────────────────────────────────┐  y = 0
//   │ [⌂ Home]    Terminal          12:34 │  top bar  28 px
//   ├──────────────────────────────────────┤  y = 28
//   │                                      │
//   │   scrollable log (green on dark)     │  156 px
//   │                                      │
//   ├──────────────────────────────────────┤  y = 184
//   │ > [_input___________________] [Send] │  input    44 px
//   │ Type /help for help, ENTER to run…  │  hint     12 px
//   └──────────────────────────────────────┘  y = 240

#include "ScreenTerminal.h"
#include "ScreenLauncher.h"
#include "Theme.h"
#include "../mesh/MeshService.h"
#include "../utils/Config.h"
#include "../utils/Contacts.h"
#include "../utils/Repeaters.h"
#include "../utils/SDCard.h"
#include "../utils/Log.h"
#include "../hardware/Board.h"
#include "../version.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <time.h>
#include <sys/time.h>
#include <esp_heap_caps.h>
#include <Wire.h>
#include <LittleFS.h>

namespace ops { namespace ui {

static constexpr int TOP_H   = 28;
static constexpr int INPUT_H = 56;   // 44 input row + 12 hint row
static constexpr int LOG_H   = OPS_SCREEN_H - TOP_H - INPUT_H;  // 156 px

lv_obj_t* ScreenTerminal::_screen    = nullptr;
lv_obj_t* ScreenTerminal::_logScroll = nullptr;
lv_obj_t* ScreenTerminal::_logLabel  = nullptr;
lv_obj_t* ScreenTerminal::_input     = nullptr;
char      ScreenTerminal::_logBuf[3072] = "Saitama Terminal - type /help\n";
int       ScreenTerminal::_logLen    = (int)strlen("Saitama Terminal - type /help\n");
char      ScreenTerminal::_serialBuf[256] = {};
int       ScreenTerminal::_serialLen = 0;
char      ScreenTerminal::s_dmTarget[32]   = {};
uint8_t   ScreenTerminal::s_dmTargetKey[4] = {};
char      ScreenTerminal::s_adminName[32]  = {};
uint8_t   ScreenTerminal::s_adminKey[4]    = {};

// ── show() ───────────────────────────────────────────────────────────
void ScreenTerminal::show() {
    if (!_screen) {
        _screen = lv_obj_create(nullptr);
        lv_obj_set_size(_screen, OPS_SCREEN_W, OPS_SCREEN_H);
        lv_obj_set_style_bg_color(_screen, theme::BG, 0);
        lv_obj_set_style_pad_all(_screen, 0, 0);
        lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);

        _buildTopBar(_screen);
        _buildLog(_screen);
        _buildInput(_screen);
    }

    lv_scr_load(_screen);
    _scrollToBottom();
    OPS_LOG("UI", "Terminal shown");
}

// ── appendLine() ─────────────────────────────────────────────────────
void ScreenTerminal::appendLine(const char* line) {
    // Echo to CDC serial so the serial console sees all responses
    Serial.println(line);

    int remaining = (int)sizeof(_logBuf) - _logLen - 1;
    if (remaining < 2) {
        int half = sizeof(_logBuf) / 2;
        memmove(_logBuf, _logBuf + half, sizeof(_logBuf) - half);
        _logLen -= half;
        if (_logLen < 0) _logLen = 0;
        _logBuf[_logLen] = '\0';
        remaining = (int)sizeof(_logBuf) - _logLen - 1;
    }
    int n = snprintf(_logBuf + _logLen, remaining, "%s\n", line);
    if (n > 0) _logLen += n;

    if (_logLabel) {
        lv_label_set_text(_logLabel, _logBuf);
        _scrollToBottom();
    }
}

// ── _buildTopBar() ───────────────────────────────────────────────────
void ScreenTerminal::_buildTopBar(lv_obj_t* parent) {
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

    lv_obj_t* homeLbl = lv_label_create(homeBtn);
    lv_label_set_text(homeLbl, LV_SYMBOL_HOME " Home");
    lv_obj_set_style_text_color(homeLbl, theme::ACCENT, 0);
    lv_obj_set_style_text_font(homeLbl, &lv_font_montserrat_10, 0);
    lv_obj_center(homeLbl);

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, LV_SYMBOL_KEYBOARD " Terminal");
    lv_obj_set_style_text_color(title, theme::TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
}

// ── _buildLog() ──────────────────────────────────────────────────────
void ScreenTerminal::_buildLog(lv_obj_t* parent) {
    _logScroll = lv_obj_create(parent);
    lv_obj_set_size(_logScroll, OPS_SCREEN_W, LOG_H);
    lv_obj_align(_logScroll, LV_ALIGN_TOP_LEFT, 0, TOP_H);
    lv_obj_set_style_bg_color(_logScroll, theme::BG, 0);
    lv_obj_set_style_border_width(_logScroll, 0, 0);
    lv_obj_set_style_pad_all(_logScroll, 4, 0);
    lv_obj_set_scrollbar_mode(_logScroll, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(_logScroll, LV_DIR_VER);

    _logLabel = lv_label_create(_logScroll);
    lv_label_set_long_mode(_logLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_logLabel, OPS_SCREEN_W - 8);
    lv_obj_set_style_text_color(_logLabel, theme::GREEN, 0);
    lv_obj_set_style_text_font(_logLabel, &lv_font_montserrat_10, 0);
    lv_label_set_text(_logLabel, _logBuf);
}

// ESC on input or send button exits to the launcher
static void _onTermKey(lv_event_t* e) {
    if (lv_event_get_key(e) == LV_KEY_ESC) ScreenLauncher::show();
}

// ── _buildInput() ────────────────────────────────────────────────────
void ScreenTerminal::_buildInput(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, OPS_SCREEN_W, INPUT_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::BG_CARD, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_set_style_pad_top(bar, 4, 0);
    lv_obj_set_style_pad_bottom(bar, 2, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Prompt ">"
    lv_obj_t* prompt = lv_label_create(bar);
    lv_label_set_text(prompt, ">");
    lv_obj_set_style_text_color(prompt, theme::ACCENT, 0);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_12, 0);
    lv_obj_align(prompt, LV_ALIGN_TOP_LEFT, 0, 6);

    // Send button
    lv_obj_t* sendBtn = lv_btn_create(bar);
    lv_obj_set_size(sendBtn, 44, 32);
    lv_obj_align(sendBtn, LV_ALIGN_TOP_RIGHT, 0, 2);
    lv_obj_set_style_bg_color(sendBtn, theme::PRIMARY, 0);
    lv_obj_set_style_bg_color(sendBtn, theme::ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(sendBtn, 4, 0);
    lv_obj_set_style_shadow_width(sendBtn, 0, 0);
    lv_obj_add_event_cb(sendBtn, _onSend, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* sendLbl = lv_label_create(sendBtn);
    lv_label_set_text(sendLbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(sendLbl, theme::TEXT, 0);
    lv_obj_center(sendLbl);

    // Textarea
    _input = lv_textarea_create(bar);
    lv_obj_set_size(_input, OPS_SCREEN_W - 18 - 50, 32);
    lv_obj_align(_input, LV_ALIGN_TOP_LEFT, 14, 2);
    lv_obj_set_style_bg_color(_input, theme::BG, 0);
    lv_obj_set_style_text_color(_input, theme::TEXT, 0);
    lv_obj_set_style_border_color(_input, theme::BORDER, 0);
    lv_obj_set_style_border_color(_input, theme::ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(_input, &lv_font_montserrat_10, 0);
    lv_textarea_set_placeholder_text(_input, "/help");
    lv_textarea_set_one_line(_input, true);

    // Hint line
    lv_obj_t* hint = lv_label_create(bar);
    lv_label_set_text(hint, "Type /help for help  |  ENTER to run  |  ESC to exit");
    lv_obj_set_style_text_color(hint, theme::TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -1);

    // ENTER on textarea → send command; ESC on either widget → launcher
    lv_obj_add_event_cb(_input, _onSend,    LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(_input, _onTermKey, LV_EVENT_KEY,   nullptr);
    lv_obj_add_event_cb(sendBtn, _onTermKey, LV_EVENT_KEY,  nullptr);

    lv_group_t* g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, _input);
        lv_group_add_obj(g, sendBtn);
        lv_group_focus_obj(_input);
    }
}

// ── _scrollToBottom() ────────────────────────────────────────────────
void ScreenTerminal::_scrollToBottom() {
    if (!_logScroll) return;
    lv_obj_scroll_to_y(_logScroll, LV_COORD_MAX, LV_ANIM_OFF);
}

// ── _onHomeClick() ───────────────────────────────────────────────────
void ScreenTerminal::_onHomeClick(lv_event_t* /*e*/) {
    ScreenLauncher::show();
}

// ── _onSend() ────────────────────────────────────────────────────────
void ScreenTerminal::_onSend(lv_event_t* /*e*/) {
    if (!_input) return;
    const char* txt = lv_textarea_get_text(_input);
    if (!txt || txt[0] == '\0') return;

    char line[256];
    snprintf(line, sizeof(line), "> %s", txt);
    appendLine(line);
    _dispatch(txt);
    lv_textarea_set_text(_input, "");
}

// Minimal JSON string-field extractor for message log lines (no heap, no ArduinoJson).
// Handles simple \" and \\ escapes.
static bool _extractJsonStr(const char* json, const char* key, char* out, int outMax)
{
    char needle[12];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    int i = 0;
    for (; *p && *p != '"' && i < outMax - 1; p++) {
        if (*p == '\\' && *(p + 1)) { p++; }
        out[i++] = *p;
    }
    out[i] = '\0';
    return true;
}

// ── _dispatch() ──────────────────────────────────────────────────────
// Parses the user input (with or without leading /) and routes to handlers.
// Self-contained commands run immediately; mesh commands print [mesh not connected].

void ScreenTerminal::_dispatch(const char* raw) {
    // Strip optional leading '/'
    const char* input = (raw[0] == '/') ? raw + 1 : raw;

    // Tokenise: cmd + remainder
    char cmd[32] = {};
    const char* args = "";
    const char* sp = strchr(input, ' ');
    if (sp) {
        size_t len = (size_t)(sp - input);
        if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
        memcpy(cmd, input, len);
        args = sp + 1;
        while (*args == ' ') args++;  // skip extra spaces
    } else {
        strncpy(cmd, input, sizeof(cmd) - 1);
    }

    // ── help ──────────────────────────────────────────────────────────
    if (strcmp(cmd, "help") == 0) {
        appendLine("Commands (prefix with /):");
        appendLine("  /set {name|lat|lon|freq|sf|bw|tx|cr|af} {value}");
        appendLine("  /get radio");
        appendLine("  /card");
        appendLine("  /import {biz card}");
        appendLine("  /clock");
        appendLine("  /time <epoch-seconds>");
        appendLine("  /memory");
        appendLine("  /battery");
        appendLine("  /sd  |  /sd mount  |  /sd restore  |  /sd ls [path]");
        appendLine("  /identity restore");
        appendLine("  /list {n}  |  /messages {n|all}  |  /clearmessages");
        appendLine("  /clearcontacts  |  /contacts delete <prefix>");
        appendLine("  /find <id>  |  /to <name|prefix>  |  /send <text>");
        appendLine("  /advert [flood]");
        appendLine("  /trace <path>  |  /getpath <hex>  |  /setpath <hex> <path>");
        appendLine("  /reset path  |  /resetpath <hex>");
        appendLine("  /public <text>  |  /local <text>");
        appendLine("  /ch3 <text>  |  /ch4 <text>  |  /ch5 <text>");
        appendLine("  /setchannel <1-5> #<name> [psk]");
        appendLine("  /channel <1-5>  |  /channels");
        appendLine("  /adverts  |  /quiet");
        appendLine("  /mobrep  |  /autorep  |  /replist  |  /clearrep");
        appendLine("  /scope #<region>  |  /scope clear  |  /scope");
        appendLine("  /pathsize [1|2]  - 1=1-byte hashes, 2=2-byte hashes");
        appendLine("  /uizoom [10|12|13|15]  |  /uifont [0|1]");
        appendLine("  /control <hex>");
        appendLine("  /repeaters [secs]  |  /repeateradmin <hex> [pass]");
        appendLine("  /repadmin [<hex>] <cmd>  - send admin cmd (login first)");
        appendLine("  /gps [on|off]  - status if no arg");
        appendLine("  /i2c scan");
        appendLine("  /kbbl <0-255>  - probe keyboard backlight protocols");
        appendLine("  /tbdebug on|off  - trackball ISR log to CDC serial");
        return;
    }

    // ── clock ─────────────────────────────────────────────────────────
    if (strcmp(cmd, "clock") == 0) {
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        char buf[48];
        snprintf(buf, sizeof(buf), "Time: %04d-%02d-%02d %02d:%02d:%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
        appendLine(buf);
        return;
    }

    // ── time <epoch> ──────────────────────────────────────────────────
    if (strcmp(cmd, "time") == 0) {
        if (args[0] == '\0') { appendLine("Usage: /time <epoch-seconds>"); return; }
        time_t ts = (time_t)atol(args);
        struct timeval tv = { ts, 0 };
        settimeofday(&tv, nullptr);
        appendLine("RTC updated.");
        ScreenLauncher::refreshClock();
        return;
    }

    // ── memory ────────────────────────────────────────────────────────
    if (strcmp(cmd, "memory") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Free heap:  %6lu B", (unsigned long)esp_get_free_heap_size());
        appendLine(buf);
        snprintf(buf, sizeof(buf), "Min heap:   %6lu B", (unsigned long)esp_get_minimum_free_heap_size());
        appendLine(buf);
        snprintf(buf, sizeof(buf), "Free PSRAM: %6lu B", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        appendLine(buf);
        return;
    }

    // ── battery ───────────────────────────────────────────────────────
    if (strcmp(cmd, "battery") == 0) {
        auto& b = Board::instance();
        int pct = b.batteryPercent();
        bool chg = b.batteryCharging();
        char buf[48];
        snprintf(buf, sizeof(buf), "Battery: %d%%%s", pct, chg ? " (charging)" : "");
        appendLine(buf);
        return;
    }

    // ── tbdebug ───────────────────────────────────────────────────────
    if (strcmp(cmd, "tbdebug") == 0) {
        if (strcmp(args, "on") == 0) {
            Board::trackballDebug = true;
            appendLine("Trackball ISR debug: ON  (watch CDC serial)");
        } else if (strcmp(args, "off") == 0) {
            Board::trackballDebug = false;
            appendLine("Trackball ISR debug: OFF");
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "Trackball ISR debug: %s  (usage: /tbdebug on|off)",
                     Board::trackballDebug ? "ON" : "OFF");
            appendLine(buf);
        }
        return;
    }

    // ── gps ───────────────────────────────────────────────────────────
    if (strcmp(cmd, "gps") == 0) {
        auto& cfg = const_cast<ops::Config&>(ops::config::get());
        if (strcmp(args, "on") == 0) {
            cfg.gpsEnabled = true; ops::config::save();
            appendLine("GPS enabled.");
        } else if (strcmp(args, "off") == 0) {
            cfg.gpsEnabled = false; ops::config::save();
            appendLine("GPS disabled.");
        } else if (strcmp(args, "get") == 0 || args[0] == '\0') {
            auto& b = Board::instance();
            char buf[80];
            appendLine("GPS Status:");
            snprintf(buf, sizeof(buf), "   Enabled: %s", cfg.gpsEnabled ? "yes" : "no");
            appendLine(buf);
            appendLine("   Baud rate: 38400");
            snprintf(buf, sizeof(buf), "   NMEA sentences received: %lu",
                     (unsigned long)b.gpsNmeaCount());
            appendLine(buf);
            snprintf(buf, sizeof(buf), "   Valid fix: %s", b.hasGPSFix() ? "YES" : "NO");
            appendLine(buf);
            snprintf(buf, sizeof(buf), "   Satellites: %u", b.gpsSatellites());
            appendLine(buf);
            if (b.hasGPSFix()) {
                snprintf(buf, sizeof(buf), "   Position: %.6f, %.6f",
                         b.gpsLat(), b.gpsLng());
                appendLine(buf);
                snprintf(buf, sizeof(buf), "   Altitude: %.0f m", b.gpsAltM());
                appendLine(buf);
                uint16_t yr; uint8_t mo, dy, hr, mi, sc;
                if (b.gpsDateTime(yr, mo, dy, hr, mi, sc)) {
                    snprintf(buf, sizeof(buf),
                             "   Time: %04u-%02u-%02u %02u:%02u:%02u UTC",
                             yr, mo, dy, hr, mi, sc);
                    appendLine(buf);
                }
                snprintf(buf, sizeof(buf), "   HDOP: %.1f", b.gpsHdop());
                appendLine(buf);
            }
        } else {
            appendLine("Usage: /gps on|off");
        }
        return;
    }

    // ── channels ──────────────────────────────────────────────────────
    if (strcmp(cmd, "channels") == 0) {
        const auto& cfg = ops::config::get();
        for (int i = 0; i < 5; i++) {
            char buf[48];
            if (cfg.channels[i].name[0]) {
                snprintf(buf, sizeof(buf), "  CH%d: #%s", i + 1, cfg.channels[i].name);
            } else {
                snprintf(buf, sizeof(buf), "  CH%d: disabled", i + 1);
            }
            appendLine(buf);
        }
        return;
    }

    // ── channel <n> ───────────────────────────────────────────────────
    if (strcmp(cmd, "channel") == 0) {
        int n = atoi(args);
        if (n < 1 || n > 5) { appendLine("Usage: /channel <1-5>"); return; }
        const auto& cfg = ops::config::get();
        const auto& ch = cfg.channels[n - 1];
        char buf[48];
        if (!ch.name[0]) { snprintf(buf, sizeof(buf), "CH%d: disabled", n); }
        else              { snprintf(buf, sizeof(buf), "CH%d: #%s", n, ch.name); }
        appendLine(buf);
        return;
    }

    // ── setchannel <n> #<name> [hex-psk] ─────────────────────────────
    if (strcmp(cmd, "setchannel") == 0) {
        int n = atoi(args);
        if (n < 1 || n > 5) { appendLine("Usage: /setchannel <1-5> #<name> [hex-psk]"); return; }
        const char* nameStart = strchr(args, '#');
        if (!nameStart) { appendLine("Usage: /setchannel <1-5> #<name> [hex-psk]"); return; }
        nameStart++;  // skip '#'

        char chName[16] = {};
        const char* nameEnd = strchr(nameStart, ' ');
        if (nameEnd) {
            size_t nlen = (size_t)(nameEnd - nameStart);
            if (nlen >= sizeof(chName)) nlen = sizeof(chName) - 1;
            memcpy(chName, nameStart, nlen);
        } else {
            strncpy(chName, nameStart, sizeof(chName) - 1);
        }

        char psk64[28] = {};
        const char* pskArg = nameEnd ? nameEnd + 1 : nullptr;
        if (pskArg) { while (*pskArg == ' ') pskArg++; }

        if (pskArg && pskArg[0]) {
            size_t plen = strlen(pskArg);
            if (plen == 24 && pskArg[22] == '=' && pskArg[23] == '=') {
                memcpy(psk64, pskArg, 24);
            } else if (plen >= 2 && plen <= 32 && (plen % 2 == 0)) {
                // hex bytes → 16-byte PSK (zero-padded if short) → base64
                uint8_t pskBytes[16] = {};
                int bc = (int)(plen / 2);
                if (bc > 16) bc = 16;
                for (int i = 0; i < bc; i++) {
                    char hb[3] = { pskArg[i*2], pskArg[i*2+1], '\0' };
                    pskBytes[i] = (uint8_t)strtol(hb, nullptr, 16);
                }
                static const char kB64[] =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                for (int b = 0, j = 0; b < 15; b += 3, j += 4) {
                    uint32_t v = ((uint32_t)pskBytes[b]   << 16)
                               | ((uint32_t)pskBytes[b+1] <<  8)
                               |            pskBytes[b+2];
                    psk64[j+0] = kB64[(v >> 18) & 63];
                    psk64[j+1] = kB64[(v >> 12) & 63];
                    psk64[j+2] = kB64[(v >>  6) & 63];
                    psk64[j+3] = kB64[ v        & 63];
                }
                uint32_t v = (uint32_t)pskBytes[15] << 16;
                psk64[20] = kB64[(v >> 18) & 63];
                psk64[21] = kB64[(v >> 12) & 63];
                psk64[22] = '='; psk64[23] = '=';
            } else {
                appendLine("PSK: use 2-32 hex chars (1-16 bytes) or 24-char base64");
                return;
            }
        }

        auto& cfg = const_cast<ops::Config&>(ops::config::get());
        strncpy(cfg.channels[n-1].name, chName, sizeof(cfg.channels[n-1].name) - 1);
        cfg.channels[n-1].name[sizeof(cfg.channels[n-1].name) - 1] = '\0';
        if (psk64[0]) {
            strncpy(cfg.channels[n-1].psk, psk64, sizeof(cfg.channels[n-1].psk) - 1);
            cfg.channels[n-1].psk[sizeof(cfg.channels[n-1].psk) - 1] = '\0';
        }
        ops::config::save();
        char buf[64];
        if (psk64[0]) {
            snprintf(buf, sizeof(buf), "CH%d set to #%s  psk=%.8s...", n, chName, psk64);
        } else {
            snprintf(buf, sizeof(buf), "CH%d set to #%s  (PSK auto-derived)", n, chName);
        }
        appendLine(buf);
        return;
    }

    // ── mobrep ────────────────────────────────────────────────────────
    if (strcmp(cmd, "mobrep") == 0) {
        auto& cfg = const_cast<ops::Config&>(ops::config::get());
        cfg.mobileRepeater = !cfg.mobileRepeater;
        ops::config::save();
        appendLine(cfg.mobileRepeater ? "Mobile repeater: ON" : "Mobile repeater: OFF");
        return;
    }

    // ── quiet ─────────────────────────────────────────────────────────
    if (strcmp(cmd, "quiet") == 0) {
        auto& cfg = const_cast<ops::Config&>(ops::config::get());
        cfg.notifyPopup = !cfg.notifyPopup;
        ops::config::save();
        appendLine(cfg.notifyPopup ? "Quiet mode: OFF (popups on)" : "Quiet mode: ON (popups suppressed)");
        return;
    }

    // ── adverts ───────────────────────────────────────────────────────
    if (strcmp(cmd, "adverts") == 0) {
        auto& cfg = const_cast<ops::Config&>(ops::config::get());
        cfg.showAdverts = !cfg.showAdverts;
        ops::config::save();
        appendLine(cfg.showAdverts ? "Show adverts: ON" : "Show adverts: OFF");
        return;
    }

    // ── i2c scan ──────────────────────────────────────────────────────
    if (strcmp(cmd, "i2c") == 0 && strcmp(args, "scan") == 0) {
        appendLine("Scanning I2C bus (SDA=18, SCL=8)...");
        int found = 0;
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "  0x%02X (%d)", addr, addr);
                appendLine(buf);
                found++;
            }
        }
        char summary[32];
        snprintf(summary, sizeof(summary), "Done. %d device(s) found.", found);
        appendLine(summary);
        return;
    }

    // ── kbbl — keyboard backlight probe ──────────────────────────────
    // Usage: /kbbl <0-255>
    // Tries every plausible I2C protocol for the keyboard backlight and
    // reports which writes are ACKed.  Run /i2c scan first to see addresses.
    if (strcmp(cmd, "kbbl") == 0) {
        int val = (args[0] != '\0') ? atoi(args) : 255;
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        char buf[64];
        snprintf(buf, sizeof(buf), "kbbl probe val=%d", val);
        appendLine(buf);

        struct { uint8_t addr; const char* label; } addrs[] = {
            { 0x55, "0x55 (T-Deck MCU)" },
            { 0x1F, "0x1F (BBQ10)"      },
        };
        for (auto& a : addrs) {
            // Protocol A: single-byte write (brightness only)
            Wire.beginTransmission(a.addr);
            Wire.write((uint8_t)val);
            uint8_t ra = Wire.endTransmission(true);
            snprintf(buf, sizeof(buf), "  %s  A(1-byte val): %s",
                     a.label, ra == 0 ? "ACK" : "NACK");
            appendLine(buf);

            // Protocol B: 2-byte write — cmd 0x05 + value (used by BBQ10 REG_BKL)
            Wire.beginTransmission(a.addr);
            Wire.write((uint8_t)0x05);
            Wire.write((uint8_t)val);
            uint8_t rb = Wire.endTransmission(true);
            snprintf(buf, sizeof(buf), "  %s  B(0x05 val):   %s",
                     a.label, rb == 0 ? "ACK" : "NACK");
            appendLine(buf);

            // Protocol C: 2-byte write — cmd 0x01 + value
            Wire.beginTransmission(a.addr);
            Wire.write((uint8_t)0x01);
            Wire.write((uint8_t)val);
            uint8_t rc = Wire.endTransmission(true);
            snprintf(buf, sizeof(buf), "  %s  C(0x01 val):   %s",
                     a.label, rc == 0 ? "ACK" : "NACK");
            appendLine(buf);
        }
        appendLine("Done. ACK = device accepted the write.");
        return;
    }

    // ── scope [#<region> | clear] ────────────────────────────────────
    if (strcmp(cmd, "scope") == 0) {
        auto& cfg = const_cast<ops::Config&>(ops::config::get());
        if (args[0] == '\0') {
            char buf[32];
            if (cfg.scopeTag[0]) snprintf(buf, sizeof(buf), "Scope: #%s", cfg.scopeTag);
            else                 strncpy(buf, "Scope: (none)", sizeof(buf));
            appendLine(buf);
        } else if (strcmp(args, "clear") == 0) {
            cfg.scopeTag[0] = '\0';
            ops::config::save();
            appendLine("Scope cleared.");
        } else {
            const char* region = (args[0] == '#') ? args + 1 : args;
            strncpy(cfg.scopeTag, region, sizeof(cfg.scopeTag) - 1);
            cfg.scopeTag[sizeof(cfg.scopeTag) - 1] = '\0';
            ops::config::save();
            char buf[32];
            snprintf(buf, sizeof(buf), "Scope: #%s", cfg.scopeTag);
            appendLine(buf);
        }
        return;
    }

    // ── uizoom ────────────────────────────────────────────────────────
    if (strcmp(cmd, "uizoom") == 0) {
        appendLine("[uizoom - not yet implemented]");
        return;
    }

    // ── uifont ────────────────────────────────────────────────────────
    if (strcmp(cmd, "uifont") == 0) {
        appendLine("[uifont - not yet implemented]");
        return;
    }

    // ── set {name|...} ────────────────────────────────────────────────
    if (strcmp(cmd, "set") == 0) {
        char sub[16] = {};
        const char* sub_args = "";
        const char* sp2 = strchr(args, ' ');
        if (sp2) {
            size_t len2 = (size_t)(sp2 - args);
            if (len2 >= sizeof(sub)) len2 = sizeof(sub) - 1;
            memcpy(sub, args, len2);
            sub_args = sp2 + 1;
            while (*sub_args == ' ') sub_args++;
        } else {
            strncpy(sub, args, sizeof(sub) - 1);
        }
        auto& cfg = const_cast<ops::Config&>(ops::config::get());
        auto& mesh = MeshService::instance();
        char buf[64];
        if (strcmp(sub, "name") == 0) {
            if (!sub_args[0]) { appendLine("Usage: /set name <callsign>"); return; }
            ops::config::setCallsign(sub_args);
            snprintf(buf, sizeof(buf), "Callsign: %s", sub_args);
            appendLine(buf);
        } else if (strcmp(sub, "freq") == 0) {
            if (!sub_args[0]) { appendLine("Usage: /set freq <MHz>"); return; }
            float mhz = (float)atof(sub_args);
            if (mhz < 150.0f || mhz > 960.0f) { appendLine("Freq out of range (150-960 MHz)."); return; }
            cfg.freqMHz = mhz; cfg.radioCustom = true;
            ops::config::save();
            if (mesh.initialized()) mesh.setFreqMHz(mhz);
            snprintf(buf, sizeof(buf), "Freq: %.3f MHz", (double)mhz);
            appendLine(buf);
        } else if (strcmp(sub, "sf") == 0) {
            if (!sub_args[0]) { appendLine("Usage: /set sf <7-12>"); return; }
            int sf = atoi(sub_args);
            if (sf < 7 || sf > 12) { appendLine("SF out of range (7-12)."); return; }
            cfg.radioSF = (uint8_t)sf; cfg.radioCustom = true;
            ops::config::save();
            if (mesh.initialized()) mesh.setSpreadingFactor((uint8_t)sf);
            snprintf(buf, sizeof(buf), "SF: %d", sf);
            appendLine(buf);
        } else if (strcmp(sub, "bw") == 0) {
            if (!sub_args[0]) { appendLine("Usage: /set bw <62|125|250>"); return; }
            int bwi = atoi(sub_args);
            uint8_t bwCode; float bwKhz;
            if      (bwi <= 63)  { bwCode = 1; bwKhz = 62.5f; }
            else if (bwi <= 126) { bwCode = 2; bwKhz = 125.0f; }
            else                 { bwCode = 3; bwKhz = 250.0f; }
            cfg.radioBW = bwCode; cfg.radioCustom = true;
            ops::config::save();
            if (mesh.initialized()) mesh.setBandwidth(bwKhz);
            snprintf(buf, sizeof(buf), "BW: %.1f kHz", (double)bwKhz);
            appendLine(buf);
        } else if (strcmp(sub, "cr") == 0) {
            if (!sub_args[0]) { appendLine("Usage: /set cr <5-8>"); return; }
            int cr = atoi(sub_args);
            if (cr < 5 || cr > 8) { appendLine("CR out of range (5-8)."); return; }
            cfg.radioCR = (uint8_t)cr; cfg.radioCustom = true;
            ops::config::save();
            if (mesh.initialized()) mesh.setCodingRate((uint8_t)cr);
            snprintf(buf, sizeof(buf), "CR: %d", cr);
            appendLine(buf);
        } else if (strcmp(sub, "tx") == 0) {
            if (!sub_args[0]) { appendLine("Usage: /set tx <dBm>  (-17 to 22)"); return; }
            int tx = atoi(sub_args);
            if (tx < -17 || tx > 22) { appendLine("TX out of range (-17 to 22 dBm)."); return; }
            cfg.radioTX = (int8_t)tx; cfg.radioCustom = true;
            ops::config::save();
            if (mesh.initialized()) mesh.setTxPower((int8_t)tx);
            snprintf(buf, sizeof(buf), "TX: %d dBm", tx);
            appendLine(buf);
        } else if (strcmp(sub, "lat") == 0) {
            if (!sub_args[0]) { appendLine("Usage: /set lat <decimal>"); return; }
            float lat = (float)atof(sub_args);
            if (lat < -90.0f || lat > 90.0f) { appendLine("Lat out of range (-90 to 90)."); return; }
            cfg.manualLat = lat;
            ops::config::save();
            snprintf(buf, sizeof(buf), "Manual lat: %.6f", (double)lat);
            appendLine(buf);
        } else if (strcmp(sub, "lon") == 0) {
            if (!sub_args[0]) { appendLine("Usage: /set lon <decimal>"); return; }
            float lon = (float)atof(sub_args);
            if (lon < -180.0f || lon > 180.0f) { appendLine("Lon out of range (-180 to 180)."); return; }
            cfg.manualLon = lon;
            ops::config::save();
            snprintf(buf, sizeof(buf), "Manual lon: %.6f", (double)lon);
            appendLine(buf);
        } else if (strcmp(sub, "af") == 0) {
            if (!sub_args[0] || (strcmp(sub_args,"on")!=0 && strcmp(sub_args,"off")!=0)) {
                appendLine("Usage: /set af on|off"); return;
            }
            cfg.autoForward = (strcmp(sub_args, "on") == 0);
            ops::config::save();
            appendLine(cfg.autoForward ? "Auto-forward: ON" : "Auto-forward: OFF");
        } else {
            snprintf(buf, sizeof(buf), "[set %s - not yet implemented]", sub);
            appendLine(buf);
        }
        return;
    }

    // ── get {radio|...} ───────────────────────────────────────────────
    if (strcmp(cmd, "get") == 0) {
        if (strcmp(args, "radio") == 0) {
            const auto& cfg = ops::config::get();
            uint8_t p = cfg.radioProfile > 2 ? 0 : cfg.radioProfile;
            static const char*   kProfNames[] = { "NAR", "MED", "LON" };
            static const uint8_t kProfSF[]    = { 8, 10, 11 };
            static const float   kProfBW[]    = { 62.5f, 250.0f, 250.0f };
            static const uint8_t kProfCR[]    = { 8, 5, 5 };
            auto& mesh = MeshService::instance();
            char buf[72];
            snprintf(buf, sizeof(buf), "Profile: %s%s",
                     kProfNames[p], cfg.radioCustom ? " (overrides active)" : "");
            appendLine(buf);
            snprintf(buf, sizeof(buf), "  Freq: %.3f MHz%s",
                     (double)mesh.getFreqMHz(),
                     (cfg.radioCustom && cfg.freqMHz > 0.0f) ? " *" : "");
            appendLine(buf);
            uint8_t sf = (cfg.radioCustom && cfg.radioSF) ? cfg.radioSF : kProfSF[p];
            snprintf(buf, sizeof(buf), "  SF: %d%s",
                     sf, (cfg.radioCustom && cfg.radioSF) ? " *" : "");
            appendLine(buf);
            float bw;
            if (cfg.radioCustom && cfg.radioBW) {
                bw = (cfg.radioBW == 1) ? 62.5f : (cfg.radioBW == 2) ? 125.0f : 250.0f;
            } else {
                bw = kProfBW[p];
            }
            snprintf(buf, sizeof(buf), "  BW: %.1f kHz%s",
                     (double)bw, (cfg.radioCustom && cfg.radioBW) ? " *" : "");
            appendLine(buf);
            uint8_t cr = (cfg.radioCustom && cfg.radioCR) ? cfg.radioCR : kProfCR[p];
            snprintf(buf, sizeof(buf), "  CR: %d%s",
                     cr, (cfg.radioCustom && cfg.radioCR) ? " *" : "");
            appendLine(buf);
            if (cfg.radioCustom && cfg.radioTX) {
                snprintf(buf, sizeof(buf), "  TX: %d dBm *", (int)cfg.radioTX);
                appendLine(buf);
            }
            if (cfg.radioCustom) appendLine("  (* = custom override)");
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "[get %s - not yet implemented]", args);
            appendLine(buf);
        }
        return;
    }

    // ── card ──────────────────────────────────────────────────────────
    if (strcmp(cmd, "card") == 0) {
        const auto& cfg = ops::config::get();
        uint8_t p = cfg.radioProfile > 2 ? 0 : cfg.radioProfile;
        static const char* kProfNames[] = { "NAR", "MED", "LON" };
        auto& mesh = MeshService::instance();
        char buf[64];
        snprintf(buf, sizeof(buf), "Callsign: %s",
                 cfg.callsign[0] ? cfg.callsign : "(not set)");
        appendLine(buf);
        uint8_t prefix[4] = {};
        mesh.getSelfPubKeyPrefix(prefix);
        if (prefix[0] || prefix[1] || prefix[2] || prefix[3]) {
            snprintf(buf, sizeof(buf), "Key: %02X%02X%02X%02X",
                     prefix[0], prefix[1], prefix[2], prefix[3]);
        } else {
            strncpy(buf, "Key: (mesh not initialised)", sizeof(buf));
        }
        appendLine(buf);
        snprintf(buf, sizeof(buf), "Radio: %s  %.3f MHz",
                 kProfNames[p], (double)mesh.getFreqMHz());
        appendLine(buf);
        return;
    }

    // ── autorep ───────────────────────────────────────────────────────
    if (strcmp(cmd, "autorep") == 0) {
        auto& cfg = const_cast<ops::Config&>(ops::config::get());
        cfg.autoAddRepeater = !cfg.autoAddRepeater;
        ops::config::save();
        appendLine(cfg.autoAddRepeater ? "Auto-add repeater: ON" : "Auto-add repeater: OFF");
        return;
    }

    // ── list {n} ──────────────────────────────────────────────────────
    if (strcmp(cmd, "list") == 0) {
        int total = ops::contacts::count();
        int n = (args[0] != '\0') ? atoi(args) : total;
        if (n <= 0 || n > total) n = total;
        int start = total - n;
        char buf[64];
        snprintf(buf, sizeof(buf), "Contacts (%d/%d):", n, total);
        appendLine(buf);
        for (int i = start; i < total; i++) {
            ops::Contact c;
            if (ops::contacts::get(i, c)) {
                snprintf(buf, sizeof(buf), "  [%d] %s  %02X%02X%02X%02X",
                         i, c.name,
                         c.pubKeyPrefix[0], c.pubKeyPrefix[1],
                         c.pubKeyPrefix[2], c.pubKeyPrefix[3]);
                appendLine(buf);
            }
        }
        return;
    }

    // ── clearcontacts ─────────────────────────────────────────────────
    if (strcmp(cmd, "clearcontacts") == 0) {
        int n = ops::contacts::count();
        while (ops::contacts::count() > 0) ops::contacts::remove(0);
        ops::contacts::save();
        char buf[48];
        snprintf(buf, sizeof(buf), "Cleared %d contact(s).", n);
        appendLine(buf);
        return;
    }

    // ── contacts delete <8-hex-prefix> ───────────────────────────────
    if (strcmp(cmd, "contacts") == 0) {
        char sub[16] = {};
        const char* sub_args = "";
        const char* sp2 = strchr(args, ' ');
        if (sp2) {
            size_t len2 = (size_t)(sp2 - args);
            if (len2 >= sizeof(sub)) len2 = sizeof(sub) - 1;
            memcpy(sub, args, len2);
            sub_args = sp2 + 1;
            while (*sub_args == ' ') sub_args++;
        } else {
            strncpy(sub, args, sizeof(sub) - 1);
        }
        if (strcmp(sub, "delete") == 0) {
            if (strlen(sub_args) < 8) {
                appendLine("Usage: /contacts delete <8-hex-chars>");
                return;
            }
            uint8_t prefix[4] = {};
            for (int i = 0; i < 4; i++) {
                char hb[3] = { sub_args[i*2], sub_args[i*2+1], '\0' };
                prefix[i] = (uint8_t)strtol(hb, nullptr, 16);
            }
            int idx = -1;
            if (ops::contacts::findByKey(prefix, &idx)) {
                ops::Contact c{};
                char buf[48];
                ops::contacts::get(idx, c);
                snprintf(buf, sizeof(buf), "Deleted: %s", c.name);
                ops::contacts::remove(idx);
                ops::contacts::save();
                appendLine(buf);
            } else {
                appendLine("Contact not found.");
            }
        } else {
            appendLine("Usage: /contacts delete <8-hex-chars>");
        }
        return;
    }

    // ── find <name-or-hex> ────────────────────────────────────────────
    if (strcmp(cmd, "find") == 0) {
        if (args[0] == '\0') { appendLine("Usage: /find <name-or-hex>"); return; }
        char qu[32] = {};
        for (int i = 0; i < (int)sizeof(qu) - 1 && args[i]; i++) {
            char ch = args[i];
            qu[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
        }
        int found = 0;
        char buf[64];
        int cn = ops::contacts::count();
        for (int i = 0; i < cn; i++) {
            ops::Contact ct;
            if (!ops::contacts::get(i, ct)) continue;
            char hex[9];
            snprintf(hex, sizeof(hex), "%02X%02X%02X%02X",
                     ct.pubKeyPrefix[0], ct.pubKeyPrefix[1],
                     ct.pubKeyPrefix[2], ct.pubKeyPrefix[3]);
            if (strstr(hex, qu) || strstr(ct.name, args)) {
                snprintf(buf, sizeof(buf), "  [C] %s  %s", ct.name, hex);
                appendLine(buf);
                found++;
            }
        }
        int rn = ops::repeaters::count();
        for (int i = 0; i < rn; i++) {
            ops::Repeater rt;
            if (!ops::repeaters::get(i, rt)) continue;
            char hex[9];
            snprintf(hex, sizeof(hex), "%02X%02X%02X%02X",
                     rt.pubKeyPrefix[0], rt.pubKeyPrefix[1],
                     rt.pubKeyPrefix[2], rt.pubKeyPrefix[3]);
            if (strstr(hex, qu) || strstr(rt.name, args)) {
                snprintf(buf, sizeof(buf), "  [R] %s  %s", rt.name, hex);
                appendLine(buf);
                found++;
            }
        }
        if (found == 0) appendLine("No match found.");
        return;
    }

    // ── replist ───────────────────────────────────────────────────────
    if (strcmp(cmd, "replist") == 0) {
        int n = ops::repeaters::count();
        char buf[64];
        snprintf(buf, sizeof(buf), "Repeaters (%d):", n);
        appendLine(buf);
        for (int i = 0; i < n; i++) {
            ops::Repeater r;
            if (ops::repeaters::get(i, r)) {
                snprintf(buf, sizeof(buf), "  [%d] %s  %02X%02X%02X%02X",
                         i, r.name,
                         r.pubKeyPrefix[0], r.pubKeyPrefix[1],
                         r.pubKeyPrefix[2], r.pubKeyPrefix[3]);
                appendLine(buf);
            }
        }
        return;
    }

    // ── clearrep ──────────────────────────────────────────────────────
    if (strcmp(cmd, "clearrep") == 0) {
        int n = ops::repeaters::count();
        while (ops::repeaters::count() > 0) ops::repeaters::remove(0);
        ops::repeaters::save();
        char buf[48];
        snprintf(buf, sizeof(buf), "Cleared %d repeater(s).", n);
        appendLine(buf);
        return;
    }

    // ── to [name|prefix] ─────────────────────────────────────────────
    if (strcmp(cmd, "to") == 0) {
        if (args[0] == '\0') {
            char buf[64];
            if (s_dmTarget[0]) {
                snprintf(buf, sizeof(buf), "DM target: %s  %02X%02X%02X%02X",
                         s_dmTarget,
                         s_dmTargetKey[0], s_dmTargetKey[1],
                         s_dmTargetKey[2], s_dmTargetKey[3]);
            } else {
                strncpy(buf, "No DM target set.  Usage: /to <name|8-hex>", sizeof(buf));
            }
            appendLine(buf);
            return;
        }
        // Detect 8-hex prefix
        bool isHex = (strlen(args) == 8);
        if (isHex) {
            for (int i = 0; i < 8; i++) {
                char c = args[i];
                if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) { isHex = false; break; }
            }
        }
        if (isHex) {
            uint8_t prefix[4] = {};
            for (int i = 0; i < 4; i++) {
                char hb[3] = { args[i*2], args[i*2+1], '\0' };
                prefix[i] = (uint8_t)strtol(hb, nullptr, 16);
            }
            int idx = -1;
            if (ops::contacts::findByKey(prefix, &idx)) {
                ops::Contact ct{};
                ops::contacts::get(idx, ct);
                strncpy(s_dmTarget, ct.name, sizeof(s_dmTarget) - 1);
            } else {
                snprintf(s_dmTarget, sizeof(s_dmTarget), "%02X%02X%02X%02X",
                         prefix[0], prefix[1], prefix[2], prefix[3]);
            }
            memcpy(s_dmTargetKey, prefix, 4);
        } else {
            int cn = ops::contacts::count();
            bool found = false;
            for (int i = 0; i < cn; i++) {
                ops::Contact ct{};
                if (!ops::contacts::get(i, ct)) continue;
                if (strstr(ct.name, args)) {
                    strncpy(s_dmTarget, ct.name, sizeof(s_dmTarget) - 1);
                    memcpy(s_dmTargetKey, ct.pubKeyPrefix, 4);
                    found = true;
                    break;
                }
            }
            if (!found) { appendLine("Contact not found."); return; }
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "DM target: %s  %02X%02X%02X%02X",
                 s_dmTarget,
                 s_dmTargetKey[0], s_dmTargetKey[1],
                 s_dmTargetKey[2], s_dmTargetKey[3]);
        appendLine(buf);
        return;
    }

    // ── send <text> ───────────────────────────────────────────────────
    if (strcmp(cmd, "send") == 0) {
        if (!s_dmTarget[0]) { appendLine("No DM target.  Use /to <name|prefix> first."); return; }
        if (args[0] == '\0') { appendLine("Usage: /send <text>"); return; }
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        bool ok = mesh.sendDirect(s_dmTargetKey, args);
        char buf[48];
        snprintf(buf, sizeof(buf), ok ? "Sent to %s." : "Send to %s failed.", s_dmTarget);
        appendLine(buf);
        return;
    }

    // ── advert ────────────────────────────────────────────────────────
    if (strcmp(cmd, "advert") == 0) {
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        mesh.sendAdvert(0);
        appendLine("Advert sent.");
        return;
    }

    // ── public <text> ─────────────────────────────────────────────────
    if (strcmp(cmd, "public") == 0) {
        if (args[0] == '\0') { appendLine("Usage: /public <text>"); return; }
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        appendLine(mesh.sendChannel(0, args) ? "Sent to public channel." : "Send failed.");
        return;
    }

    // ── local <text> ──────────────────────────────────────────────────
    if (strcmp(cmd, "local") == 0) {
        if (args[0] == '\0') { appendLine("Usage: /local <text>"); return; }
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        appendLine(mesh.sendChannel(1, args)
                   ? "Sent to local channel."
                   : "Send failed (channel 2 not configured?).");
        return;
    }

    // ── ch3 / ch4 / ch5 <text> ────────────────────────────────────────
    if (strcmp(cmd, "ch3") == 0 || strcmp(cmd, "ch4") == 0 || strcmp(cmd, "ch5") == 0) {
        if (args[0] == '\0') {
            char buf[28]; snprintf(buf, sizeof(buf), "Usage: /%s <text>", cmd);
            appendLine(buf); return;
        }
        int chIdx = (cmd[2] - '3') + 2;   // ch3→2, ch4→3, ch5→4
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        char buf[56];
        int ch1 = cmd[2] - '0';
        if (mesh.sendChannel(chIdx, args)) {
            snprintf(buf, sizeof(buf), "Sent to CH%d.", ch1);
        } else {
            snprintf(buf, sizeof(buf), "Send failed (CH%d not configured?).", ch1);
        }
        appendLine(buf);
        return;
    }

    // ── trace <name|prefix> ───────────────────────────────────────────
    if (strcmp(cmd, "trace") == 0) {
        if (args[0] == '\0') { appendLine("Usage: /trace <name|8-hex>"); return; }
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        // Resolve target to prefix
        uint8_t prefix[4] = {};
        char targetName[32] = {};
        bool resolved = false;
        bool isHex = (strlen(args) == 8);
        if (isHex) {
            for (int i = 0; i < 8; i++) {
                char c = args[i];
                if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) { isHex = false; break; }
            }
        }
        if (isHex) {
            for (int i = 0; i < 4; i++) {
                char hb[3] = { args[i*2], args[i*2+1], '\0' };
                prefix[i] = (uint8_t)strtol(hb, nullptr, 16);
            }
            snprintf(targetName, sizeof(targetName), "%02X%02X%02X%02X",
                     prefix[0], prefix[1], prefix[2], prefix[3]);
            resolved = true;
        } else {
            int cn = ops::contacts::count();
            for (int i = 0; i < cn; i++) {
                ops::Contact ct{};
                if (!ops::contacts::get(i, ct)) continue;
                if (strstr(ct.name, args)) {
                    memcpy(prefix, ct.pubKeyPrefix, 4);
                    strncpy(targetName, ct.name, sizeof(targetName) - 1);
                    resolved = true; break;
                }
            }
        }
        if (!resolved) { appendLine("Contact not found."); return; }
        // Show path info before sending
        ops::PathInfo pi{};
        char buf[80];
        if (mesh.getContactPath(prefix, pi) && pi.found && pi.known) {
            if (pi.direct) {
                snprintf(buf, sizeof(buf), "Path: direct (0 hops)");
                appendLine(buf);
                appendLine("Note: trace needs target to have forwarding enabled");
            } else {
                snprintf(buf, sizeof(buf), "Path: %d relay hop(s)  hash_sz=%d",
                         (int)pi.hopCount, (int)pi.hashSz);
                appendLine(buf);
                appendLine("Note: result only arrives if target is in direct RF range");
            }
        }
        uint32_t tag;
        if (mesh.sendTrace(prefix, tag)) {
            snprintf(buf, sizeof(buf), "Trace → %s  tag=%08X", targetName, tag);
            appendLine(buf);
            appendLine("Open Trace app to see result (if any).");
        } else {
            appendLine("Trace failed - no path known for this contact.");
        }
        return;
    }

    // ── getpath <8hex> ────────────────────────────────────────────────
    if (strcmp(cmd, "getpath") == 0) {
        if (strlen(args) < 8) { appendLine("Usage: /getpath <8-hex-chars>"); return; }
        uint8_t prefix[4] = {};
        for (int i = 0; i < 4; i++) {
            char hb[3] = { args[i*2], args[i*2+1], '\0' };
            prefix[i] = (uint8_t)strtol(hb, nullptr, 16);
        }
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        ops::PathInfo pi{};
        char buf[72];
        if (!mesh.getContactPath(prefix, pi) || !pi.found) {
            snprintf(buf, sizeof(buf), "Path to %02X%02X%02X%02X: not in mesh table",
                     prefix[0], prefix[1], prefix[2], prefix[3]);
        } else if (!pi.known) {
            snprintf(buf, sizeof(buf), "Path to %02X%02X%02X%02X: unknown",
                     prefix[0], prefix[1], prefix[2], prefix[3]);
        } else if (pi.direct) {
            snprintf(buf, sizeof(buf), "Path to %02X%02X%02X%02X: direct (0 hops)",
                     prefix[0], prefix[1], prefix[2], prefix[3]);
        } else {
            snprintf(buf, sizeof(buf), "Path to %02X%02X%02X%02X: %d hop(s)  hash_sz=%d",
                     prefix[0], prefix[1], prefix[2], prefix[3],
                     (int)pi.hopCount, (int)pi.hashSz);
        }
        appendLine(buf);
        return;
    }

    // ── reset path  (reset all contact paths) ────────────────────────
    if (strcmp(cmd, "reset") == 0) {
        if (strcmp(args, "path") == 0) {
            auto& mesh = MeshService::instance();
            if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
            mesh.resetAllContactPaths();
            appendLine("All contact paths reset.");
        } else {
            char buf[48];
            snprintf(buf, sizeof(buf), "[reset %s - not yet implemented]", args);
            appendLine(buf);
        }
        return;
    }

    // ── resetpath <8hex> ─────────────────────────────────────────────
    if (strcmp(cmd, "resetpath") == 0) {
        if (strlen(args) < 8) { appendLine("Usage: /resetpath <8-hex-chars>"); return; }
        uint8_t prefix[4] = {};
        for (int i = 0; i < 4; i++) {
            char hb[3] = { args[i*2], args[i*2+1], '\0' };
            prefix[i] = (uint8_t)strtol(hb, nullptr, 16);
        }
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        mesh.resetContactPath(prefix);
        char buf[40];
        snprintf(buf, sizeof(buf), "Path reset: %02X%02X%02X%02X",
                 prefix[0], prefix[1], prefix[2], prefix[3]);
        appendLine(buf);
        return;
    }

    // ── sd [mount|restore|ls [path]] ─────────────────────────────────
    if (strcmp(cmd, "sd") == 0) {
        char sub[12] = {};
        const char* sub_args = "";
        const char* sp2 = strchr(args, ' ');
        if (sp2) {
            size_t len2 = (size_t)(sp2 - args);
            if (len2 >= sizeof(sub)) len2 = sizeof(sub) - 1;
            memcpy(sub, args, len2);
            sub_args = sp2 + 1;
            while (*sub_args == ' ') sub_args++;
        } else {
            strncpy(sub, args, sizeof(sub) - 1);
        }

        if (strcmp(sub, "mount") == 0) {
            ops::sdcard::init();
            if (ops::sdcard::isMounted()) {
                char buf[64];
                snprintf(buf, sizeof(buf), "SD mounted: %llu MB free",
                         (unsigned long long)ops::sdcard::freeMB());
                appendLine(buf);
                appendLine(ops::sdcard::hasFile("/ops/identity.bin")
                           ? "  identity.bin   FOUND"
                           : "  identity.bin   not found");
                appendLine(ops::sdcard::hasFile("/ops/settings.json")
                           ? "  settings.json  FOUND"
                           : "  settings.json  not found");
                appendLine(ops::sdcard::hasFile("/ops/contacts.json")
                           ? "  contacts.json  FOUND"
                           : "  contacts.json  not found");
                appendLine(ops::sdcard::hasFile("/ops/repeaters.json")
                           ? "  repeaters.json FOUND"
                           : "  repeaters.json not found");
                bool complete = ops::sdcard::hasCompleteBackup();
                appendLine(complete
                           ? "  Complete set - auto-restore active on next boot."
                           : "  Incomplete set - /sd restore to force reload.");
                appendLine("Run /sd restore to reload contacts+repeaters now.");
            } else {
                appendLine("SD mount failed - no card?");
            }
            return;
        }

        if (!ops::sdcard::isMounted()) { appendLine("SD not mounted. Try /sd mount"); return; }

        if (strcmp(sub, "restore") == 0) {
            char buf[56];
            int nc = ops::contacts::reloadFromSD();
            if (nc >= 0) {
                snprintf(buf, sizeof(buf), "Contacts: %d loaded from SD", nc);
            } else {
                snprintf(buf, sizeof(buf), "Contacts: no SD backup found");
            }
            appendLine(buf);
            int nr = ops::repeaters::reloadFromSD();
            if (nr >= 0) {
                snprintf(buf, sizeof(buf), "Repeaters: %d loaded from SD", nr);
            } else {
                snprintf(buf, sizeof(buf), "Repeaters: no SD backup found");
            }
            appendLine(buf);
            // Identity status
            auto& mesh = MeshService::instance();
            if (mesh.initialized()) {
                uint8_t curKey[4] = {};
                mesh.getSelfPubKeyPrefix(curKey);
                uint8_t sdId[128]; size_t sdIdLen = 0;
                if (ops::sdcard::readFile("/ops/identity.bin", sdId, sizeof(sdId), &sdIdLen)
                    && sdIdLen >= 32)
                {
                    if (memcmp(curKey, sdId, 4) == 0) {
                        appendLine("Identity: SD matches current node.");
                    } else {
                        appendLine("Identity: SD has DIFFERENT key.");
                        appendLine("  Use /identity restore to switch.");
                    }
                } else {
                    appendLine("Identity: no SD backup.");
                }
            }
            return;
        }

        if (strcmp(sub, "ls") == 0) {
            const char* path = sub_args[0] ? sub_args : "/ops";
            static char lsbuf[1024];
            size_t n = ops::sdcard::listDir(path, lsbuf, sizeof(lsbuf));
            if (n == 0) { appendLine("(empty or not found)"); return; }
            char* p = lsbuf;
            while (*p) {
                char* nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                if (*p) appendLine(p);
                if (nl) { *nl = '\n'; p = nl + 1; } else break;
            }
        } else if (sub[0] == '\0') {
            char buf[64];
            snprintf(buf, sizeof(buf), "SD: %llu MB total  %llu MB free",
                     (unsigned long long)ops::sdcard::totalMB(),
                     (unsigned long long)ops::sdcard::freeMB());
            appendLine(buf);
        } else {
            char buf[40];
            snprintf(buf, sizeof(buf), "[sd %s - not yet implemented]", sub);
            appendLine(buf);
        }
        return;
    }

    // ── messages [tag] [n] ────────────────────────────────────────────
    if (strcmp(cmd, "messages") == 0) {
        if (!ops::sdcard::isMounted()) { appendLine("SD not mounted."); return; }
        char tag[16] = {};
        int n = 20;
        const char* sp2 = strchr(args, ' ');
        if (sp2) {
            size_t tlen = (size_t)(sp2 - args);
            if (tlen >= sizeof(tag)) tlen = sizeof(tag) - 1;
            memcpy(tag, args, tlen);
            n = atoi(sp2 + 1);
            if (n <= 0) n = 20;
        } else {
            strncpy(tag, args, sizeof(tag) - 1);
        }
        if (!tag[0]) {
            // List available log files
            static char lsbuf[512];
            size_t bytes = ops::sdcard::listDir("/ops/msgs", lsbuf, sizeof(lsbuf));
            if (bytes == 0) { appendLine("No message logs."); return; }
            appendLine("Logs (use /messages <tag> [n]):");
            char* p = lsbuf;
            while (*p) {
                char* nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                if (*p) {
                    char entry[24] = {};
                    strncpy(entry, p, sizeof(entry) - 1);
                    char* dot = strrchr(entry, '.');
                    if (dot && strcmp(dot, ".log") == 0) *dot = '\0';
                    char line[32];
                    snprintf(line, sizeof(line), "  %s", entry);
                    appendLine(line);
                }
                if (nl) { *nl = '\n'; p = nl + 1; } else break;
            }
            return;
        }
        // Show last n messages from tag
        static char logBuf[4096];
        size_t bytes = ops::sdcard::readMsgLog(tag, logBuf, sizeof(logBuf));
        if (bytes == 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), "No log for '%s'.", tag);
            appendLine(buf);
            return;
        }
        // Count lines to find starting offset for last n
        int lineCount = 0;
        for (size_t i = 0; i < bytes; i++) if (logBuf[i] == '\n') lineCount++;
        int skip = lineCount - n;
        if (skip < 0) skip = 0;
        int lineNum = 0;
        char* p = logBuf;
        while (*p) {
            char* nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (*p && lineNum >= skip) {
                char sender[32] = {}, text[120] = {};
                bool sent = false;
                const char* sf = strstr(p, "\"s\":");
                if (sf) sent = (sf[4] == '1');
                _extractJsonStr(p, "n", sender, sizeof(sender));
                _extractJsonStr(p, "t", text, sizeof(text));
                char line[160];
                snprintf(line, sizeof(line), "%s %s: %.100s",
                         sent ? ">>" : "<<",
                         sent ? (const char*)"You" : (sender[0] ? sender : "?"),
                         text[0] ? text : "(?)");
                appendLine(line);
            }
            if (nl) { *nl = '\n'; p = nl + 1; } else break;
            lineNum++;
        }
        return;
    }

    // ── clearmessages ─────────────────────────────────────────────────
    if (strcmp(cmd, "clearmessages") == 0) {
        if (!ops::sdcard::isMounted()) { appendLine("SD not mounted."); return; }
        ops::sdcard::clearMsgLogs();
        appendLine("Message logs cleared.");
        return;
    }

    // ── identity restore ─────────────────────────────────────────────
    if (strcmp(cmd, "identity") == 0 && strcmp(args, "restore") == 0) {
        if (!ops::sdcard::isMounted()) { appendLine("SD not mounted. Try /sd mount first."); return; }
        uint8_t sdId[128]; size_t sdIdLen = 0;
        if (!ops::sdcard::readFile("/ops/identity.bin", sdId, sizeof(sdId), &sdIdLen) || sdIdLen < 32) {
            appendLine("No identity.bin on SD."); return;
        }
        auto& mesh = MeshService::instance();
        if (mesh.initialized()) {
            uint8_t curKey[4] = {};
            mesh.getSelfPubKeyPrefix(curKey);
            if (memcmp(curKey, sdId, 4) == 0) {
                appendLine("SD identity matches current node - no change needed."); return;
            }
        }
        // Delete LittleFS identity so begin_mesh() restores from SD on next boot
        LittleFS.begin(false);
        LittleFS.remove("/mesh/self.id");
        appendLine("LittleFS identity cleared.");
        appendLine("Restarting in 2s to restore old identity from SD...");
        lv_task_handler();  // flush LVGL before restart
        delay(2000);
        ESP.restart();
        return;
    }

    // ── import <8hex|64hex> <name> ────────────────────────────────────
    if (strcmp(cmd, "import") == 0) {
        const char* sp2 = strchr(args, ' ');
        if (!sp2) { appendLine("Usage: /import <8hex|64hex> <name>"); return; }
        size_t hexLen = (size_t)(sp2 - args);
        const char* name = sp2 + 1;
        while (*name == ' ') name++;
        if (!name[0] || (hexLen != 8 && hexLen != 64)) {
            appendLine("Usage: /import <8hex|64hex> <name>"); return;
        }
        for (size_t i = 0; i < hexLen; i++) {
            char c = args[i];
            if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) {
                appendLine("Invalid hex in key."); return;
            }
        }
        ops::Contact ct{};
        strncpy(ct.name, name, sizeof(ct.name) - 1);
        if (hexLen == 64) {
            for (int i = 0; i < 32; i++) {
                char hb[3] = { args[i*2], args[i*2+1], '\0' };
                ct.pubKey[i] = (uint8_t)strtol(hb, nullptr, 16);
            }
            memcpy(ct.pubKeyPrefix, ct.pubKey, 4);
        } else {
            for (int i = 0; i < 4; i++) {
                char hb[3] = { args[i*2], args[i*2+1], '\0' };
                ct.pubKeyPrefix[i] = (uint8_t)strtol(hb, nullptr, 16);
            }
        }
        ops::contacts::add(ct);
        ops::contacts::save();
        auto& mesh = MeshService::instance();
        if (hexLen == 64 && mesh.initialized()) {
            mesh.preloadContact(ct.pubKey, ct.name);
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "Imported: %s  %02X%02X%02X%02X%s",
                 ct.name,
                 ct.pubKeyPrefix[0], ct.pubKeyPrefix[1],
                 ct.pubKeyPrefix[2], ct.pubKeyPrefix[3],
                 hexLen == 8 ? " (prefix only)" : "");
        appendLine(buf);
        return;
    }

    // ── setpath <8hex> <hexpath> [hashsz] ────────────────────────────
    if (strcmp(cmd, "setpath") == 0) {
        if (strlen(args) < 8) { appendLine("Usage: /setpath <8hex> <hexpath> [hashsz]"); return; }
        uint8_t prefix[4] = {};
        for (int i = 0; i < 4; i++) {
            char hb[3] = { args[i*2], args[i*2+1], '\0' };
            prefix[i] = (uint8_t)strtol(hb, nullptr, 16);
        }
        const char* pathArg = args + 8;
        while (*pathArg == ' ') pathArg++;
        if (!pathArg[0]) { appendLine("Usage: /setpath <8hex> <hexpath> [hashsz]"); return; }
        char pathHex[66] = {};
        uint8_t hashSz = 0;
        const char* sp3 = strchr(pathArg, ' ');
        if (sp3) {
            size_t plen = (size_t)(sp3 - pathArg);
            if (plen >= sizeof(pathHex)) plen = sizeof(pathHex) - 1;
            memcpy(pathHex, pathArg, plen);
            const char* szStr = sp3 + 1;
            while (*szStr == ' ') szStr++;
            hashSz = (uint8_t)atoi(szStr);
        } else {
            strncpy(pathHex, pathArg, sizeof(pathHex) - 1);
        }
        if (hashSz < 1 || hashSz > 2) {
            hashSz = (uint8_t)(ops::config::get().pathHashSz == 2 ? 2 : 1);
        }
        size_t hexLen2 = strlen(pathHex);
        if (hexLen2 == 0 || hexLen2 % 2 != 0) {
            appendLine("Path hex must be even number of hex chars."); return;
        }
        uint8_t pathBytes[32] = {};
        size_t byteCount = hexLen2 / 2;
        if (byteCount > 32) { appendLine("Path too long (max 32 bytes)."); return; }
        if (byteCount % hashSz != 0) {
            appendLine("Path byte count not divisible by hashsz."); return;
        }
        for (size_t i = 0; i < byteCount; i++) {
            char hb[3] = { pathHex[i*2], pathHex[i*2+1], '\0' };
            pathBytes[i] = (uint8_t)strtol(hb, nullptr, 16);
        }
        uint8_t numHops = (uint8_t)(byteCount / hashSz);
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        char buf[64];
        if (mesh.setContactPath(prefix, pathBytes, numHops, hashSz)) {
            snprintf(buf, sizeof(buf), "Path set: %02X%02X%02X%02X  %d hop(s)  sz=%d",
                     prefix[0], prefix[1], prefix[2], prefix[3], numHops, hashSz);
        } else {
            snprintf(buf, sizeof(buf), "Contact %02X%02X%02X%02X not in mesh table.",
                     prefix[0], prefix[1], prefix[2], prefix[3]);
        }
        appendLine(buf);
        return;
    }

    // ── pathsize [1|2] ────────────────────────────────────────────────
    if (strcmp(cmd, "pathsize") == 0) {
        auto& cfg = const_cast<ops::Config&>(ops::config::get());
        if (args[0] == '\0') {
            uint8_t cur = (cfg.pathHashSz == 2) ? 2 : 1;
            char buf[32];
            snprintf(buf, sizeof(buf), "Path hash size: %d byte(s)", cur);
            appendLine(buf);
        } else {
            int sz = atoi(args);
            if (sz != 1 && sz != 2) { appendLine("Usage: /pathsize [1|2]"); return; }
            cfg.pathHashSz = (uint8_t)sz;
            ops::config::save();
            char buf[40];
            snprintf(buf, sizeof(buf), "Path hash size: %d byte(s)", sz);
            appendLine(buf);
        }
        return;
    }

    // ── repeaters [secs] ─────────────────────────────────────────────
    if (strcmp(cmd, "repeaters") == 0) {
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        int n = ops::repeaters::count();
        if (n == 0) {
            appendLine("No repeaters in list.  Add via /autorep or Repeaters screen.");
            return;
        }
        int secs = (args[0] != '\0') ? atoi(args) : 30;
        if (secs <= 0) secs = 30;
        char buf[56];
        snprintf(buf, sizeof(buf), "Querying %d repeater(s) (timeout %ds)...", n, secs);
        appendLine(buf);
        mesh.sendRepeatersStatus(secs);
        return;
    }

    // ── repeateradmin <8hex> [pass] ───────────────────────────────────
    if (strcmp(cmd, "repeateradmin") == 0) {
        if (strlen(args) < 8) {
            appendLine("Usage: /repeateradmin <8hex> [password]");
            return;
        }
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        bool hexOk = true;
        for (int i = 0; i < 8; i++) {
            char c = args[i];
            if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) { hexOk = false; break; }
        }
        if (!hexOk) { appendLine("Invalid hex prefix.  Usage: /repeateradmin <8hex> [password]"); return; }
        uint8_t prefix[4] = {};
        for (int i = 0; i < 4; i++) {
            char hb[3] = { args[i*2], args[i*2+1], '\0' };
            prefix[i] = (uint8_t)strtol(hb, nullptr, 16);
        }
        const char* pass = "";
        if (strlen(args) > 8 && args[8] == ' ') {
            pass = args + 9;
            while (*pass == ' ') pass++;
        }
        appendLine(pass[0] ? "Sending login request..." : "Sending login (no password)...");
        mesh.sendRepeaterLogin(prefix, pass);
        // Store as active admin target for subsequent /repadmin commands
        memcpy(s_adminKey, prefix, 4);
        // Try to resolve a display name from repeater/contact list
        {
            ops::Repeater r;
            int idx = -1;
            if (ops::repeaters::findByKey(prefix, &idx) && ops::repeaters::get(idx, r)) {
                strncpy(s_adminName, r.name, sizeof(s_adminName) - 1);
            } else {
                ops::Contact ct{};
                if (ops::contacts::findByKey(prefix, &idx) && ops::contacts::get(idx, ct)) {
                    strncpy(s_adminName, ct.name, sizeof(s_adminName) - 1);
                } else {
                    snprintf(s_adminName, sizeof(s_adminName), "%02X%02X%02X%02X",
                             prefix[0], prefix[1], prefix[2], prefix[3]);
                }
            }
            s_adminName[sizeof(s_adminName) - 1] = '\0';
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "Admin target set: %s", s_adminName);
        appendLine(buf);
        return;
    }

    // ── repadmin [<8hex>] <command> ───────────────────────────────────
    if (strcmp(cmd, "repadmin") == 0) {
        auto& mesh = MeshService::instance();
        if (!mesh.initialized()) { appendLine("Mesh not ready."); return; }
        if (args[0] == '\0') {
            // Show current admin target
            if (s_adminName[0]) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Admin target: %s  %02X%02X%02X%02X",
                         s_adminName,
                         s_adminKey[0], s_adminKey[1], s_adminKey[2], s_adminKey[3]);
                appendLine(buf);
                appendLine("Usage: /repadmin <cmd>  or  /repadmin <8hex> <cmd>");
            } else {
                appendLine("No admin target.  Use /repeateradmin <8hex> [pass] first.");
            }
            return;
        }
        // If first token is an 8-hex prefix, use it as an override target
        uint8_t targetKey[4];
        const char* command = args;
        bool hasExplicitTarget = false;
        if (strlen(args) >= 8) {
            bool hexOk = true;
            for (int i = 0; i < 8; i++) {
                char c = args[i];
                if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) { hexOk = false; break; }
            }
            if (hexOk && (args[8] == ' ' || args[8] == '\0')) {
                for (int i = 0; i < 4; i++) {
                    char hb[3] = { args[i*2], args[i*2+1], '\0' };
                    targetKey[i] = (uint8_t)strtol(hb, nullptr, 16);
                }
                command = args + 8;
                while (*command == ' ') command++;
                hasExplicitTarget = true;
            }
        }
        if (!hasExplicitTarget) {
            if (!s_adminName[0]) {
                appendLine("No admin target.  Use /repeateradmin <8hex> [pass] first.");
                return;
            }
            memcpy(targetKey, s_adminKey, 4);
        }
        if (command[0] == '\0') { appendLine("Usage: /repadmin [<8hex>] <command>"); return; }
        mesh.sendAdminCommand(targetKey, command);
        return;
    }

    // ── Mesh-dependent stubs ──────────────────────────────────────────
    static const char* kMeshCmds[] = { "control", nullptr };
    for (int i = 0; kMeshCmds[i]; i++) {
        if (strcmp(cmd, kMeshCmds[i]) == 0) {
            appendLine("[mesh not connected]");
            return;
        }
    }

    // ── Unknown ───────────────────────────────────────────────────────
    char buf[64];
    snprintf(buf, sizeof(buf), "Unknown command: %s  (try /help)", cmd);
    appendLine(buf);
}

// ── tickSerial() ─────────────────────────────────────────────────────
// Call from loop().  Reads CDC serial one character at a time, echoes
// printable chars back (so the terminal emulator shows what you type),
// handles backspace, and dispatches on newline.
void ScreenTerminal::tickSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();

        if (c == '\r' || c == '\n') {
            if (_serialLen == 0) continue;     // ignore bare CR/LF
            _serialBuf[_serialLen] = '\0';
            Serial.print("\r\n> ");
            Serial.println(_serialBuf);        // echo the completed command
            _dispatch(_serialBuf);
            _serialLen = 0;
            Serial.print("OMS> ");             // re-print prompt
        } else if ((c == 8 || c == 127) && _serialLen > 0) {
            // Backspace / DEL — erase last character
            _serialLen--;
            Serial.print("\b \b");
        } else if (c >= 32 && c < 127 &&
                   _serialLen < (int)sizeof(_serialBuf) - 1) {
            _serialBuf[_serialLen++] = c;
            Serial.print(c);                   // echo character
        }
    }
}

// ── setAdminTarget() ─────────────────────────────────────────────────
void ScreenTerminal::setAdminTarget(const uint8_t* prefix4, const char* name)
{
    memcpy(s_adminKey, prefix4, 4);
    strncpy(s_adminName, name, sizeof(s_adminName) - 1);
    s_adminName[sizeof(s_adminName) - 1] = '\0';
    char buf[64];
    snprintf(buf, sizeof(buf), "[terminal] Admin target set: %s", s_adminName);
    appendLine(buf);
}

}}  // namespace ops::ui
