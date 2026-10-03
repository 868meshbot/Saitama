// Saitama — BtPin.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "BtPin.h"
#include "Log.h"
#include <Preferences.h>
#include <esp_random.h>

namespace ops {
namespace btpin {

static constexpr const char* NVS_NS = "opsbt";
static uint32_t s_pin = 0;

static uint32_t _random()
{
    return MIN_PIN + esp_random() % (MAX_PIN - MIN_PIN + 1);
}

static bool _store(uint32_t pin, bool clearBonds)
{
    Preferences p;
    if (!p.begin(NVS_NS, false)) return false;
    bool ok = p.putUInt("pin", pin) > 0;
    if (ok && clearBonds) p.putBool("clr", true);
    p.end();
    if (ok) s_pin = pin;
    return ok;
}

void init()
{
    Preferences p;
    uint32_t pin = 0;
    if (p.begin(NVS_NS, true)) {
        pin = p.getUInt("pin", 0);
        p.end();
    }
    if (pin >= MIN_PIN && pin <= MAX_PIN) {
        s_pin = pin;
        return;
    }
    // First boot on this firmware: replace the shared default, and drop any
    // phone that paired with it.
    if (_store(_random(), true))
        OPS_LOG("BT", "Generated a pairing PIN (see Settings > Bluetooth PIN)");
}

uint32_t get() { return s_pin; }

bool set(uint32_t pin)
{
    if (pin < MIN_PIN || pin > MAX_PIN) return false;
    if (pin == s_pin) return true;
    return _store(pin, true);
}

uint32_t randomize()
{
    uint32_t pin;
    do { pin = _random(); } while (pin == s_pin);
    return _store(pin, true) ? pin : s_pin;
}

bool takeBondClear()
{
    Preferences p;
    if (!p.begin(NVS_NS, false)) return false;
    bool clr = p.getBool("clr", false);
    if (clr) p.remove("clr");
    p.end();
    return clr;
}

}  // namespace btpin
}  // namespace ops
