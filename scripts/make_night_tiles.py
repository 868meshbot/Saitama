#!/usr/bin/env python3
# Saitama — make_night_tiles.py
# Copyright 2026 Saitama — GPL-3.0-or-later
"""
Make a dark "night" tile set from day tiles, for Saitama's day/night maps.

    python3 scripts/make_night_tiles.py --day /Volumes/SD/maps/osm/day \
                                        --night /Volumes/SD/maps/osm/night

Each tile's lightness is inverted and compressed into a dark range while its
hue is kept, so water stays blue, parks green and major roads keep their
colours; light land becomes dark and dark text becomes light. Paletted PNGs
(as rendered by OSM tile servers) are transformed palette-only, which is fast.
Existing night tiles are skipped, so it can be re-run to fill in new tiles.

Requires Pillow (pip install pillow).
"""

import argparse
import colorsys
import os
import sys

from PIL import Image


def night_rgb(r, g, b):
    """Map one day colour to its night colour."""
    h, l, s = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
    l = 0.08 + (1.0 - l) * 0.75     # invert lightness into a dark range
    s = s * 0.8                      # a little less saturated at night
    nr, ng, nb = colorsys.hls_to_rgb(h, l, s)
    return int(nr * 255 + 0.5), int(ng * 255 + 0.5), int(nb * 255 + 0.5)


def convert(src, dst):
    img = Image.open(src)
    if img.mode != "P":
        # Quantise so the transform stays palette-only (and the output small).
        img = img.convert("RGB").quantize(colors=256)
    pal = img.getpalette()
    out = []
    for i in range(0, len(pal), 3):
        out.extend(night_rgb(pal[i], pal[i + 1], pal[i + 2]))
    img.putpalette(out)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    img.save(dst, optimize=True)


def main():
    ap = argparse.ArgumentParser(description="Make dark night tiles from day tiles")
    ap.add_argument("--day", required=True, help="Day tile folder ({z}/{x}/{y}.png)")
    ap.add_argument("--night", required=True, help="Night tile folder to write")
    args = ap.parse_args()

    done = skipped = failed = 0
    for root, _dirs, files in os.walk(args.day):
        for name in files:
            if not name.endswith(".png"):
                continue
            src = os.path.join(root, name)
            dst = os.path.join(args.night, os.path.relpath(src, args.day))
            if os.path.exists(dst) and os.path.getsize(dst) > 0:
                skipped += 1
                continue
            try:
                convert(src, dst)
                done += 1
            except Exception as e:  # keep going past a bad tile
                print(f"  {src}: {e}", file=sys.stderr)
                failed += 1
            if done and done % 2000 == 0:
                print(f"  {done} converted...")
    print(f"Done: {done} converted, {skipped} already there, {failed} failed")


if __name__ == "__main__":
    main()
