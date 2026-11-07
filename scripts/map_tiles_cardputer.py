#!/usr/bin/env python3
# Saitama — map_tiles_cardputer.py
# Copyright 2026 Saitama — GPL-3.0-or-later
#
# Downloads offline map tiles for the Cardputer map as small baseline JPEGs:
#   <out>/maps/osm/{z}/{x}/{y}.jpg
# Copy the "maps" folder to the root of the SD card. PNG tiles for the T-Deck
# can live in the same folders; the Cardputer only reads .jpg.
#
# The Cardputer's ROM JPEG decoder needs baseline (not progressive) RGB
# JPEGs, which is what this writes.
#
# Convert the T-Deck's PNG tiles (no download):
#   python3 scripts/map_tiles_cardputer.py --from /Volumes/SD/maps/osm/day
# Download an area:
#   python3 scripts/map_tiles_cardputer.py --lat 51.50 --lon -0.12 --url <provider>
#
# Tile servers have usage rules. OpenStreetMap's own server forbids bulk
# (offline) downloading and answers with an "Access blocked" image (HTTP 200):
# https://operations.osmfoundation.org/policies/tiles/ - pass --url for a
# provider that allows offline use. The script stops if it gets the blocked
# image. (Identical tiles are normal: open sea, empty land.) Map data (c) OpenStreetMap
# contributors; the Cardputer shows "(c)OSM" on the map.
#
# Needs Pillow:  pip install pillow

import argparse
import hashlib
import io
import math
import os
import sys
import time
import urllib.request

DEFAULT_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
# OpenStreetMap's "Access blocked" PNG (same MD5 as download_tiles.py)
BLOCKED_TILE_MD5 = {"c069a15b2cc2d6b6f527ad09eb93c61a"}
USER_AGENT = "Saitama-mesh-firmware-offline-map/1.0 (+https://github.com/868meshbot/Saitama)"


def tile_xy(lat, lon, z):
    n = 1 << z
    lat = max(min(lat, 85.05), -85.05)
    x = int((lon + 180.0) / 360.0 * n)
    r = math.radians(lat)
    y = int((1.0 - math.asinh(math.tan(r)) / math.pi) / 2.0 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


def tiles_for_box(south, west, north, east, z):
    x0, y0 = tile_xy(north, west, z)
    x1, y1 = tile_xy(south, east, z)
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            yield x, y


def tiles_for_area(lat, lon, radius_km, z):
    dlat = radius_km / 111.32
    dlon = radius_km / (111.32 * max(math.cos(math.radians(lat)), 0.01))
    x0, y0 = tile_xy(lat + dlat, lon - dlon, z)
    x1, y1 = tile_xy(lat - dlat, lon + dlon, z)
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            yield x, y


def to_jpeg(data, quality):
    from PIL import Image
    img = Image.open(io.BytesIO(data)).convert("RGB")   # 3 components
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=quality, progressive=False, optimize=True)
    return buf.getvalue()


def convert_local(src, out, quality, min_zoom, max_zoom):
    """PNG tiles in src/{z}/{x}/{y}.png -> out/maps/osm/{z}/{x}/{y}.jpg"""
    done = skipped = failed = 0
    total_bytes = 0
    for z in sorted(os.listdir(src)):
        if not z.isdigit() or not min_zoom <= int(z) <= max_zoom:
            continue
        for x in sorted(os.listdir(os.path.join(src, z))):
            xdir = os.path.join(src, z, x)
            if not x.isdigit() or not os.path.isdir(xdir):
                continue
            for name in sorted(os.listdir(xdir)):
                stem, ext = os.path.splitext(name)
                if not stem.isdigit() or ext.lower() not in (".png", ".jpg", ".jpeg"):
                    continue
                dst = os.path.join(out, "maps", "osm", z, x, stem + ".jpg")
                if os.path.exists(dst):
                    skipped += 1
                    total_bytes += os.path.getsize(dst)
                    continue
                try:
                    with open(os.path.join(xdir, name), "rb") as f:
                        data = f.read()
                    if hashlib.md5(data).hexdigest() in BLOCKED_TILE_MD5:
                        failed += 1
                        continue
                    jpg = to_jpeg(data, quality)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    with open(dst, "wb") as f:
                        f.write(jpg)
                    done += 1
                    total_bytes += len(jpg)
                except Exception as e:
                    failed += 1
                    print(f"  {z}/{x}/{name}: {e}")
            print(f"  zoom {z} column {x}: {done} converted so far")
    print(f"done: {done} converted, {skipped} already there, {failed} skipped (bad or blocked), "
          f"{total_bytes / 1e6:.1f} MB in {os.path.join(out, 'maps')}")
    print("Copy the 'maps' folder to the root of the SD card.")


def main():
    ap = argparse.ArgumentParser(description="Offline JPEG map tiles for the Saitama Cardputer map")
    ap.add_argument("--from", dest="src", help="convert PNG tiles in this {z}/{x}/{y}.png folder "
                    "(e.g. the T-Deck's /maps/osm/day) instead of downloading")
    ap.add_argument("--lat", type=float, help="centre latitude")
    ap.add_argument("--lon", type=float, help="centre longitude")
    ap.add_argument("--bounds", nargs=4, type=float, metavar=("SOUTH", "WEST", "NORTH", "EAST"),
                    help="download a lat/lon box instead of --lat/--lon/--radius-km")
    ap.add_argument("--radius-km", type=float, default=25.0, help="area radius (default 25 km)")
    ap.add_argument("--min-zoom", type=int, default=8)
    ap.add_argument("--max-zoom", type=int, default=14)
    ap.add_argument("--quality", type=int, default=60, help="JPEG quality (default 60)")
    ap.add_argument("--out", default="sdcard", help="output folder (default ./sdcard)")
    ap.add_argument("--url", default=DEFAULT_URL, help="tile URL template with {z} {x} {y}")
    ap.add_argument("--delay", type=float, default=0.5, help="seconds between downloads (default 0.5)")
    ap.add_argument("--dry-run", action="store_true", help="only count the tiles")
    a = ap.parse_args()

    if not 0 <= a.min_zoom <= a.max_zoom <= 19:
        sys.exit("zoom range must be 0..19 with min <= max")

    if a.src:
        try:
            import PIL  # noqa: F401
        except ImportError:
            sys.exit("Pillow is needed: pip install pillow")
        convert_local(a.src, a.out, a.quality, a.min_zoom, a.max_zoom)
        return
    if a.bounds:
        jobs = [(z, x, y) for z in range(a.min_zoom, a.max_zoom + 1)
                for x, y in tiles_for_box(*a.bounds, z)]
        where = "box S{:g} W{:g} N{:g} E{:g}".format(*a.bounds)
    elif a.lat is not None and a.lon is not None:
        jobs = [(z, x, y) for z in range(a.min_zoom, a.max_zoom + 1)
                for x, y in tiles_for_area(a.lat, a.lon, a.radius_km, z)]
        where = f"{a.radius_km:g} km around {a.lat:.4f},{a.lon:.4f}"
    else:
        sys.exit("give --from <folder>, --bounds, or --lat and --lon to download")
    print(f"{len(jobs)} tiles, zoom {a.min_zoom}-{a.max_zoom}, {where}")
    if a.dry_run:
        return

    try:
        import PIL  # noqa: F401
    except ImportError:
        sys.exit("Pillow is needed: pip install pillow")

    done = skipped = failed = 0
    total_bytes = 0
    for i, (z, x, y) in enumerate(jobs, 1):
        path = os.path.join(a.out, "maps", "osm", str(z), str(x), f"{y}.jpg")
        if os.path.exists(path):
            skipped += 1
            total_bytes += os.path.getsize(path)
            continue
        url = a.url.format(z=z, x=x, y=y)
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=30) as r:
                data = r.read()
            h = hashlib.md5(data).hexdigest()
            if h in BLOCKED_TILE_MD5:
                sys.exit(f"The server answered {z}/{x}/{y} with its 'Access blocked' image: it "
                         "doesn't allow offline downloading. Use --url for another provider, "
                         "or --from to convert tiles you already have.")
            jpg = to_jpeg(data, a.quality)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(jpg)
            done += 1
            total_bytes += len(jpg)
        except Exception as e:  # keep going; a rerun fills the gaps
            failed += 1
            print(f"  {z}/{x}/{y}: {e}")
        if i % 50 == 0:
            print(f"  {i}/{len(jobs)}")
        time.sleep(a.delay)

    print(f"done: {done} new, {skipped} already there, {failed} failed, "
          f"{total_bytes / 1e6:.1f} MB in {os.path.join(a.out, 'maps')}")
    print("Copy the 'maps' folder to the root of the SD card.")


if __name__ == "__main__":
    main()
