# Releasing

## Versions

- **App and SDK:** one SemVer version, set in `CMakeLists.txt` and `vcpkg.json`. The bundled modules (Gain, Level Meter, Spectrogram) share it.
- **Separate modules:** each has its own version in `module.json` and its own `CHANGELOG.md`. Set `minHostVersion` by hand to the first app version with every host feature the module uses.
- **Plugin ABI:** the integer `DSPTB_ABI_VERSION`. Changes within a version are append-only.

## App release

1. Update the versions and `CHANGELOG.md`, and get CI green on `main`.
2. Tag `vX.Y.Z` (`vX.Y.Z-rc.N` for a pre-release) and push it.

`release.yml` builds, tests and smoke-tests the app, then publishes the app, SDK, source and bundled module zips with checksums.

## Module release

1. Update the module's `module.json` and `CHANGELOG.md`.
2. Tag `module/<short>/vX.Y.Z`, where `<short>` is the last part of the module id, and push it.

`release-module.yml` publishes the zip, its checksum, a catalog entry and the source. The app's release stays marked as latest. Release a host change before any module that needs it.

## Before tagging

- [ ] The app zip runs on a PC without the VC++ runtime (Windows Sandbox works).
- [ ] No ASIO SDK code is tracked. CI checks this.
- [ ] Any page that names ASIO has the logo, the ® and the trademark line.

To try the packaging locally: `tools/make-release.ps1 -Version <version> -AllowDirty`.
