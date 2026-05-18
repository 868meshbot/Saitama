// Saitama — Board.h
// Copyright 2026 Saitama — MIT License
//
// Hardware abstraction for the LilyGo T-Deck Plus.
// Pin map verified against the official LilyGo T-Deck Plus schematic.

#pragma once

// Board.cpp uses OMS_HAS_BUILTIN_GPS (hardware-stable — do not modify Board.cpp).
// Bridge from the ops build flag so both Board.h and Board.cpp agree.
#ifdef OPS_HAS_BUILTIN_GPS
#  define OMS_HAS_BUILTIN_GPS 1
#endif

#include <Arduino.h>
#include <Wire.h>
#ifdef OMS_HAS_BUILTIN_GPS
#include <TinyGPSPlus.h>
#endif

namespace oms {

class Board {
public:
    static Board& instance();

    void init();
    void tick();

    // Trackball direction accumulates between calls.
    // Press is polled from GPIO0 (BOOT button = trackball click, active-low).
    // signalTrackballPress() can also be called by the keyboard driver if needed.
    void consumeTrackballDelta(int16_t &dx, int16_t &dy);
    bool consumeTrackballPress();
    void signalTrackballPress() { _trackballPressed = true; }

    // Battery level 0-100 % via voltage divider on BOARD_BAT_ADC (GPIO4)
    int  batteryPercent() const;
    bool batteryCharging() const;  // true when VBAT > 4.1 V (USB attached)

    // BBQ10 keyboard: returns true + fills key when a key was just pressed.
    // Maps arrow keys → up/down signals on the trackball delta.
    bool pollKeyboard(char& outKey);

    // GPS (T-Deck Plus built-in, enable with -DOPS_HAS_BUILTIN_GPS)
    bool     hasGPSFix()    const;
    float    gpsLat()       const;
    float    gpsLng()       const;
    float    gpsAltM()      const;
    float    gpsHdop()      const;
    uint8_t  gpsSatellites() const;
    uint32_t gpsNmeaCount() const;  // sentences that passed checksum
    // Fills UTC date/time fields; returns false if GPS time not valid.
    bool gpsDateTime(uint16_t& year, uint8_t& month,  uint8_t& day,
                     uint8_t&  hour, uint8_t& minute, uint8_t& sec) const;

    // Screen backlight on GPIO42 (BOARD_BL_PIN)
    void setBacklight(bool on) { digitalWrite(42, on ? HIGH : LOW); }

    // Keyboard backlight via keyboard MCU (I2C 0x55).
    // Sends all three protocols — A (single byte), B (reg 0x05 + val), C (reg 0x09 + val) —
    // mirroring exactly what the /kbbl terminal probe does, which is confirmed to work.
    // All three ACK; the MCU firmware determines which one it acts on.
    void setKeyboardBacklight(uint8_t brightness) {
        Wire.beginTransmission((uint8_t)0x55);  // A: single byte
        Wire.write(brightness);
        Wire.endTransmission(true);
        Wire.beginTransmission((uint8_t)0x55);  // B: BBQ10 REG_BKL = 0x05
        Wire.write((uint8_t)0x05);
        Wire.write(brightness);
        Wire.endTransmission(true);
        Wire.beginTransmission((uint8_t)0x55);  // C: reg 0x01
        Wire.write((uint8_t)0x01);
        Wire.write(brightness);
        Wire.endTransmission(true);
    }

    bool initialized() const { return _initialized; }

    // Enable/disable per-tick ISR-count logging to CDC serial.
    // Controlled at runtime (e.g. from terminal command /tbdebug on|off).
    static bool trackballDebug;

private:
    bool _initialized      = false;
    bool _keyboardPresent  = false;  // set true in init() if BBQ10 ACKs at 0x1F

    int16_t _trackballX          = 0;
    int16_t _trackballY          = 0;
    bool    _trackballPressed    = false;
    bool    _trackballPrevDown   = false;  // for falling-edge debounce
    uint32_t _trackballPressMs   = 0;      // millis() of last accepted press

#ifdef OMS_HAS_BUILTIN_GPS
    HardwareSerial      _gpsSerial{1};
    mutable TinyGPSPlus _gps;  // accessors clear internal flags so can't be const
    uint32_t       _gpsLastSync = 0;  // millis() of last RTC sync from GPS
#endif
};

}  // namespace oms

// ops::Board is an alias for oms::Board.
// Board.cpp must remain in namespace oms (hardware-stable — do not modify Board.cpp).
namespace ops { using Board = oms::Board; }
