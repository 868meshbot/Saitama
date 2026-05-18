# Resources — Saitama

## MeshCore

- **Repository**: https://github.com/meshcore-dev/MeshCore
- **Protocol**: Lightweight C++ library for multi-hop LoRa mesh routing
- **License**: MIT
- **Key examples**: companion_radio, simple_repeater, simple_room_server, simple_secure_chat
- **Docs**: https://docs.meshcore.io
- **FAQ**: https://github.com/meshcore-dev/MeshCore/blob/main/docs/faq.md
- **Discord**: https://discord.gg/BMwCtwHj5V
- **Flasher**: https://meshcore.io/flasher

## MeshOS (the product we're replacing)

- **Website**: https://meshcore.co.uk/meshos.html
- **Features**: Chat, GPS maps, encrypted comms, repeater mgmt, notifications, terminal
- **License**: Proprietary (£8/device license)
- **Current version**: 1.15.0
- **Problem**: Closed-source, paid, vendor lock-in. MeshCore dev attempting takeover of the MeshOS project direction.

## Alternative Firmwares

### Aurora
- **Repo**: https://forge.hackers.town/Wrewdison/Aurora
- **License**: MIT
- **Platform**: PlatformIO, ESP32 Arduino
- **Features**: Chat, contacts, BLE companion, config import/export
- **Missing**: No map, no GPS, no room server, no repeater mgmt
- **Author**: Wrewdison
- **Mailing list**: aurora@freelists.org

### Meck
- **Repo**: https://github.com/pelgraine/Meck
- **License**: Not specified
- **Platform**: PlatformIO, vibecoded with Claude AI
- **Focus**: BLE/WiFi companion for T-Deck Pro
- **Based on**: MeshCore v1.11
- **Note**: Also vibecoded, similar disclosure approach

## LilyGo T-Deck

- **Official repo**: https://github.com/Xinyuan-LilyGO/T-Deck
- **Wiki**: https://wiki.lilygo.cc/get_started/en/Wearable/T-Deck/T-Deck.html
- **Pin reference**: See `docs/HARDWARE.md` or utilities.h in the LilyGo repo
- **ESP32-S3 datasheet**: https://www.espressif.com/en/products/socs/esp32-s3
- **SX1262 datasheet**: https://www.semtech.com/products/wireless-rf/lora-transceivers/sx1262

## UI Framework

- **LVGL**: https://lvgl.io/ (v9.x)
- **LVGL docs**: https://docs.lvgl.io/
- **TFT_eSPI**: https://github.com/Bodmer/TFT_eSPI
- **LovyanGFX T-Deck config**: https://github.com/lovyan03/LovyanGFX/discussions/451

## GPS

- **TinyGPSPlus**: https://github.com/mikalhart/TinyGPSPlus
- **NMEA protocol**: Standard GPS sentence format, 9600 baud

## Map Tiles

- **OSM tile system**: https://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
- **Tile download**: https://wiki.openstreetmap.org/wiki/Downloading_data
- **Rendering**: https://wiki.openstreetmap.org/wiki/Mod_tile

## Community

- **MeshCore Discord**: https://discord.gg/BMwCtwHj5V
- **r/meshcore**: https://www.reddit.com/r/meshcore/ (if it exists)

## Related Projects

- **Meshtastic**: https://github.com/meshtastic/firmware (different protocol, similar device space)
- **Reticulum**: https://github.com/markqvist/Reticulum (advanced mesh networking)
- **ATAK integration**: MeshCore has ATAK support planned