// Saitama — Log.h
// Copyright 2026 Saitama — MIT License

#pragma once

#include <Arduino.h>

#define OPS_LOG(tag, fmt, ...) Serial.printf("[OPS] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define OMS_LOG OPS_LOG