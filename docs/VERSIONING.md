# Saitama Versioning

## Scheme

We use [Semantic Versioning 2.0.0](https://semver.org/) with pre-release identifiers:

```
MAJOR.MINOR.PATCH[-PRERELEASE]
```

### Pre-release tags

| Tag | Meaning |
|-----|---------|
| `-alpha.N` | Code compiles, not tested on hardware. Anything may break. |
| `-beta.N` | Flashed and running on hardware. Core features work but bugs expected. |
| `-rc.N` | Release candidate. All vMAJOR.MINOR features working. Final testing. |
| *(none)* | Stable release. Hardware-tested, documented, ready for users. |

### Examples

- `0.1.0-alpha.1` — first compile, never flashed
- `0.1.0-beta.1` — first hardware test on LilyGo T-Deck Plus ← current
- `0.1.0` — chat + map + basic mesh stable on hardware

### When to bump

- **MAJOR**: breaking changes (new partition layout, incompatible mesh protocol)
- **MINOR**: new features (new screen, GPS support, new mesh command)
- **PATCH**: bug fixes only (no new features, no breaking changes)
- **Pre-release N**: increment for each alpha/beta/rc build

### Version numbers only go forward. Never re-release an existing tag.

## Firmware Variants

Each release includes two binaries:

1. **`saitama-VERSION.bin`** — application firmware only. Flash at the app partition offset. Use this for OTA updates from a running system.

2. **`saitama-VERSION-merged.bin`** — merged binary (bootloader + partition table + firmware). Flash at 0x0. Use this for first-time flash or full recovery.

### How to flash

**First time / recovery (merged binary):**
```bash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x0 saitama-0.1.0-beta.1-merged.bin
```

**OTA update (app only):**
```bash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x10000 saitama-0.1.0-beta.1.bin
```

## Release Process

1. Update `src/version.h` with new version
2. Commit with message: `bump version to vX.Y.Z[-tag]`
3. Tag: `git tag -a vX.Y.Z[-tag.N] -m "Release vX.Y.Z[-tag.N]"`
4. Push tag: `git push origin vX.Y.Z[-tag.N]`
5. CI builds both firmware variants and creates a draft GitHub Release
6. Maintainer reviews and publishes the release

## Branching Model

| Branch | Purpose |
|--------|----------|
| `main` | Stable releases. Tagged with versions. Never push directly. Default branch on GitHub. |
| `dev` | Active development. All PRs target this branch. CI must pass. Branch protection enabled. |
| `alpha` | Created from `dev` when enough features accumulate for an alpha release. |
| `beta` | Created from `alpha` when features are hardware-tested and mostly working. |

Flow: PR to `dev`. Merge to `alpha` or `beta` when ready. Merge to `main` and tag when stable. Releases only at proper milestones, not after a single PR or change.