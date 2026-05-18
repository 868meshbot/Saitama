# Contributing to Saitama

First off: thank you. This project needs human hands.

## The Reality

This codebase was initially vibecoded by **GLM-5.1** (a 754B parameter language model by Zhipu AI). That means:

- The architecture and initial code are AI-generated
- **Nothing has been tested on physical hardware yet**
- There are almost certainly bugs, wrong pin assignments, and missing edge cases
- The AI that wrote this does not have hands, a T-Deck, or a soldering iron

**Your job is to make it real.**

## What We Need Most

1. **Hardware testers** — flash it, see what breaks, tell us
2. **Embedded C++ devs** — fix the code, add features, review PRs
3. **UI/UX feedback** — does the trackball navigation make sense? Is text readable?
4. **Map tile creators** — pre-built tile sets for different regions
5. **Documentation** — if you figured something out, write it down

## Development Setup

### Prerequisites

- PlatformIO (CLI or VS Code extension)
- Git
- A LilyGo T-Deck or T-Deck Plus
- USB-C cable

### Clone and Build

```bash
git clone --recurse-submodules https://github.com/PeterAlfonsLoch/Saitama.git
cd Saitama
pio run -e t-deck
```

### Flash to Device

Put T-Deck in DFU mode (hold trackball, press reset):

```bash
pio run -e t-deck -t upload
pio device monitor  # 115200 baud
```

### Serial Monitor

All logs use the `OPS_LOG(tag, fmt, ...)` macro. Output looks like:

```
[OPS] Board: Initialising T-Deck hardware
[OPS] Board: Hardware ready
[OPS] Mesh: Initialising MeshCore stack
[OPS] Mesh: MeshCore ready
[OPS] UI: Creating home screen
[OPS] UI: Display ready (320x240)
```

## Code Conventions

We follow MeshCore's embedded C++ conventions:

1. **No dynamic allocation after setup.** All buffers pre-allocated in `begin()` or constructor.
2. **No `std::string`, `std::vector`, or `new`.** Use C arrays, `snprintf`, and stack allocation.
3. **No exceptions.** Compile with `-fno-exceptions`.
4. **Keep it simple.** Think embedded. No unnecessary abstraction layers.
5. **Same brace/indent style as MeshCore.** Allman/BSD braces (opening brace on own line), 4-space indent:
   ```cpp
   void foo()
   {
       if (bar)
       {
           doThing();
       }
   }
   ```
6. **No retroactive reformatting.** Don't create noise diffs.
7. **Every new file gets the MIT license header.**

## Project Structure

```
src/
├── main.cpp            # Entry point
├── version.h           # Version constants (single source of truth)
├── hardware/           # Board, keyboard, GPS drivers
├── mesh/               # MeshCore bridge
├── ui/                 # LVGL screens and theme
├── map/                # Map tile engine
└── utils/              # Config, config export/import, logging
```

See `docs/ARCHITECTURE.md` for the full design.

## Adding a Screen

1. Create `src/ui/ScreenFoo.h` and `ScreenFoo.cpp`
2. Add a `create()` static method that builds the LVGL tree
3. Register it in the navigation model (see `docs/UI_DESIGN.md`)
4. Update `UIScreen.cpp` if it needs to be in the main loop

## Adding a Board

1. Create `src/hardware/BoardNewDevice.h/cpp`
2. Implement the `Board` interface (or a new subclass)
3. Add a PlatformIO environment in `platformio.ini`
4. Update `docs/HARDWARE.md` with pin reference

## Branching

| Branch | Purpose |
|--------|----------|
| `main` | Stable releases. Tagged with versions. Never push directly. |
| `dev` | Active development. All PRs target this branch. CI must pass. |
| `alpha` | From `dev` when enough features accumulate. |
| `beta` | From `alpha` when hardware-tested and mostly working. |

All PRs go to `dev`. Releases merge from `beta` to `main`.

## Security

- **CodeQL** scans run weekly on `main` and `dev` branches
- **Copilot Autofix** is enabled (free for public repos) for security alerts
- **Dependabot** and **secret scanning** are enabled
- The CI security audit workflow scans incoming PRs for suspicious patterns
- Report vulnerabilities privately to maintainers, not in public issues
- **Never commit secrets, tokens, or API keys**, even in test code

## Submitting Changes

1. Fork the repo
2. Create a feature branch: `feat/short-description`
3. One logical change per PR
4. Test on hardware if possible (even "it compiles" is useful)
5. Describe what you changed and why
6. If fixing a pin assignment or hardware bug, include a photo of the serial output

## Questions?

Open an issue. No question is too basic. We were all beginners once, and this
project explicitly welcomes people who are learning embedded development.

## License

By contributing, you agree your code is released under the **MIT License**.