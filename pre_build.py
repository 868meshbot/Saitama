# Saitama - pre-build script for PlatformIO
# Removes Helium ARM assembly from LVGL (not compatible with ESP32-S3/Xtensa)
# Copyright 2026 Saitama — MIT License

import os
import glob

Import("env")

# Prepend compat/ to the include search path so it takes priority over library
# headers. compat/helpers/radiolib/ contains RadioLib 7.6.0 compatible shims
# for SX126xReset.h and CustomSX1262Wrapper.h — MeshCore's originals access
# members that RadioLib 7.6.0 moved to private.
compat_dir = os.path.join(env.subst("$PROJECT_DIR"), "compat")
env.Prepend(CPPPATH=[compat_dir])

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