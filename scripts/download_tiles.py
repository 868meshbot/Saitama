#!/usr/bin/env python3
"""
Saitama Map Tile Downloader

Downloads OpenStreetMap tiles for offline use on a LilyGo T-Deck Plus.
Tiles are saved where the firmware looks: /maps/osm/<style>/{z}/{x}/{y}.png,
where <style> is "day" (OpenStreetMap, default) or "night" (Thunderforest
Transport Dark). Pass the /maps/osm folder as --output; the style subfolder is
added. Saitama shows night tiles while night mode is on (Settings > Map Style:
Auto), or the day tiles colour-inverted if there are no night tiles.

Night tiles: need a Thunderforest API key, and Thunderforest only permits bulk
(offline) downloading on Small Business plans or higher — confirm with
--i-have-a-bulk-download-plan.

Usage:
    python3 download_tiles.py --output /Volumes/SD/maps/osm \
        --lat 49.6117 --lng 6.1300 \
        --radius 50 --zoom 8-14

    # Luxembourg country-wide
    python3 download_tiles.py --output /Volumes/SD/maps/osm \
        --bounds 49.0,5.7 50.2,6.6 \
        --zoom 8-14

    # Specific city
    python3 download_tiles.py --output /Volumes/SD/maps/osm \
        --lat 49.6117 --lng 6.1300 \
        --radius 10 --zoom 12-16

Requirements: requests (pip install requests)

WARNING: This uses the OpenStreetMap tile server which has usage policies.
See: https://operations.osmfoundation.org/policies/tiles/
- Rate limit: max 2 requests/second
- Provide a valid User-Agent
- Don't bulk-download the planet
"""

import argparse
import os
import sys
import time
import math
import requests

# Map styles. The firmware reads /maps/osm/day and /maps/osm/night (and the old
# flat /maps/osm as day); --output is the /maps/osm folder, the style
# subfolder is added automatically.
STYLES = {
    "day":   "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    # Thunderforest Transport Dark. Needs an API key, and Thunderforest only
    # permits bulk (offline) downloading on Small Business plans or higher —
    # see https://www.thunderforest.com/pricing/ . Without such a plan, use
    # day tiles: the firmware shows them colour-inverted at night.
    "night": "https://api.thunderforest.com/transport-dark/{z}/{x}/{y}.png?apikey={key}",
}
TILE_URL = STYLES["day"]
USER_AGENT = "Saitama-TileDownloader/0.1 (https://github.com/868meshbot/Saitama)"
API_KEY = ""
RATE_LIMIT_DELAY = 0.6

# OpenStreetMap's public servers forbid bulk/offline downloading and answer
# with an "Access blocked" PNG (HTTP 200). Its MD5, so we stop instead of
# saving it as every tile.
import hashlib
BLOCKED_TILE_MD5 = {"c069a15b2cc2d6b6f527ad09eb93c61a"}  # seconds between requests (stay under 2/s)


def lat_lng_to_tile(lat, lng, zoom):
    """Convert lat/lng to tile coordinates (slippy map convention)."""
    n = 2 ** zoom
    x = int((lng + 180.0) / 360.0 * n)
    lat_rad = math.radians(lat)
    y = int((1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * n)
    x = max(0, min(x, n - 1))
    y = max(0, min(y, n - 1))
    return x, y


def tile_to_lat_lng(x, y, zoom):
    """Convert tile coordinates to lat/lng of the tile's top-left corner."""
    n = 2 ** zoom
    lng = x / n * 360.0 - 180.0
    lat_rad = math.atan(math.sinh(math.pi * (1.0 - 2.0 * y / n)))
    lat = math.degrees(lat_rad)
    return lat, lng


def parse_bounds(bounds_str):
    """Parse 'lat1,lng1' string to float tuple."""
    parts = bounds_str.split(',')
    if len(parts) != 2:
        raise ValueError(f"Invalid bounds format: {bounds_str} (expected lat,lng)")
    return float(parts[0]), float(parts[1])


def download_tiles(output_dir, zoom_range, bounds=None, center=None, radius_km=0):
    """Download tiles for the specified area and zoom range."""
    if bounds:
        lat_min, lng_min = parse_bounds(bounds[0])
        lat_max, lng_max = parse_bounds(bounds[1])
    elif center:
        center_lat, center_lng = center
        # Convert radius to approximate degree offset
        lat_delta = radius_km / 111.32
        lng_delta = radius_km / (111.32 * math.cos(math.radians(center_lat)))
        lat_min = center_lat - lat_delta
        lat_max = center_lat + lat_delta
        lng_min = center_lng - lng_delta
        lng_max = center_lng + lng_delta
    else:
        print("Error: specify --bounds or --lat/--lng with --radius")
        sys.exit(1)

    total_tiles = 0
    downloaded = 0
    skipped = 0
    errors = 0

    # Count tiles first
    for zoom in range(zoom_range[0], zoom_range[1] + 1):
        x_min, y_min = lat_lng_to_tile(lat_max, lng_min, zoom)
        x_max, y_max = lat_lng_to_tile(lat_min, lng_max, zoom)
        total_tiles += (x_max - x_min + 1) * (y_max - y_min + 1)

    print(f"Area: {lat_min:.4f},{lng_min:.4f} to {lat_max:.4f},{lng_max:.4f}")
    print(f"Zoom: {zoom_range[0]}-{zoom_range[1]}")
    print(f"Total tiles to download: {total_tiles}")
    # ~30-50 KB per tile
    print(f"Estimated size: ~{total_tiles * 30 / 1024:.1f}-{total_tiles * 50 / 1024:.1f} MB")
    print()

    session = requests.Session()
    session.headers.update({"User-Agent": USER_AGENT})

    for zoom in range(zoom_range[0], zoom_range[1] + 1):
        x_min, y_min = lat_lng_to_tile(lat_max, lng_min, zoom)
        x_max, y_max = lat_lng_to_tile(lat_min, lng_max, zoom)

        print(f"Zoom {zoom}: tiles ({x_min},{y_min}) to ({x_max},{y_max}) "
              f"— {(x_max-x_min+1)*(y_max-y_min+1)} tiles")

        for x in range(x_min, x_max + 1):
            for y in range(y_min, y_max + 1):
                tile_dir = os.path.join(output_dir, str(zoom), str(x))
                tile_path = os.path.join(tile_dir, f"{y}.png")

                # Skip if already downloaded
                if os.path.exists(tile_path) and os.path.getsize(tile_path) > 0:
                    skipped += 1
                    continue

                # Download
                url = TILE_URL.format(z=zoom, x=x, y=y, key=API_KEY)
                try:
                    resp = session.get(url, timeout=30)
                    if resp.status_code == 200 and \
                            hashlib.md5(resp.content).hexdigest() in BLOCKED_TILE_MD5:
                        sys.exit("\nThe tile server returned its 'Access blocked' image: it does not\n"
                                 "allow bulk/offline downloading. Use a source you are licensed to\n"
                                 "bulk-download from, e.g. your own renderer via --url.")
                    if resp.status_code == 200:
                        os.makedirs(tile_dir, exist_ok=True)
                        with open(tile_path, "wb") as f:
                            f.write(resp.content)
                        downloaded += 1
                    else:
                        print(f"  HTTP {resp.status_code} for tile {zoom}/{x}/{y}")
                        errors += 1
                except Exception as e:
                    msg = str(e)
                    if API_KEY:
                        msg = msg.replace(API_KEY, "***")   # never print the API key
                    print(f"  Error downloading {zoom}/{x}/{y}: {msg}")
                    errors += 1

                # Rate limit
                if RATE_LIMIT_DELAY > 0:
                    time.sleep(RATE_LIMIT_DELAY)

        print(f"  Progress: {downloaded} downloaded, {skipped} cached, {errors} errors")

    print()
    print(f"Done! {downloaded} tiles downloaded, {skipped} already cached, {errors} errors")
    print(f"Tiles saved to: {output_dir}")
    print()
    style = os.path.basename(output_dir.rstrip("/"))
    print(f"Tiles are ready. If output_dir is already /maps/osm/{style} on your SD card, you're done.")
    print("Otherwise copy to SD card:")
    print(f"  cp -r {output_dir}/* /Volumes/SD/maps/osm/{style}/")


def main():
    parser = argparse.ArgumentParser(
        description="Download OSM map tiles for Saitama offline use")
    parser.add_argument("--output", "-o", required=True,
                        help="Output directory for tiles")
    parser.add_argument("--zoom", "-z", default="10-14",
                        help="Zoom range (default: 10-14)")
    parser.add_argument("--bounds", "-b", nargs=2, metavar=("SOUTH_WEST", "NORTH_EAST"),
                        help="Bounds as 'lat,lng' 'lat,lng'")
    parser.add_argument("--lat", type=float, help="Center latitude")
    parser.add_argument("--lng", type=float, help="Center longitude")
    parser.add_argument("--radius", "-r", type=float, default=50,
                        help="Radius in km around center (default: 50)")
    parser.add_argument("--style", "-s", choices=sorted(STYLES), default="day",
                        help="day = OpenStreetMap (default); night = Thunderforest Transport Dark")
    parser.add_argument("--apikey", default=os.environ.get("THUNDERFOREST_API_KEY", ""),
                        help="Thunderforest API key for --style night "
                             "(or set THUNDERFOREST_API_KEY)")
    parser.add_argument("--url", default="",
                        help="Tile URL template with {z} {x} {y} (e.g. your own tile server "
                             "http://localhost:8080/tile/{z}/{x}/{y}.png); overrides --style's source")
    parser.add_argument("--delay", type=float, default=None,
                        help="Seconds between requests (default 0.6 for public servers; "
                             "use 0 for your own local server)")
    parser.add_argument("--i-have-a-bulk-download-plan", dest="bulk_ok", action="store_true",
                        help="Confirm your Thunderforest plan permits bulk/offline downloading "
                             "(Small Business or higher) — required for --style night")

    args = parser.parse_args()

    global TILE_URL, API_KEY, RATE_LIMIT_DELAY
    TILE_URL = args.url or STYLES[args.style]
    API_KEY = args.apikey
    if args.delay is not None:
        RATE_LIMIT_DELAY = max(0.0, args.delay)
    if args.style == "night" and not args.url:
        if not args.apikey:
            sys.exit("--style night needs a Thunderforest API key: --apikey KEY "
                     "or THUNDERFOREST_API_KEY")
        if not args.bulk_ok:
            sys.exit("Thunderforest only permits bulk/offline tile downloads on Small Business\n"
                     "plans or higher (https://www.thunderforest.com/pricing/). If yours does,\n"
                     "re-run with --i-have-a-bulk-download-plan. Otherwise download day tiles\n"
                     "only — Saitama shows them inverted (dark) at night.")

    # Tiles go in the style's subfolder, e.g. /Volumes/SD/maps/osm/day
    out = args.output.rstrip("/")
    if os.path.basename(out) not in STYLES:
        out = os.path.join(out, args.style)
    args.output = out

    # Parse zoom range
    zoom_parts = args.zoom.split("-")
    zoom_range = (int(zoom_parts[0]), int(zoom_parts[-1]))

    center = (args.lat, args.lng) if args.lat and args.lng else None

    download_tiles(args.output, zoom_range, bounds=args.bounds, center=center, radius_km=args.radius)


if __name__ == "__main__":
    main()