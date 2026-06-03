// Saitama — Config.h
// Copyright 2026 Saitama — MIT License

#pragma once

#include <Arduino.h>
#include <time.h>

namespace ops {

struct ChannelCfg {
    char name[32];      // room/group name; empty = disabled
    char shortname[6];  // tab label (≤5 chars); empty = use first 4 chars of name
    char psk[28];       // base64 PSK override; empty = auto-derive from "#name"
    bool notify;        // play notification sound on incoming messages
    char scope[16];     // flood scope tag (e.g. "AU"); empty = no scoping
};

struct Config {
    char       callsign[16];      // device name shown on mesh
    char       radioRegion[8];    // "EU868", "US915", etc
    ChannelCfg channels[10];      // ten channel slots
    int        activeChannel;     // 0-9
    bool       bluetoothEnabled;
    bool       speakerEnabled;
    uint8_t    gpsMode;         // 0=off, 1=intermittent, 2=on (GPS_MODE_* constants)
    uint8_t    kbBrightness;     // keyboard backlight 0=off, 1-255=brightness
    uint8_t    kbLayout;         // keyboard layout: 0=EN, 1=FR AZERTY, 2=DE QWERTZ
    bool       autoAddClient;    // auto-add client nodes to contacts
    bool       autoAddRepeater;  // auto-add repeater nodes to contacts
    bool       saveMsgs;          // persist received messages to flash
    bool       showHops;          // display hop count on incoming messages
    bool       showRssi;          // display RSSI on incoming messages
    bool       locationSharing;   // broadcast GPS coords on mesh
    bool       notifyPopup;       // show on-screen popup for new messages
    // screenTimeoutSec == 0 means always-on display
    int        brightness;        // 0-255 backlight
    int        screenTimeoutSec;  // seconds before screensaver activates (0=off)
    uint8_t    screenOffSec;      // seconds after screensaver before backlight off (0=never, 20-120 in 10s steps)
    uint8_t    speakerVolume;     // 0-100 percent; scales I2S samples at playback
    bool       notifySound;
    uint8_t    notifySoundChoice; // 0=default ping, 1=pluck, 2=clear, 3=whoosh
    char       mapTileDir[32];
    int        theme;             // 0 = dark
    uint8_t    radioProfile;      // 0=NAR 1=MED 2=LON
    bool       showAdverts;       // print incoming advert packets in terminal
    // Radio parameter overrides — applied after profile when radioCustom is true
    bool       radioCustom;       // true = use individual overrides below
    float      freqMHz;           // MHz; 0 = use profile default
    uint8_t    radioSF;           // spreading factor 7-12; 0 = use profile default
    uint8_t    radioBW;           // 1=62.5kHz, 2=125kHz, 3=250kHz; 0 = use profile default
    uint8_t    radioCR;           // coding rate 5-8; 0 = use profile default
    int8_t     radioTX;           // TX power dBm; 0 = use profile default
    // Manual GPS coordinates for location sharing without a fix
    float      manualLat;         // decimal degrees; 0.0 = not set
    float      manualLon;
    // Packet forwarding and path preferences
    bool       autoForward;       // forward received mesh packets (default true)
    uint8_t    pathHashSz;        // preferred path hash size: 0/1 = 1-byte, 2 = 2-byte
    // Timezone: whole-hour UTC offset applied to all time displays (-11 to +11)
    int8_t     timezoneOffsetHours;
    // Scope / region tag for flood-scoped transmissions (empty = no scope)
    char       scopeTag[16];
    // LoRa duty cycle: SX1262 hardware RX duty cycle for power saving
    bool       loraDutyCycle;   // true = hardware duty cycle (46ms RX / 469ms sleep)
    bool       rxBoost;         // true = SX1262 boosted RX gain (~2-3 dB, +0.7 mA)
    // CPU governor: controls setCpuFrequencyMhz based on display state
    // 0=PowerSave(40/40/40) 1=Medium(80/40/40) 2=Normal(240/80/80) 3=Turbo(240/240/240)
    uint8_t    cpuGovernor;
    // Touch calibration: linear correction applied to raw GT911 portrait coords.
    // screen_x = touchCalXOff + py * touchCalXScale        (identity: XScale=1, XOff=0)
    // screen_y = touchCalYOff + (239-px) * touchCalYScale  (identity: YScale=1, YOff=0)
    float      touchCalXScale;
    float      touchCalXOff;
    float      touchCalYScale;
    float      touchCalYOff;
};

namespace config {
    void init();
    void save();
    const Config& get();

    void setCallsign(const char* cs);
    void setRegion(const char* reg);
    void setChannel(int idx, const char* name, const char* psk, const char* shortname, const char* scope = "");
    void setChannelNotify(int idx, bool notify);

    // Returns time(nullptr) adjusted by timezoneOffsetHours.
    // Use with gmtime_r() wherever local time needs to be displayed.
    time_t localEpoch();

    // Write calibration coefficients and persist (NVS + SD).
    void setTouchCal(float xScale, float xOff, float yScale, float yOff);
}

}  // namespace ops