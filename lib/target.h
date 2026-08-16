// Saitama — target.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Saitama does not use MeshCore's board/variant build system — hardware
// bring-up lives entirely in src/hardware/Board.cpp. MeshCore's own
// lib/MeshCore/src/helpers/ESP32Board.cpp (added upstream in companion
// v1.17.x) unconditionally does `#include <target.h>` and expects a
// variant-supplied `radio_driver` / `sensors` global. Since Saitama's
// OMSBoard subclasses ESP32Board only to satisfy MeshCore's MainBoard
// interface (see src/mesh/MeshService.cpp), ESP32Board::powerOff() /
// enterDeepSleep() are never actually invoked by Saitama — this stub
// only needs to let that translation unit link, not do anything real.
//
// This file lives outside lib/MeshCore/ so the submodule stays untouched.
#pragma once

struct SaitamaStubRadioDriver
{
    void powerOff() {}
};

struct SaitamaStubLocationProvider
{
    void stop() {}
};

struct SaitamaStubSensorManager
{
    SaitamaStubLocationProvider* getLocationProvider() { return nullptr; }
};

static SaitamaStubRadioDriver   radio_driver;
static SaitamaStubSensorManager sensors;
