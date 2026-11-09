# Map and GPS

## GPS

**Settings > GPS** sets the receiver to **Off**, **Intermittent** (wakes now and then
to save battery) or **On**. The GPS needs a view of the sky for its first fix, which
can take a few minutes.

With a fix, the clock is set from GPS, and your position can be shared:

- **Settings > Location Sharing**: include your position in your adverts.
- **Settings > Telemetry**: whether others may ask your device for battery level and
  position.

The **GPS** app shows the fix:

- **T-Deck:** a sky plot of the satellites in view, with position, altitude and time.
- **Cardputer:** the same information as text.

## Offline map (T-Deck)

The **Map** app shows map tiles from the SD card, with the positions of nodes that
share their location.

![Map](https://raw.githubusercontent.com/868meshbot/Saitama/main/screenshots/maps.png)

### Putting tiles on the card

Tiles go on the FAT32 SD card, one folder per style, in the usual
`{zoom}/{x}/{y}.png` layout:

```
/maps/osm/day/{z}/{x}/{y}.png      standard OpenStreetMap
/maps/osm/night/{z}/{x}/{y}.png    optional dark style
```

**With the included script** (from a copy of the Saitama repository):

```bash
python3 scripts/download_tiles.py \
    --output /Volumes/SD/maps/osm \
    --lat 51.5 --lng -0.1 --radius 20 --zoom 10-14
```

Use your own latitude and longitude. `--radius` is in kilometres. Stay within the
[OpenStreetMap tile usage policy](https://operations.osmfoundation.org/policies/tiles/):
the script is limited to 2 requests a second, and large areas at high zoom are a lot
of tiles.

**With a GUI tool:** [map-tiles-downloader](https://github.com/tekk/map-tiles-downloader).
Export standard OSM tiles and put them under `/maps/osm/day`.

### Night tiles

**Settings > Map Style** is **Day**, **Night** or **Auto** (the default: night tiles
while Night Mode is on). Without night tiles on the card, the day tiles are shown
colour-inverted at night, so no extra download is needed.

## Offline map on the Cardputer

The Cardputer has its own, lighter map. It reads small **JPEG** tiles from the SD
card (the T-Deck's PNG tiles are too big to decode without PSRAM) and draws them
straight to the screen. It shows repeaters (purple), contacts (red) and you (blue),
with the zoom level and the map centre on the bottom line. There is no night style.

| Key | Does |
|---|---|
| `;` `.` `,` `/` (arrows) | Move the map |
| `=` / `-` | Zoom in / out |
| `c` | Centre on your position |
| Backspace or Esc | Back to the launcher |

With no GPS fix and no manual position, the map opens on the tiles that are on the
card, and zoom stops at the levels the card has.

### Putting tiles on the card

Tiles go on the SD card as:

```
/maps/osm/{z}/{x}/{y}.jpg
```

They must be ordinary (baseline) RGB JPEGs, 256 x 256. The included script
`scripts/map_tiles_cardputer.py` (needs Python 3 and Pillow: `pip install pillow`)
makes them in a local `sdcard/maps` folder; copy that `maps` folder to the root of
the card.

**You need a tile provider that allows offline downloads.** OpenStreetMap's own
server does not: it answers bulk downloads with an "Access blocked" picture, and the
script stops when it sees one. We use **[Thunderforest](https://www.thunderforest.com/)**,
which needs an **API key**, and permits bulk/offline downloading only on its
[Small Business plan or higher](https://www.thunderforest.com/pricing/). Create an
account, pick the plan, and copy the API key from your Thunderforest dashboard. Keep
the key private: don't put it in a file you share or commit.

Download an area around a point (25 km radius by default, zoom 8 to 14):

```bash
python3 scripts/map_tiles_cardputer.py --lat 51.50 --lon -3.70 \
    --url "https://tile.thunderforest.com/atlas/{z}/{x}/{y}.png?apikey=YOUR_KEY"
```

Or a box, south west north east:

```bash
python3 scripts/map_tiles_cardputer.py --bounds 49.9 -5.8 52.0 0.5 \
    --url "https://tile.thunderforest.com/atlas/{z}/{x}/{y}.png?apikey=YOUR_KEY"
```

`atlas` is a clean style that reads well on the small screen; `outdoors` (paths and
contours) also works. Add `--dry-run` first to see how many tiles an area is: every
tile counts against your Thunderforest quota. Southern England and Wales at zoom
8 to 14 is about 59,000 tiles and 125 MB of data (more on the card, as each file
takes at least one cluster). Run the script again to fill any gaps: tiles already
there are skipped.

**Already have T-Deck tiles?** Convert them with no download:

```bash
python3 scripts/map_tiles_cardputer.py --from /Volumes/SD/maps/osm/day
```

Other options: `--radius-km`, `--min-zoom`, `--max-zoom`, `--quality` (JPEG quality,
default 60), `--out` (output folder) and `--delay` (seconds between downloads).
