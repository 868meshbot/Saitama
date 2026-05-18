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