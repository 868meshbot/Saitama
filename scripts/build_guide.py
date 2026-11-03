#!/usr/bin/env python3
# Saitama — build_guide.py
# Copyright 2026 Saitama — GPL-3.0-or-later
#
# Builds the PDF user guide from the wiki pages in wiki/.
#
#   python3 scripts/build_guide.py            # docs/Saitama-User-Guide.pdf
#   python3 scripts/build_guide.py --html     # also keep the intermediate HTML
#
# Needs the Python "markdown" package and Google Chrome (or Chromium) for the
# HTML -> PDF step. The wiki pages stay the source: edit them, then rerun.

import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import tempfile

import markdown

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIKI = os.path.join(ROOT, "wiki")
SHOTS = os.path.join(ROOT, "screenshots")
OUT_PDF = os.path.join(ROOT, "docs", "Saitama-User-Guide.pdf")

# Chapter order (the same as Home.md and _Sidebar.md).
PAGES = [
    "Why-Mesh",
    "Why-Saitama",
    "Message-Reliability",
    "Apps-Explained",
    "Installing-Saitama",
    "First-Setup",
    "Controls",
    "Launcher",
    "Chat-and-Channels",
    "Contacts-Repeaters-and-Heard",
    "Regions-and-Scopes",
    "Network-Tools",
    "Map-and-GPS",
    "Settings-Reference",
    "Terminal-Commands",
    "Backup-and-Security",
    "Troubleshooting",
    "Glossary",
]

RAW_SHOTS = "https://raw.githubusercontent.com/868meshbot/Saitama/main/screenshots/"

CHROME_CANDIDATES = [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "google-chrome",
    "chromium",
    "chromium-browser",
]

CSS = """
@page { size: A4; margin: 16mm 15mm 18mm 15mm; }
body { font-family: -apple-system, "Helvetica Neue", Arial, sans-serif;
       font-size: 10.5pt; line-height: 1.45; color: #1d232b; }
h1 { font-size: 20pt; color: #0b5d8f; border-bottom: 2px solid #0b5d8f;
     padding-bottom: 4px; margin-top: 0; }
h2 { font-size: 14pt; color: #0b5d8f; margin-top: 18px; }
h3 { font-size: 11.5pt; margin-top: 14px; }
.chapter { page-break-before: always; }
table { border-collapse: collapse; width: 100%; margin: 8px 0 12px; font-size: 9.5pt; }
th, td { border: 1px solid #c9d1da; padding: 4px 6px; vertical-align: top; text-align: left; }
th { background: #eef3f8; }
tr { page-break-inside: avoid; }
code { font-family: Menlo, Consolas, monospace; font-size: 9pt;
       background: #f1f3f5; padding: 0 3px; border-radius: 3px; }
pre { background: #f1f3f5; padding: 8px 10px; border-radius: 4px;
      white-space: pre-wrap; page-break-inside: avoid; }
pre code { background: none; padding: 0; }
img { max-width: 100%; max-height: 62mm; border: 1px solid #c9d1da;
      border-radius: 4px; display: block; margin: 6px auto; }
td img { max-height: 48mm; }
a { color: #0b5d8f; text-decoration: none; }
.cover { text-align: center; padding-top: 40mm; }
.cover h1 { border: none; font-size: 40pt; margin-bottom: 0; }
.cover .sub { font-size: 15pt; color: #55606c; margin: 6px 0 30px; }
.cover .devices { display: flex; justify-content: center; gap: 10mm;
                  align-items: flex-end; margin: 18mm 0; }
.cover .devices img { max-height: 55mm; margin: 0 0 4px; }
.cover .devices div { font-size: 10pt; color: #55606c; }
.cover .highlights { font-size: 11.5pt; color: #0b5d8f; font-weight: 600; }
.cover .meta { font-size: 10pt; color: #55606c; margin-top: 25mm; }
.toc { page-break-before: always; }
.toc ol { font-size: 12pt; line-height: 2; }
"""


def slug(page):
    return page.lower()


def convert(page):
    with open(os.path.join(WIKI, page + ".md"), encoding="utf-8") as f:
        text = f.read()
    # Screenshots: use the local files.
    text = text.replace(RAW_SHOTS, "file://" + SHOTS + "/")
    # Links to other wiki pages become links inside the PDF.
    text = re.sub(r"\]\(([A-Z][A-Za-z-]+)\)",
                  lambda m: "](#%s)" % slug(m.group(1))
                  if m.group(1) in PAGES or m.group(1) == "Home" else m.group(0),
                  text)
    html = markdown.markdown(text, extensions=["tables", "fenced_code"])
    return '<section class="chapter" id="%s">%s</section>' % (slug(page), html)


def title_of(page):
    with open(os.path.join(WIKI, page + ".md"), encoding="utf-8") as f:
        return f.readline().lstrip("# ").strip()


def version():
    with open(os.path.join(ROOT, "src", "version.h"), encoding="utf-8") as f:
        m = re.search(r'OPS_VERSION_STRING\s+"([^"]+)"', f.read())
    return m.group(1) if m else "?"


def build_html():
    ver = version()
    today = datetime.date.today().strftime("%-d %B %Y")
    cover = f"""
<div class="cover">
  <h1>Saitama</h1>
  <div class="sub">User guide for the LilyGo T-Deck Plus<br>and the M5Stack Cardputer ADV</div>
  <div class="highlights">Full touchscreen on the T-Deck &middot; Verified direct messages
  &middot; Messages that get through<br>No phone, no internet, no subscription</div>
  <div class="devices">
    <div><img src="file://{SHOTS}/home.png"><br>T-Deck Plus</div>
    <div><img src="file://{SHOTS}/cardputer-carousel.png"><br>Cardputer ADV</div>
  </div>
  <div class="meta">Firmware version {ver} &middot; {today}<br>
  github.com/868meshbot/Saitama &middot; GPL-3.0-or-later</div>
</div>"""
    toc = '<div class="toc"><h1>Contents</h1><ol>' + "".join(
        '<li><a href="#%s">%s</a></li>' % (slug(p), title_of(p)) for p in PAGES
    ) + "</ol></div>"
    intro = convert("Home").replace('class="chapter"', 'class="chapter intro"')
    # The wiki home page's own contents list duplicates the TOC above.
    intro = re.sub(r"<h2>Contents</h2>.*?</ol>", "", intro, flags=re.S)
    body = "".join(convert(p) for p in PAGES)
    return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Saitama user guide {ver}</title>
<style>{CSS}</style></head>
<body>{cover}{toc}{intro}{body}</body></html>"""


def find_chrome():
    for c in CHROME_CANDIDATES:
        if os.path.isabs(c) and os.path.exists(c):
            return c
        if not os.path.isabs(c) and shutil.which(c):
            return shutil.which(c)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--html", action="store_true", help="keep docs/Saitama-User-Guide.html")
    args = ap.parse_args()

    chrome = find_chrome()
    if not chrome:
        sys.exit("Chrome or Chromium not found (needed for the PDF step)")

    html = build_html()
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "guide.html")
        with open(src, "w", encoding="utf-8") as f:
            f.write(html)
        if args.html:
            shutil.copy(src, OUT_PDF[:-4] + ".html")
        subprocess.run([chrome, "--headless", "--disable-gpu",
                        "--allow-file-access-from-files",
                        "--no-pdf-header-footer",
                        "--print-to-pdf=" + OUT_PDF, "file://" + src],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("Wrote", os.path.relpath(OUT_PDF, ROOT))


if __name__ == "__main__":
    main()
