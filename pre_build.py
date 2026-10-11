# Saitama - pre-build script for PlatformIO
# Removes Helium ARM assembly from LVGL (not compatible with ESP32-S3/Xtensa)
# Copyright 2026 Saitama — MIT License

import os
import glob

Import("env")

# ed25519 lives in lib/MeshCore/lib/ed25519 with no PlatformIO manifest.
# PlatformIO's LDF won't auto-link it because no project source directly
# includes it — only MeshCore's Identity.cpp does. Compile it explicitly.
ed25519_src = os.path.join(env.subst("$PROJECT_DIR"), "lib", "MeshCore", "lib", "ed25519")
ed25519_build = os.path.join(env.subst("$BUILD_DIR"), "ed25519")
env.BuildSources(ed25519_build, ed25519_src, "+<*.c>")

# Find and remove Helium ARM assembly files from LVGL
# These are ARM-only and cause build failures on ESP32-S3 (Xtensa)
build_dir = env.subst("$BUILD_DIR")
lib_build_dir = os.path.join(os.path.dirname(build_dir), "lib")

# Walk the LVGL build directory and remove any .S (assembly) files
# that are Helium-specific
for root, dirs, files in os.walk(lib_build_dir):
    for f in files:
        if f.endswith('.S') or f.endswith('.s'):
            filepath = os.path.join(root, f)
            if 'helium' in filepath.lower() or 'arm2d' in filepath.lower():
                try:
                    os.remove(filepath)
                    print("Removed incompatible assembly: " + filepath)
                except OSError as e:
                    print("Warning: could not remove %s: %s" % (filepath, e))

# Also check source LVGL dirs in .pio/libdeps
libdeps_dir = os.path.join(env.subst("$PROJECT_DIR"), ".pio", "libdeps", env.subst("$PIOENV"))
if os.path.exists(libdeps_dir):
    for root, dirs, files in os.walk(libdeps_dir):
        for f in files:
            if f.endswith('.S') or f.endswith('.s'):
                filepath = os.path.join(root, f)
                if 'helium' in filepath.lower() or 'arm2d' in filepath.lower():
                    try:
                        os.remove(filepath)
                        print("Removed incompatible assembly: " + filepath)
                    except OSError as e:
                        print("Warning: could not remove %s: %s" % (filepath, e))
# NavBoxLib keeps its PNG decoder as a static object (~45 KB) in internal DRAM
# for the whole uptime, even if the Map is never opened. Internal RAM is what
# BLE pairing and hardware AES allocate from, so put the decoder in PSRAM,
# created on first use. Patched in place (idempotent) after each lib download.
navbox_px = os.path.join(libdeps_dir, "NavBoxLib", "src", "navboxlib", "PixelBuffer.cpp")
if os.path.exists(navbox_px):
    with open(navbox_px) as fh:
        src = fh.read()
    marker = "// Saitama patch: PNG decoder in PSRAM"
    old = "static PNG _png;\n"
    if marker not in src and old in src:
        new = (marker + "\n"
               "#include <new>\n"
               "#include <esp_heap_caps.h>\n"
               "static PNG* _pngPtr = nullptr;\n"
               "static PNG& _pngInst() {\n"
               "    if (!_pngPtr) {\n"
               "        void* m = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n"
               "        _pngPtr = m ? new (m) PNG() : new PNG();\n"
               "    }\n"
               "    return *_pngPtr;\n"
               "}\n"
               "#define _png (_pngInst())\n")
        with open(navbox_px, "w") as fh:
            fh.write(src.replace(old, new, 1))
        print("Patched NavBoxLib: PNG decoder moved to PSRAM")
    elif marker not in src:
        print("Warning: NavBoxLib PixelBuffer.cpp changed - PNG PSRAM patch not applied")
