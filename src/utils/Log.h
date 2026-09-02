// Saitama — Log.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once

#include <Arduino.h>

// CRLF, not bare LF: platformio.ini sets `monitor_filters = direct`, which
// passes bytes through with no LF->CRLF translation. A terminal in raw mode
// (kitty, screen, some serial tools) then drops a line without returning the
// carriage, so output marches diagonally down the screen. Arduino's
// Serial.println() already emits "\r\n" — this brings printf-style logging in
// line with it.
#define OPS_LOG(tag, fmt, ...) Serial.printf("[OPS] %s: " fmt "\r\n", tag, ##__VA_ARGS__)