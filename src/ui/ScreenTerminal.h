// Saitama — ScreenTerminal.h
// Copyright 2026 Saitama — MIT License

#pragma once
#include <lvgl.h>

namespace ops { namespace ui {

class ScreenTerminal {
public:
    static void show();

    // Open terminal in repeater-admin mode: every typed line is sent as
    // sendAdminCommand to prefix4/name; ESC returns to ScreenRepeaters.
    static void showAdmin(const uint8_t* prefix4, const char* name);

    // Append a line of text to the terminal output AND echo it to CDC serial.
    static void appendLine(const char* line);

    // Call from loop() — reads CDC serial, buffers lines, dispatches commands.
    static void tickSerial();

private:
    static lv_obj_t* _screen;
    static lv_obj_t* _logScroll;   // scrollable container
    static lv_obj_t* _logLabel;    // label inside scroll container
    static lv_obj_t* _input;       // textarea for command input

    // true while in repeater-admin mode (input routed to sendAdminCommand)
    static bool s_adminMode;

    static void _buildTopBar(lv_obj_t* parent);
    static void _buildLog   (lv_obj_t* parent);
    static void _buildInput (lv_obj_t* parent);
    static void _scrollToBottom();

    static void _onHomeClick(lv_event_t* e);
    static void _onSend     (lv_event_t* e);
    static void _onTermKey  (lv_event_t* e);  // ESC handler — checks s_adminMode

    // Command dispatch — called by _onSend in normal mode
    static void _dispatch(const char* input);

    static constexpr int LOG_BUF_SIZE = 3072;

    // Log buffer lives in PSRAM — allocated once in show() to avoid burning
    // internal DRAM that Bluedroid needs for its connection-time allocations.
    static char*   _logBuf;
    static int     _logLen;

    // Serial line-input buffer
    static char    _serialBuf[256];
    static int     _serialLen;

    // Active DM target — set by /to, used by /send
    static char    s_dmTarget[32];
    static uint8_t s_dmTargetKey[4];

    // Active admin target — set by /repeateradmin or ScreenRepeaters, used by /repadmin
    static char    s_adminName[32];
    static uint8_t s_adminKey[4];

public:
    // Set the active admin target (called by ScreenRepeaters after login)
    static void setAdminTarget(const uint8_t* prefix4, const char* name);

private:
};

}}  // namespace ops::ui
