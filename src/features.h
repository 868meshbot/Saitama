// Saitama — features.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Per-board feature switches and capacities. The board is chosen in
// platformio.ini (-DOPS_BOARD_CARDPUTER for the M5Stack Cardputer ADV; the
// T-Deck Plus is the default). Shared code tests the OPS_FEAT_* switches,
// never the board name, and the files of a switched-off feature are also
// left out by that env's build_src_filter.

#pragma once

#if defined(OPS_BOARD_CARDPUTER)

// Cardputer ADV: 240x135, keyboard only, no PSRAM (ESP32-S3FN8).
#define OPS_BOARD_NAME "M5Stack Cardputer ADV"
#define OPS_FEAT_MAP          0   // offline tile map (needs PSRAM tile cache)
#define OPS_FEAT_SKYPLOT      0   // GPS skyplot screen (text status instead)
#define OPS_FEAT_RADIO_TOOLS  0   // spectrum/waterfall, channel scan, signal generator
#define OPS_FEAT_TRACE        0   // hop-by-hop trace screen
#define OPS_FEAT_GAMES        0   // 2048, Snake, 0x0
#define OPS_FEAT_PCAP         0   // packet capture to SD
#define OPS_FEAT_FOXHUNT      0   // BT foxhunt
#define OPS_FEAT_MEDIA        0   // MP3 player, picture viewer, file manager
#define OPS_FEAT_POWER        0   // battery history / power estimates screen
#define OPS_FEAT_BOOTANIM     0   // Saitama mesh animation screensaver
#define OPS_FEAT_IDLE_SAVER   1   // idle screensaver (mesh rain by default)
#define OPS_SS_DIM_MIN        128 // idle-saver backlight floor (lower is too dark here)
#define OPS_FEAT_LAUNCHER_STYLE 1 // Settings > Launcher: Menu / Carousel
#define OPS_LAUNCHER_DEFAULT  1   // launcherStyle default: 1 = Carousel
#define OPS_FEAT_SOUND        1   // I2S speaker through the ES8311 codec
#define OPS_SPK_BCLK          41  // pins and codec setup from M5Unified
#define OPS_SPK_WS            43
#define OPS_SPK_DOUT          42
#define OPS_SPK_ES8311        1   // codec at 0x18 on the internal I2C (8/9)
#define OPS_FEAT_TOUCH        0   // GT911 touchscreen
#define OPS_FEAT_KB_BACKLIGHT 0   // keyboard backlight
#define OPS_FEAT_BLE          1   // BLE companion (the largest internal-RAM user)
#define OPS_HAS_PSRAM         0
#define OPS_HAS_POWER_RAIL    0   // no switched peripheral rail like the T-Deck GPIO10
#define OPS_KEYBOARD_ONLY     1   // no trackball/touch: KeyNav, keys wake the screensaver
#define OPS_WAKE_KEY_PIN      11  // TCA8418 INT: a key press wakes light sleep
#define OPS_BL_PWM_HZ         256 // backlight driver goes dark at 5 kHz below ~95 % duty (M5GFX uses 256 Hz)
#define OPS_SAVER1_NAME       "Mesh rain"   // screensaver style 1
#define OPS_DEFAULT_SAVER     1             // mesh rain unless set otherwise
#define OPS_FEAT_ADVERT_APP   0   // adverts live in Settings > Advert

// SD card: own CS, SPI shared with the LoRa cap (SCK 40, MISO 39, MOSI 14).
#define OPS_SD_CS    12
#define OPS_SD_SCK   40
#define OPS_SD_MISO  39
#define OPS_SD_MOSI  14

#define OPS_CONTACTS_CAPACITY  100
#define OPS_REPEATERS_CAPACITY 60
#define OPS_CHAT_HISTORY       60    // messages held in RAM across all chats

// Layout (240x135)
#define OPS_TOP_H        22   // screen title bar
#define OPS_LIST_ROW_H   30   // contact / repeater / channel rows
#define OPS_LIST_COL_H   28   // their two-line text column
#define OPS_AVATAR_SZ    0    // no initials circle

#else

// T-Deck Plus: everything.
#define OPS_BOARD_NAME "LilyGo T-Deck Plus"
#define OPS_FEAT_MAP          1
#define OPS_FEAT_SKYPLOT      1
#define OPS_FEAT_RADIO_TOOLS  1
#define OPS_FEAT_TRACE        1
#define OPS_FEAT_GAMES        1
#define OPS_FEAT_PCAP         1
#define OPS_FEAT_FOXHUNT      1
#define OPS_FEAT_MEDIA        1
#define OPS_FEAT_POWER        1
#define OPS_FEAT_BOOTANIM     1
#define OPS_FEAT_IDLE_SAVER   1
#define OPS_SS_DIM_MIN        60
#define OPS_FEAT_LAUNCHER_STYLE 0
#define OPS_LAUNCHER_DEFAULT  0
#define OPS_FEAT_SOUND        1
#define OPS_SPK_BCLK          7   // MAX98357A amplifier, no setup
#define OPS_SPK_WS            5
#define OPS_SPK_DOUT          6
#define OPS_SPK_ES8311        0
#define OPS_FEAT_TOUCH        1
#define OPS_FEAT_KB_BACKLIGHT 1
#define OPS_FEAT_BLE          1
#define OPS_HAS_PSRAM         1
#define OPS_HAS_POWER_RAIL    1   // GPIO10 powers every peripheral
#define OPS_KEYBOARD_ONLY     0
#define OPS_BL_PWM_HZ         5000
#define OPS_SAVER1_NAME       "Saitama"
#define OPS_DEFAULT_SAVER     0             // clock
#define OPS_FEAT_ADVERT_APP   1   // Advertise app on the launcher

// SD card: CS 39 on the SPI bus shared with the TFT and LoRa.
#define OPS_SD_CS    39
#define OPS_SD_SCK   40
#define OPS_SD_MISO  38
#define OPS_SD_MOSI  41

#define OPS_CONTACTS_CAPACITY  250
#define OPS_REPEATERS_CAPACITY 250
#define OPS_CHAT_HISTORY       200

// Layout (320x240)
#define OPS_TOP_H        28
#define OPS_LIST_ROW_H   60
#define OPS_LIST_COL_H   44
#define OPS_AVATAR_SZ    44

#endif
