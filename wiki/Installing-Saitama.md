# Installing Saitama

## What you need

- **T-Deck:** a LilyGo T-Deck Plus (the plain T-Deck should work but is untested).
- **Cardputer:** an M5Stack Cardputer ADV with the Cap LoRa-1262 module fitted.
- A USB-C data cable (some cables only charge).
- A microSD card, FAT32. Optional, but strongly recommended: it holds your message
  history and the backup of your identity, contacts and settings.
- An antenna on the radio before you send anything. Transmitting without one can
  damage the radio.

## Download

Get the files from the
[latest release](https://github.com/868meshbot/Saitama/releases/latest). Each
release has two files per device:

| File | Flash at | Use it for |
|---|---|---|
| `saitama-<device>-X.Y.Z-merged.bin` | `0x0` | First install, or recovery. Bootloader, partition table and app in one file. |
| `saitama-<device>-X.Y.Z.bin` | `0x10000` | Updating a device that already runs Saitama, or installing from M5Launcher. |

`<device>` is `t-deck-plus` or `cardputer-adv`. Don't mix them up: each file only
runs on its own device.

## Flashing tools

Any ESP32-S3 flasher works. Two common ones:

- **In a browser:** [ESP Tool](https://espressif.github.io/esptool-js/) in Chrome or
  Edge. Connect, add the file at the address from the table above, and press
  Program.
- **From a terminal:** [esptool](https://github.com/espressif/esptool)
  (`pip install esptool`).

## T-Deck Plus

1. Put the T-Deck into download mode: hold the trackball button down, press the
   reset button on the side, then let go of both.
2. Connect it by USB.
3. Flash the merged file at `0x0` (command below).
4. Press reset. The Saitama splash appears, then the home screen.

```bash
esptool.py --chip esp32s3 --port <port> write_flash 0x0 saitama-t-deck-plus-1.6.2-merged.bin
```

`<port>` is something like `/dev/ttyACM0` (Linux), `/dev/cu.usbmodem…` (macOS) or
`COM5` (Windows).

To update later, flash the app-only `.bin` at `0x10000` the same way.

## Cardputer ADV

**With a USB flasher:**

1. Hold the **G0** button while plugging in USB. This starts download mode.
2. Flash the merged file at `0x0` (command below; the Cardputer ADV has 8 MB of
   flash).
3. Unplug and plug back in (or press reset).

```bash
esptool.py --chip esp32s3 --port <port> write_flash 0x0 saitama-cardputer-adv-1.6.2-merged.bin
```

**With M5Launcher:** copy `saitama-cardputer-adv-X.Y.Z.bin` (the app-only file) to
the SD card and install it from M5Launcher's file browser.

## Updating

- Flashing the **app-only** file keeps everything on the device: settings, identity,
  contacts.
- Flashing the **merged** file can reset the settings held on the device. With an SD
  card in, Saitama restores them from the `/ops/` backup on the card at the next
  boot.

Whichever you use, it's worth keeping the SD card in. See
[Backup and security](Backup-and-Security).

## Building it yourself

```bash
git clone --recurse-submodules https://github.com/868meshbot/Saitama.git
cd Saitama
pio run -e t-deck -t upload          # T-Deck / T-Deck Plus
pio run -e cardputer-adv -t upload   # Cardputer ADV
```

This needs [PlatformIO](https://platformio.org/).
