# Hardware Reference — Saitama

## LilyGo T-Deck

### MCU
- **ESP32-S3FN16R8** (Espressif)
- Dual-core Xtensa LX7 @ 240 MHz
- 16MB Flash (Quad SPI)
- 8MB PSRAM (Octal SPI / OPI)
- Wi-Fi 802.11 b/g/n (2.4 GHz)
- Bluetooth 5.0 + BLE

### Display
- **ST7789** IPS LCD
- 2.8" diagonal
- 320×240 pixels (landscape)
- SPI interface (shared with LoRa)
- Capacitive touch: **GT911** (I2C)
- Backlight: GPIO 2

### Keyboard
- **BBQ10KB** (ESP32-C3 I2C slave)
- Blackberry-style QWERTY
- I2C address: 0x1F (default)
- SDA: GPIO 18, SCL: GPIO 8

### Trackball
- 5 GPIO inputs (active LOW, pull-up)
- Up: GPIO 3
- Down: GPIO 15
- Left: GPIO 21
- Right: GPIO 43
- Press (center click): GPIO 44

### LoRa Radio
- **Semtech SX1262**
- Frequencies: 433/868/915 MHz (device-dependent)
- SPI: SCK=GPIO40, MISO=GPIO41, MOSI=GPIO42
- CS: GPIO 9
- RST: GPIO 12
- DIO1 (interrupt): GPIO 14
- BUSY: GPIO 13

### Audio
- Microphone: analog, GPIO 46 (conflicts with GPIO 0 button)
- Speaker: I2S DAC, GPIO 7 (BCLK), GPIO 47 (DOUT)

### SD Card
- SPI mode
- CS: GPIO 11
- Shared SPI bus with display and LoRa
- Supports FAT32, up to 32GB tested

### GPS (T-Deck Plus only)
- Built-in GPS module on Grove interface
- UART: TX=GPIO17, RX=GPIO16
- 9600 baud, NMEA protocol
- **Note**: Grove interface unusable on Plus (pins reassigned to GPS)

### Power
- USB-C (5V input)
- LiPo battery connector (3.7V)
- Battery voltage ADC (specific GPIO TBD by board revision)
- Power switch on side

### Boot / DFU Mode
1. Connect USB-C
2. Toggle power switch to ON
3. Press and hold trackball center
4. Press reset button (left side)
5. Release both
6. Device is now in download mode

### SPI Bus Sharing

Display, LoRa, and SD card share the same SPI bus:
```
SPI host: SPI2 (FSPI)
SCK:  GPIO 40
MISO: GPIO 41
MOSI: GPIO 42

CS lines:
  Display: GPIO 4
  LoRa:    GPIO 9
  SD card: GPIO 11
```

Only one device active at a time (CS deselects others). The TFT_eSPI
library handles this, but care is needed when accessing LoRa and SD
concurrently. MeshCore and the map engine must coordinate SPI access.

### T-Deck vs T-Deck Plus

| Feature | T-Deck (OG) | T-Deck Plus |
|---------|-------------|-------------|
| GPS | No (external only) | Built-in on Grove |
| Grove port | Available | Occupied by GPS |
| LoRa antenna | Internal | Internal |
| Price | ~$43 | ~$53 |

Both share the same ESP32-S3, display, keyboard, trackball, and LoRa radio.
The only hardware difference is the GPS module.

### Key Resources

- [LilyGo T-Deck GitHub](https://github.com/Xinyuan-LilyGO/T-Deck)
- [LilyGo Wiki](https://wiki.lilygo.cc/get_started/en/Wearable/T-Deck/T-Deck.html)
- [ESP32-S3 Datasheet](https://www.espressif.com/en/products/socs/esp32-s3)
- [SX1262 Datasheet](https://www.semtech.com/products/wireless-rf/lora-transceivers/sx1262)
- [MeshCore Hardware Support](https://meshcore.io/flasher)
## M5Stack Cardputer ADV + Cap LoRa-1262

Built with `pio run -e cardputer-adv`. Board code: `src/hardware/cardputer/Board.cpp`;
feature switches and capacities: `src/features.h`. Pins come from M5Stack's
M5Unified, M5GFX and M5Cardputer sources and the Cap LoRa-1262 example; they
have not been checked on hardware yet.

### MCU

Stamp-S3A (ESP32-S3FN8): 8 MB flash, **no PSRAM** (Espressif's "FN8" has no
"R"). Everything that lives in PSRAM on the T-Deck is in internal RAM here, so
lists are smaller (100 contacts, 60 repeaters, 60 chat messages in RAM) and
the map, spectrum and other large-buffer screens are left out.

| Signal | GPIO | Notes |
|---|---|---|
| TFT MOSI / SCLK | 35 / 36 | ST7789V2 135x240, own SPI host (HSPI), offset 52/40 |
| TFT CS / DC / RST | 37 / 34 / 33 | |
| TFT backlight | 38 | ledc PWM |
| I2C SDA / SCL | 8 / 9 | TCA8418 keyboard (0x34), PI4IOE5V6408 expander on the cap (0x43), ES8311 codec (0x18) |
| Keyboard INT | 11 | TCA8418, active low; also the light-sleep wake |
| G0 button | 0 | Select; wakes from deep sleep |
| LoRa NSS / DIO1 / RST / BUSY | 5 / 4 / 3 / 6 | SX1262 on the Cap LoRa-1262, TCXO 3.0 V |
| LoRa + SD SPI | SCK 40, MOSI 14, MISO 39 | Shared bus |
| LoRa RF switch | expander P0 | Driven high at boot |
| SD CS | 12 | |
| GPS RX / TX | 15 / 13 | ATGM336H on the cap, 115200 baud |
| Battery | 10 | ADC, VBAT / 2 |
| Speaker I2S | BCK 41, WS 43, DOUT 42 | ES8311 codec — not driven yet (no sounds) |

### Keyboard

56 keys in a 4x14 layout read through the TCA8418. Fn + `;` `.` `,` `/` are the
arrow keys and stand in for the trackball; Fn + `` ` `` is Esc. There is no
touchscreen: rows and popup buttons that the T-Deck only offers to touch are
reached with the arrow keys (`src/ui/cardputer/KeyNav.cpp`).
