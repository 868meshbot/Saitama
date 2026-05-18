# UI Design — Saitama

All screens designed for **320x240 landscape** (T-Deck). Every pixel counts.

## Screen Flow

```
                    ┌──────────┐
                    │   Home   │ ← default on boot
                    │ (Chat)   │
                    └────┬─────┘
                         │
          ┌──────────┬───┴───┬──────────┐
          ▼          ▼       ▼          ▼
     ┌─────────┐┌────────┐┌────────┐┌────────┐
     │  Map    ││  DMs   ││Settings││Terminal│
     └─────────┘└────────┘└────────┘└────────┘
          │          │       │
          ▼          ▼       ▼
     ┌─────────┐┌────────┐┌────────────┐
     │Tile View││Contact ││Radio Config│
     │+ Nodes  ││Profile ││Display     │
     └─────────┘└────────┘│Security    │
                          └────────────┘
```

## 1. Home Screen (Chat)

```
+----------------------------------+ 240px
| [Public] [Ch1] [DM]  🔋 🔊 [⚙] |  Status bar 22px
|----------------------------------|
|                                  |
|  ┌─────────────────────────┐    |
|  │ Alice                    │    |
|  │ Anyone near LU?          │    |
|  └─────────────────────────┘    |
|                                  |
|         ┌────────────────────┐   |
|         │You                  │   |
|         │Yes, LUX area       │   |
|         └────────────────────┘   |
|                                  |
|  ┌─────────────────────────┐    |
|  │ Bob                      │    |
|  │ Signal check on R4       │    |
|  └─────────────────────────┘    |
|                                  |
|----------------------------------|
| [Type message...          ] [➤] |  Input bar 34px
+----------------------------------+ 320px wide
```

- **Status bar**: Channel tabs left, status icons right (battery, sound, gear)
- **Message list**: Scrollable, bubbles aligned left (others) / right (self)
- **Input bar**: Text area + send button
- **Navigation**: Trackball scrolls messages, press = focus input
- **Tab switch**: Tap channel tab or left/right on trackball when not in input

### Bubble styling
- Others: `BG_CARD` background, left-aligned, username in `ACCENT`
- Self: `PRIMARY` background (dark blue), right-aligned, white text
- Timestamp: `TEXT_MUTED`, small font, below bubble
- Delivery indicator: ✓✓ (sent), ✓✓ blue (delivered) — right of timestamp

## 2. Map Screen

```
+----------------------------------+ 240px
| [🗺] Z:10  🔍+ 🔍-  [Home]     |  Map bar 22px
|----------------------------------|
|                                  |
|     ┌───────────────────┐        |
|     │  Tile grid         │        |
|     │  (from SD card)    │        |
|     │                    │        |
|     │     ● Alice        │        |
|     │        ● R4 (rep)  │        |
|     │  ★ You             │        |
|     │                    │        |
|     └───────────────────┘        |
|                                  |
|----------------------------------|
| Lat: 49.6117 Lng: 6.1300  [📡] |  Info bar 22px
+----------------------------------+ 320px wide
```

- **Map bar**: Zoom level, zoom +/− buttons, back to Home
- **Tile view**: 320x196 pixel area, renders 1–4 tiles depending on zoom
- **Nodes**: Circles on map, color-coded:
  - ★ (star) = self (ACCENT/blue)
  - ● (dot) = contact (GREEN)
  - ● (dot) = repeater (ORANGE)
  - Tap node → info popup with name, RSSI, distance
- **Info bar**: Current coordinates, nearby repeaters count
- **Navigation**: Trackball = pan map, center press = select node, zoom +/- buttons
- **Touch**: Drag to pan, pinch to zoom (if touchscreen responsive enough)

### Tile rendering strategy
At zoom 10: one 256x256 tile covers ~350km — fits in view with padding
At zoom 14: ~22km per tile — need 2x2 grid for 320x196 area
At zoom 16: ~1.4km per tile — need 3x2 grid

Tiles loaded on demand from SD card `/map/{z}/{x}/{y}.png`
Cache up to 8 tiles in PSRAM to reduce SD reads

## 3. DM / Contacts Screen

```
+----------------------------------+ 240px
| [← Back]  Direct Messages        |  Header 22px
|----------------------------------|
|                                  |
|  ● Alice          2m ago  RSSI:-42 |
|  ● Bob            15m ago RSSI:-58 |
|  ○ Charlie        2h ago  RSSI:--  |
|  ● R4 (repeater)  Always  RSSI:-31 |
|                                  |
|                                  |
|                                  |
|                                  |
|                                  |
|                                  |
|                                  |
|                                  |
+----------------------------------+ 320px wide
```

- **Contact row**: Name, last seen time, RSSI (if online)
- ● = online (green dot), ○ = offline (muted dot)
- Tap/select contact → opens DM chat view (same layout as Home but one-on-one)
- Repeater entries show as read-only with management option

## 4. Settings Screen

```
+----------------------------------+ 240px
| [← Back]  Settings               |  Header 22px
|----------------------------------|
|                                  |
|  Radio                           |
|  ├── Region        [EU868  ▾]   |
|  ├── Channel       [0     ▾]    |
|  ├── TX Power      [20dBm ▾]    |
|  └── Modem Config  [BW125/SF9]  |
|                                  |
|  Device                          |
|  ├── Callsign     [OPS-0001]    |
|  ├── Brightness   [══════─○  ]  |
|  ├── Screen Sleep  [30s   ▾]   |
|  ├── Sound         [ON    ▾]    |
|  └── Theme         [Dark  ▾]    |
|                                  |
|  Network                         |
|  ├── BLE          [ON    ▾]     |
|  └── WiFi SSID    [      ]      |
|                                  |
|  About                           |
|  └── Saitama v0.1.0          |
+----------------------------------+ 320px wide
```

- Scrollable list with section headers
- Trackball navigates up/down, center press = edit field
- Text fields use on-screen keyboard (T-Deck has physical keyboard)
- Dropdown fields cycle through options on press

## 5. Terminal Screen

```
+----------------------------------+ 240px
| [← Back]  Terminal                |  Header 22px
|----------------------------------|
| > repeater.scan                   |
| Found 3 repeaters:                |
|   R4  -42dB  1.2km                |
|   R7  -58dB  3.8km                |
|   R12 -31dB  0.4km                |
| >                                 |
|                                   |
|                                   |
|                                   |
|                                   |
|                                   |
|                                   |
|----------------------------------|
| [command input............] [↵]  |  Input bar 28px
+----------------------------------+ 320px wide
```

- Full MeshCore CLI access
- Monospace font
- Multi-color output (errors in RED, warnings in ORANGE, data in GREEN)
- Command history (up to 20 entries, stored in SPIFFS)
- Scrollable output buffer (up to 200 lines)

## 6. Lock / Sleep Screen

```
+----------------------------------+
|                                  |
|                                  |
|          13:03                   |
|        Thu 23 Apr                |
|                                  |
|     🔋 78%   📶 -42dBm          |
|     🔊 3 nodes                  |
|                                  |
|                                  |
|                                  |
|                                  |
|    Press any key to unlock       |
+----------------------------------+
```

- Shown after screen timeout (configurable, default 30s)
- Shows time, date, battery, signal strength, nearby node count
- Any key / trackball press returns to Home
- Auto-dimming: backlight drops to 30% before full sleep

## Navigation Model

### Trackball
- **Scroll up/down**: Navigate lists, scroll messages
- **Press**: Select / activate focused item
- **Left/Right**: Switch channel tabs (Home), pan (Map)

### Keyboard
- **Full QWERTY**: Type messages, commands, search
- **Enter**: Send message / confirm input
- **Esc**: Back / cancel
- **Tab**: Switch between input and list focus

### Touch
- **Tap**: Select item, focus input
- **Drag**: Scroll lists, pan map
- Touch is secondary — the primary input method is trackball + keyboard

## Colour Reference

```
  BG         #0d1117  ██████  Main background
  BG_CARD    #161b22  ██████  Card/panel background
  TEXT       #e6edf3  ██████  Primary text
  TEXT_MUTED #8b949e  ██████  Secondary text
  ACCENT     #58a6ff  ██████  Interactive elements
  PRIMARY    #0033aa  ██████  Own message bubbles
  GREEN      #3fb950  ██████  Online, success
  RED        #f85149  ██████  Error, offline
  ORANGE     #d29922  ██████  Warning, repeater
  BORDER     #30363d  ██████  Borders, dividers
```

## Font Sizes

On 320x240 at 2.8", readability is critical:
- **Status bar**: 11px
- **Message text**: 13px
- **Timestamp**: 10px
- **Section headers**: 14px bold
- **Input text**: 13px
- **Terminal**: 12px monospace

No font below 10px. If it can't be read outdoors, it shouldn't be there.