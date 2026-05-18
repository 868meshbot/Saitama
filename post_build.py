# Saitama - post-build script: copies artifacts and builds merged flash image
# Copyright 2026 Saitama — MIT License

import os
import shutil
import subprocess

Import("env")

def copy_and_merge(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    build_dir   = env.subst("$BUILD_DIR")
    out_dir     = os.path.join(project_dir, "firmware")

    os.makedirs(out_dir, exist_ok=True)

    # Copy individual artifacts
    for name in ("firmware.bin", "bootloader.bin", "partitions.bin"):
        src = os.path.join(build_dir, name)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(out_dir, name))
            print("Copied %s → firmware/%s" % (name, name))

    # Build merged flash image via esptool merge_bin
    bootloader  = os.path.join(build_dir, "bootloader.bin")
    partitions  = os.path.join(build_dir, "partitions.bin")
    app         = os.path.join(build_dir, "firmware.bin")
    merged      = os.path.join(out_dir,   "firmware_merged.bin")

    if not (os.path.exists(bootloader) and os.path.exists(partitions) and os.path.exists(app)):
        print("Skipping merge: one or more inputs missing")
        return

    esptool = env.subst("$PYTHONEXE") + " -m esptool"
    cmd = (
        "%s --chip esp32s3 merge_bin"
        " --flash_mode dio --flash_freq 80m --flash_size 16MB"
        " 0x0000  \"%s\""   # bootloader
        " 0x8000  \"%s\""   # partition table
        " 0x10000 \"%s\""   # application
        " -o \"%s\""
    ) % (esptool, bootloader, partitions, app, merged)

    print("Merging flash image → firmware/firmware_merged.bin")
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if result.returncode == 0:
        print("Merged binary: firmware/firmware_merged.bin")
    else:
        print("esptool merge failed:\n" + result.stderr)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_and_merge)
