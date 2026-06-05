// Saitama — Log.h
// Copyright 2026 Saitama — GPL-3.0-or-later

#pragma once

#include <Arduino.h>

#define OPS_LOG(tag, fmt, ...) Serial.printf("[OPS] %s: " fmt "\n", tag, ##__VA_ARGS__)