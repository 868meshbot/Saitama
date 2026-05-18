# Map System Design — Saitama

## Overview

Offline GPS map rendering on a 320×240 IPS display. Tiles stored on SD card.
Nodes (contacts, repeaters, self) rendered as overlay markers.

## Tile Coordinate System

Uses the standard OpenStreetMap slippy map tile naming:

```
URL: https://tile.openstreetmap.org/{z}/{x}/{y}.png
File: /map/{z}/{x}/{y}.png
```

Where:
- `z` = zoom level (0–18)
- `x` = horizontal tile index (0 to 2^z − 1)
- `y` = vertical tile index (0 to 2^z − 1)

Each tile is 256×256 pixels in PNG format.

## Zoom Levels and Coverage

| Zoom | km/px     | Coverage per tile | Tiles visible |
|------|-----------|-------------------|----------------|
| 6    | ~8.8      | ~2'250 km         | 1              |
| 10   | ~340      | ~350 km           | 1              |
| 12   | ~85       | ~87 km            | 1              |
| 14   | ~21       | ~22 km            | 2×2            |
| 16   | ~5.3      | ~5.5 km           | 3×2            |
| 18   | ~1.3      | ~1.4 km           | 4×3            |

Default zoom: 10 (country-level overview for Luxembourg).

## Rendering Strategy

### Visible Area Calculation

```
centerTileX, centerTileY = latLngToTile(centerLat, centerLng, zoom)
```

The visible viewport (320×196 effective) shows:
- The tile containing the center point
- Adjacent tiles needed to fill the viewport

### Tile Loading

1. Calculate which tiles are visible at current center/zoom
2. Check PSRAM cache first (up to 8 tiles, LRU eviction)
3. If not cached, load PNG from SD card
4. Decode PNG to LVGL image format (lv_img_dsc_t)
5. Blit to screen canvas at correct offset
6. Return loaded tile to cache

### SD Card Performance

Typical SD card read: ~2MB/s on ESP32-S3 SPI mode.
A 256×256 PNG tile is ~15–50KB compressed.
Decode time: ~30–80ms per tile.

Target: render visible tiles within 200ms of pan/zoom event.
For zoom levels showing 1–2 tiles, this is achievable.
For zoom 18 showing 12 tiles, we progressively render.

### Progressive Rendering

```
Frame 1: Render center tile (instant feedback)
Frame 2: Render adjacent tiles (fill viewport)
Frame 3: Render remaining visible tiles
```

The user sees the map appear within 50ms, then detail fills in.

## Node Markers

Nodes are drawn as LVGL objects on top of the tile layer:

| Type | Symbol | Colour | Size |
|------|--------|--------|------|
| Self | ★ | ACCENT (#58a6ff) | 8px |
| Contact online | ● | GREEN (#3fb950) | 6px |
| Contact offline | ● | TEXT_MUTED (#8b949e) | 6px |
| Repeater | ● | ORANGE (#d29922) | 8px |

Each marker shows a label (callsign) offset above the dot.

### Tapping a Node

Touch or trackball-select a node marker → popup:

```
┌─────────────────┐
│ ★ You           │
│ 49.6117, 6.1300 │
│ Battery: 78%    │
│ [Close]         │
└─────────────────┘
```

## Preparing Map Tiles

Users download tiles before going off-grid. Recommended tool:

```bash
# Download tiles for Luxembourg at zoom 8-14
for z in 8 9 10 11 12 13 14; do
  for x in $(seq 132 136); do
    for y in $(seq 84 88); do
      mkdir -p /map/$z/$x/
      curl -s -o /map/$z/$x/$y.png \
        "https://tile.openstreetmap.org/$z/$x/$y.png"
      sleep 1  # respect tile server rate limits
    done
  done
done
```

A helper script `scripts/download_tiles.py` will be provided to automate
this with proper bounds calculation and rate limiting.

## Future Improvements (Post v0.1)

- Vector tile rendering (smaller storage, infinite zoom)
- Route tracing between nodes
- Tile pre-fetching (load adjacent tiles in background)
- Web-based tile downloader GUI
- Map rotation (compass heading)