# AGENTS.md — Guide for AI Contributors

## What This Is

Saitama is open-source firmware for the LilyGo T-Deck and T-Deck Plus (ESP32-S3, LoRa mesh). It uses MeshCore as a git submodule and LVGL 9 for the UI.

The initial codebase was vibecoded by GLM-5.1. It compiles but has **never been tested on real hardware**. Treat it as a foundation that needs real-world validation.

## Project Layout

```
src/
  main.cpp            — Entry point (setup + loop)
  version.h           — Version constants (single source of truth)
  hardware/Board.h/cpp — T-Deck hardware abstraction
  hardware/Keyboard.h/cpp — BBQ10KB I2C keyboard (planned)
  mesh/MeshService.h/cpp — MeshCore bridge
  ui/UIScreen.h/cpp    — LVGL display controller
  ui/ScreenHome.h/cpp  — Home/chat screen
  ui/ScreenMap.h/cpp   — Map screen (planned)
  ui/ScreenSettings.h/cpp — Settings (planned)
  ui/ScreenTerminal.h/cpp — Terminal (planned)
  ui/Theme.h/cpp       — Colour palette
  map/MapEngine.h/cpp  — Tile coordinate math + offline map
  utils/Config.h/cpp   — Persistent settings (SPIFFS)
  utils/Log.h          — Serial logger
lib/
  MeshCore/            — Git submodule, NOT our code
test/
  test_map_engine.cpp  — Host-side coordinate math tests (g++ compiled)
```

## Build Commands

```bash
# Full build
pio run -e t-deck

# Upload to device
pio run -e t-deck -t upload

# Host-side tests (no Arduino needed)
g++ -std=c++14 -Wall -Wextra -o test_map_engine test/test_map_engine.cpp -lm
./test_map_engine
```

## Code Conventions

1. **No dynamic allocation after setup()** — this runs on an ESP32-S3 with limited heap. Allocate everything upfront. Match MeshCore's convention.

2. **Allman/BSD brace style** — opening brace on its own line:
   ```cpp
   void foo()
   {
       // body
   }
   ```

3. **4-space indentation** — no tabs.

4. **C++14 compatible** — no C++17 features. The ESP32-S3 toolchain has limits.

5. **`ops::` namespace** — all Saitama code goes in the `oms` namespace. Never in the global namespace.

6. **MIT license header** on every source file:
   ```cpp
   // Saitama — FileName.h
   // Copyright 2026 Saitama — MIT License
   ```

7. **Version comes from `src/version.h`** — never hardcode version strings elsewhere. Use `OPS_VERSION_STRING` macro.

## Critical Constraints

- **SPI bus is shared** between display, LoRa radio, and SD card. Coordinate CS lines. Never hold CS low across blocking operations.

- **PSRAM (8MB OPI)** is available for large buffers (map tiles, message queues). Use `ps_malloc()` not `malloc()` for anything over 4KB. Regular `malloc()` goes to internal DRAM (320KB total, shared with everything).

- **Display is ST7789 320x240** — TFT_eSPI driver. The display buffer must be allocated with `ps_malloc()` at runtime, not as a static BSS array.

- **Keyboard is BBQ10KB on I2C address 0x1F** — not yet tested on hardware.

- **MeshCore is a submodule** — never modify files inside `lib/MeshCore/`. If MeshCore needs changes, contribute upstream.

- **LVGL 9** — not LVGL 8. APIs differ significantly. Always use LVGL 9 patterns.

## What Not To Do

- **Don't add dependencies** without discussion. Every library increases attack surface and binary size. Supply chain risk is real on embedded.

- **Don't modify CI workflows** without maintainer approval. The security audit workflow exists for a reason.

- **Don't use `String` (Arduino)** in hot paths. It heap-allocates. Use fixed-size buffers or `snprintf()` into stack arrays.

- **Don't use `delay()`** in the main loop. Use non-blocking patterns (state machines, `millis()` timers).

- **Don't use `new`/`malloc` after setup** except via `ps_malloc()` for large, long-lived buffers.

- **Don't submit binary files** (`.bin`, `.elf`, `.hex`) in PRs. CI builds them.

- **Don't touch `lib/MeshCore/`** — it's a git submodule. Update it by bumping the submodule reference, not by editing files.

## Testing

- **Host-side tests** go in `test/`. They must compile with standard `g++ -std=c++14` with no Arduino includes. Coordinate math, config parsing, string handling — pure logic only.

- **Coordinate math** must be tested against real city coordinates. See `test/test_map_engine.cpp` for the pattern. Luxembourg (49.6117, 6.1300) is our reference point.

- **Float32 precision**: at zoom 14+, expect ±1 tile rounding error in roundtrips. Document this in tests.

- **No hardware mocks** — we don't mock SPI, I2C, or LVGL. If you can't test it without hardware, mark it with `// TODO: test on hardware`.

## Versioning

See [docs/VERSIONING.md](docs/VERSIONING.md). Key points:

- Semantic Versioning with pre-release tags: `MAJOR.MINOR.PATCH-alpha.N`
- Current: `0.1.0-alpha.1` (first compile, not flashed)
- Update `src/version.h` before tagging a release
- Version numbers only go forward. Never re-tag.

## Firmware Variants

Each release has two binaries:

1. **App-only** (`saitama-X.Y.Z.bin`) — flash at `0x10000`, for OTA updates
2. **Merged** (`saitama-X.Y.Z-merged.bin`) — flash at `0x0`, bootloader + partitions + app

The merged binary is required for first-time flash or full recovery. The app-only binary is smaller and faster for updates.

## Security

- Report vulnerabilities privately to maintainers, not in public issues
- The CI security audit workflow scans incoming PRs for suspicious patterns (binary files, workflow modifications, credential leaks, new dependencies)
- **Never commit secrets, tokens, or API keys** — even in test code

## Branching

- **`main`**: Stable releases only. Tagged with versions. Never push directly.
- **`dev`**: Active development. All PRs target this branch. CI must pass.
- **`alpha`**: Branched from `dev` when enough features accumulate for an alpha release.
- **`beta`**: Branched from `alpha` when features are hardware-tested and mostly working.

Flow: PR to `dev`. Merge to `alpha` or `beta` when ready. Merge to `main` and tag when stable.

## Sub-Agent Issue/PR Checking

Every incoming issue and PR should be checked for:

1. **Malicious content** — binary files, workflow modifications, credential leaks, suspicious patterns (automated by `.github/workflows/security.yml`)
2. **Supply chain risk** — new dependencies, typosquatted package names
3. **Constructive vs. destructive** — does the contributor understand the codebase? Are they proposing changes that align with project goals?
4. **Code quality** — does it follow conventions (Allman braces, `ops::` namespace, no dynamic allocation after setup)?
5. **Security implications** — could the change expose the device, radio, or user data?

For AI contributors (including me), run an inversion-thinking pass before submitting: assume the change is wrong, then find the specific ways it could fail. Document those concerns in the PR description.

## GitHub AI Tools (Free for Public Repos)

These are enabled and free for Saitama (public repo):

- **CodeQL + Copilot Autofix** — automated security scanning with AI-suggested fixes (`.github/workflows/codeql.yml`)
- **Dependabot** — automated dependency updates and security alerts
- **Secret scanning** — detects leaked tokens/keys in PRs
- **Vulnerability alerts** — automated alerts for known CVEs in dependencies

Note: Copilot code review and coding agent are **paid features** (Pro/Business/Enterprise). We use our own security.yml workflow instead.

## Maintainer

- **PeterAlfonsLoch** (Jeff) — full admin access, final say on merges
- AI-generated PRs welcome but get extra scrutiny like any other PR