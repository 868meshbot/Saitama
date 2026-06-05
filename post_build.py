# Saitama — post-build script: produces named app-only and merged binaries
# Copyright 2026 Saitama — MIT License
#
# Output (in firmware/):
#   saitama-<device>-<version>.bin           — app-only (flash at 0x10000)
#   saitama-<device>-<version>-merged.bin    — bootloader+table+app (flash at 0x0)
#
# Version is read from src/version.h.  For pre-release builds (version contains
# a hyphen), build metadata is appended: +<commit_count>.<YYYYMMDD>.<short_hash>
# Example: 0.1.0-beta.10+47.20260605.abc1234

import os
import re
import shutil
import subprocess
from datetime import datetime

Import("env")

# Map PlatformIO env names to human-readable device slugs.
_DEVICE_NAMES = {
    "t-deck":          "t-deck-plus",
    "t-deck-launcher": "t-deck-plus",
}

def _git(args, cwd):
    r = subprocess.run(["git"] + args, capture_output=True, text=True, cwd=cwd)
    return r.stdout.strip() if r.returncode == 0 else ""

def _read_version(project_dir):
    path = os.path.join(project_dir, "src", "version.h")
    try:
        m = re.search(r'OPS_VERSION_STRING\s+"([^"]+)"', open(path).read())
        return m.group(1) if m else "0.0.0"
    except Exception:
        return "0.0.0"

def _version_slug(version, project_dir):
    if "-" not in version:
        return version  # stable release — clean filename, no metadata
    count = _git(["rev-list", "--count", "HEAD"], project_dir) or "0"
    hash_ = _git(["rev-parse", "--short", "HEAD"], project_dir) or "unknown"
    date  = datetime.utcnow().strftime("%Y%m%d")
    return "%s+%s.%s.%s" % (version, count, date, hash_)

def build_firmware_artifacts(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    build_dir   = env.subst("$BUILD_DIR")
    out_dir     = os.path.join(project_dir, "firmware")
    os.makedirs(out_dir, exist_ok=True)

    env_name = env.subst("$PIOENV")
    device   = _DEVICE_NAMES.get(env_name, env_name)
    version  = _version_slug(_read_version(project_dir), project_dir)
    stem     = "saitama-%s-%s" % (device, version)

    app        = os.path.join(build_dir, "firmware.bin")
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")

    # ── App-only binary — flash at 0x10000 (OTA / M5Launcher compatible) ──
    if os.path.exists(app):
        dst = os.path.join(out_dir, stem + ".bin")
        shutil.copy2(app, dst)
        print("App binary   → firmware/%s.bin" % stem)

    # ── Merged binary — flash at 0x0 (first-time flash / full recovery) ──
    if all(os.path.exists(p) for p in (bootloader, partitions, app)):
        merged  = os.path.join(out_dir, stem + "-merged.bin")
        esptool = env.subst("$PYTHONEXE") + " -m esptool"
        cmd = (
            '%s --chip esp32s3 merge_bin'
            ' --flash_mode dio --flash_freq 80m --flash_size 16MB'
            ' 0x0000  "%s"'
            ' 0x8000  "%s"'
            ' 0x10000 "%s"'
            ' -o "%s"'
        ) % (esptool, bootloader, partitions, app, merged)
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        if r.returncode == 0:
            print("Merged binary → firmware/%s-merged.bin" % stem)
        else:
            print("esptool merge failed:\n" + r.stderr)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", build_firmware_artifacts)
