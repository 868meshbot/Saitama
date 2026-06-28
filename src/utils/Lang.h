// Saitama — Lang.h
// Copyright 2026 Saitama — GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace ops {
namespace lang {

enum UiLang : uint8_t {
    LANG_EN = 0,
    LANG_IT,
    LANG_FR,
    LANG_DE,
    LANG_ES,
    LANG_COUNT
};

enum TrKey : uint8_t {
    TR_DEVICE_NAME = 0,
    TR_SHARE_CONTACT,
    TR_GEN_IDENTITY,
    TR_CHANNELS,
    TR_RADIO,
    TR_POWER,
    TR_LORA_DUTY,
    TR_CPU_GOV,
    TR_BRIGHTNESS,
    TR_THEME,
    TR_FONT,
    TR_BLUETOOTH,
    TR_SPEAKER,
    TR_GPS,
    TR_KB_LIGHT,
    TR_KB_LAYOUT,
    TR_DATE_TIME,
    TR_TIMEZONE,
    TR_FW_UPDATE,
    TR_AUTO_ADD,
    TR_SCR_TIMEOUT,
    TR_SCR_OFF,
    TR_VOLUME,
    TR_NOTIFICATIONS,
    TR_NOTIFY_SOUND,
    TR_SAVE_MSGS,
    TR_SHOW_HOPS,
    TR_SHOW_RSSI,
    TR_LOCATION,
    TR_BACKUP,
    TR_RETURN,
    TR_LANGUAGE,
    TR_ON,
    TR_OFF,
    TR_COUNT
};

// Native language names (ASCII-safe) — indexed by UiLang
extern const char* const kLangNames[LANG_COUNT];

const char* tr(TrKey key);

}  // namespace lang
}  // namespace ops
