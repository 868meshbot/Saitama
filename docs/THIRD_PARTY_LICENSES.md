# Third-Party Licenses — Saitama

Saitama itself is released under the WTFPL (Do What The Fuck You Want To Public License), Version 2. However, this project depends on several third-party libraries with their own licenses. This document lists them.

## Direct Dependencies

### MeshCore (v1.10.0)

- **Source**: https://github.com/meshcore-dev/MeshCore (git submodule)
- **License**: MIT
- **Copyright**: MeshCore contributors
- **Use**: LoRa mesh networking stack, radio communication, encryption

### LVGL (v9.2+)

- **Source**: https://github.com/lvgl/lvgl
- **License**: MIT
- **Copyright**: LVGL Kft and contributors
- **Use**: Embedded graphics library for UI rendering

### TFT_eSPI (v2.5.34)

- **Source**: https://github.com/Bodmer/TFT_eSPI
- **License**: Mixed — FreeBSD (original TFT_eSPI code), BSD (Adafruit_GFX functions), MIT (Adafruit_ILI9341 origin)
- **Copyright**: Bodmer, Adafruit Industries
- **Use**: Display driver for ST7789 320x240 IPS screen

### RadioLib (v7.6.0+)

- **Source**: https://github.com/jgromes/RadioLib
- **License**: MIT
- **Copyright**: Jan Gromes
- **Use**: LoRa radio driver (dependency of MeshCore)

### Adafruit BusIO (v1.16+)

- **Source**: https://github.com/adafruit/Adafruit_BusIO
- **License**: MIT
- **Copyright**: Adafruit Industries
- **Use**: I2C/SPI abstraction for sensor communication

### RTClib (v2.1+)

- **Source**: https://github.com/adafruit/RTClib
- **License**: MIT
- **Copyright**: Adafruit Industries
- **Use**: Real-time clock driver (I2C RTC chips)

### Melopero RV3028

- **Source**: https://github.com/melopero/Melopero-RV3028
- **License**: MIT
- **Copyright**: Melopero
- **Use**: RTC driver for RV3028 chip (dependency of MeshCore)

### Crypto (rweather/arduinolibs)

- **Source**: https://github.com/rweather/arduinolibs
- **License**: Unlicense (public domain)
- **Copyright**: Rhys Weatherley
- **Use**: Cryptographic primitives (Ed25519, AES, etc., dependency of MeshCore)

### ArduinoJson

- **Source**: https://github.com/benoitblanchon/ArduinoJson
- **License**: MIT
- **Copyright**: Benoit Blanchon
- **Use**: JSON parsing (if used in future config upgrades)

### CayenneLPP

- **Source**: https://github.com/ElectronicCats/CayenneLPP
- **License**: MIT
- **Copyright**: Electronic Cats
- **Use**: Cayenne LPP payload encoding (dependency of MeshCore for telemetry)

### ESP32 Arduino Core

- **Source**: https://github.com/espressif/arduino-esp32
- **License**: LGPL-2.1 (core) + various per-component licenses
- **Copyright**: Espressif Systems
- **Use**: Arduino framework for ESP32-S3

### PlatformIO Build System

- **Source**: https://github.com/platformio
- **License**: Apache 2.0
- **Copyright**: PlatformIO
- **Use**: Build toolchain and dependency management

## Transitive Dependencies (via MeshCore)

### AsyncElegantOTA

- **Source**: https://github.com/ayushsharma82/AsyncElegantOTA
- **License**: MIT
- **Copyright**: Ayush Sharma
- **Note**: Only included in ESP32 builds

### Adafruit LittleFS (STM32)

- **Source**: https://github.com/adafruit/Adafruit_LittleFS
- **License**: MIT
- **Copyright**: Adafruit Industries
- **Note**: Only used in STM32 builds (not in our ESP32 build)

## Font Licenses

### GFX Free Fonts (within TFT_eSPI)

Various fonts bundled with TFT_eSPI under the FreeBSD license. See `TFT_eSPI/Fonts/GFXFF/license.txt` for details.

## Summary

| Library | License | Copyleft? |
|---------|---------|-----------|
| MeshCore | MIT | No |
| LVGL 9 | MIT | No |
| TFT_eSPI | FreeBSD/BSD/MIT | No |
| RadioLib | MIT | No |
| Adafruit BusIO | MIT | No |
| RTClib | MIT | No |
| Melopero RV3028 | MIT | No |
| Crypto | Unlicense | No |
| ArduinoJson | MIT | No |
| CayenneLPP | MIT | No |
| ESP32 Arduino Core | LGPL-2.1 | Yes (LGPL) |
| PlatformIO | Apache 2.0 | No |

All dependencies are permissive (MIT, BSD, FreeBSD, Apache, Unlicense) except the ESP32 Arduino Core which is LGPL-2.1. LGPL allows linking without requiring our code to be under the same license, so the WTFPL license for Saitama itself is compatible.