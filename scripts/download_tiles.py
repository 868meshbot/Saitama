#!/usr/bin/env python3
"""
Saitama Map Tile Downloader

Downloads OpenStreetMap tiles for offline use on a LilyGo T-Deck Plus.
Tiles are saved to the SD card path the firmware expects: /ops/maps/{z}/{x}/{y}.png

Usage:
    python3 download_tiles.py --output /Volumes/SD/ops/maps \
        --lat 49.6117 --lng 6.1300 \
        --radius 50 --zoom 8-14

    # Luxembourg country-wide
    python3 download_tiles.py --output /Volumes/SD/ops/maps \
        --bounds 49.0,5.7 50.2,6.6 \
        --zoom 8-14

    # Specific city
    python3 download_tiles.py --output /Volumes/SD/ops/maps \
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

TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
USER_AGENT = "Saitama-TileDownloader/0.1 (https://github.com/868-Meshbot/Saitama)"
RATE_LIMIT_DELAY = 0.6  # seconds between requests (stay under 2/s)


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
    print(f"Estimated size: ~{total_tiles * 30 // 1024}KB - {total_tiles * 50 // 1024}KB")
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
                url = TILE_URL.format(z=zoom, x=x, y=y)
                try:
                    resp = session.get(url, timeout=30)
                    if resp.status_code == 200:
                        os.makedirs(tile_dir, exist_ok=True)
                        with open(tile_path, "wb") as f:
                            f.write(resp.content)
                        downloaded += 1
                    else:
                        print(f"  HTTP {resp.status_code} for tile {zoom}/{x}/{y}")
                        errors += 1
                except Exception as e:
                    print(f"  Error downloading {zoom}/{x}/{y}: {e}")
                    errors += 1

                # Rate limit
                time.sleep(RATE_LIMIT_DELAY)

        print(f"  Progress: {downloaded} downloaded, {skipped} cached, {errors} errors")

    print()
    print(f"Done! {downloaded} tiles downloaded, {skipped} already cached, {errors} errors")
    print(f"Tiles saved to: {output_dir}")
    print()
    print("Tiles are ready. If output_dir is already /ops/maps on your SD card, you're done.")
    print("Otherwise copy to SD card:")
    print(f"  cp -r {output_dir}/* /Volumes/SD/ops/maps/")


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

    args = parser.parse_args()

    # Parse zoom range
    zoom_parts = args.zoom.split("-")
    zoom_range = (int(zoom_parts[0]), int(zoom_parts[-1]))

    center = (args.lat, args.lng) if args.lat and args.lng else None

    download_tiles(args.output, zoom_range, bounds=args.bounds, center=center, radius_km=args.radius)


if __name__ == "__main__":
    main()