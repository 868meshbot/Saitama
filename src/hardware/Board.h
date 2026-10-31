// Saitama — Board.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Hardware abstraction shared by every board. Board.cpp implements it for the
// LilyGo T-Deck Plus (pin map verified against the official schematic);
// cardputer/Board.cpp for the M5Stack Cardputer ADV. On the Cardputer the
// "trackball" is the keyboard's arrow keys and the press is Enter/G0.

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "../features.h"
#ifdef OPS_HAS_BUILTIN_GPS
#include <TinyGPSPlus.h>
#endif

namespace ops {

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
    // Adds to the direction the next consumeTrackballDelta() returns.
    void addTrackballDelta(int16_t dx, int16_t dy) { _trackballX += dx; _trackballY += dy; }

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

    // millis() timestamp of the last successful RTC sync from GPS; 0 if never.
    uint32_t gpsLastSyncMs() const {
#ifdef OPS_HAS_BUILTIN_GPS
        return _gpsLastSync;
#else
        return 0;
#endif
    }

    // Send GPS chip to PMTK standby (any UART byte wakes it).
    void gpsStandby() {
#ifdef OPS_HAS_BUILTIN_GPS
        _gpsSerial.print("$PMTK161,0*28\r\n");
#endif
    }

    // Wake GPS from PMTK standby by sending a dummy byte.
    void gpsWake() {
#ifdef OPS_HAS_BUILTIN_GPS
        _gpsSerial.write((uint8_t)0xFF);
#endif
    }

    // Screen backlight PWM on TFT_BL (T-Deck GPIO 42) via ledc (channel 0, 5 kHz, 8-bit).
    // Call initBacklightPWM() once after Board::init(); then setDisplayBrightness()
    // anytime. Range: 0 = off/blank, 128 = 50%, 255 = full.
    static constexpr uint8_t  BL_LEDC_CH   = 0;
    static constexpr uint32_t BL_LEDC_FREQ = OPS_BL_PWM_HZ;
    static constexpr uint8_t  BL_LEDC_BITS = 8;

    void initBacklightPWM() {
        ledcSetup(BL_LEDC_CH, BL_LEDC_FREQ, BL_LEDC_BITS);
        ledcAttachPin(TFT_BL, BL_LEDC_CH);
    }
    void setDisplayBrightness(uint8_t val) { ledcWrite(BL_LEDC_CH, val); }

    // Keyboard backlight via keyboard MCU (I2C 0x55).
    // Sends all three protocols — A (single byte), B (reg 0x05 + val), C (reg 0x09 + val) —
    // mirroring exactly what the /kbbl terminal probe does, which is confirmed to work.
    // All three ACK; the MCU firmware determines which one it acts on.
    void setKeyboardBacklight(uint8_t brightness) {
#if !OPS_FEAT_KB_BACKLIGHT
        (void)brightness;
#else
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
#endif
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

#ifdef OPS_BOARD_CARDPUTER
    // Keyboard (TCA8418): key events are queued by the driver and drained by
    // pollKeyboard(); arrow keys go to the trackball delta instead.
    bool _fnHeld    = false;
    bool _shiftHeld = false;
    bool _ctrlHeld  = false;
    void _drainKeyEvents();
    char    _keyQueue[16] = {};
    uint8_t _keyHead = 0, _keyTail = 0;
#endif

#ifdef OPS_HAS_BUILTIN_GPS
    HardwareSerial      _gpsSerial{1};
    mutable TinyGPSPlus _gps;  // accessors clear internal flags so can't be const
    uint32_t       _gpsLastSync = 0;  // millis() of last RTC sync from GPS
#endif
};

}  // namespace ops
