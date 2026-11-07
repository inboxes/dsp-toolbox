# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Record** button and PRE/POST choice in the Transport panel.
- `dsptb-cli render --working-file`: the file modules see as the working file, such as a backing track.
- Modules can read the working file (`acquire_working_file` in the host API). With the file playing beside the mic, `transport.position_seconds` is the file position.
- **Input routes:** the file and the mic can each go to the chain, straight to the output at their own volume, or off. **Mic + track** is a one-click setup for singing along: the mic goes through the chain and the file plays as a backing track.

### Changed

- Session files are now format version 2. Older sessions still open, but version 0.2.0 can't open a session saved by this version.

## [0.2.0] - 2026-11-07

### Added

- Offline modules: **Run**, progress and **Cancel** in the Chain panel, for modules that analyze a whole file. They read the file at its own sample rate, not the copy resampled for playback. `dsptb-cli` and `validate` handle them too.
- **Spectrogram:** an optional note grid on the Log axis.
- `dsp_common`: tuning and note maths, a semitone-spaced pitch spectrum, and a tuning estimator.

### Changed

- Key detection estimates the recording's tuning instead of assuming A4 = 440 Hz.
- Installed modules live in the `modules` folder next to the app, not in `%LOCALAPPDATA%`. Install any you added under 0.1.0 again; the app can't write to a folder like `Program Files`, so keep it somewhere you can write to.

### Fixed

- Analyzers stop while a file is paused or has ended, instead of scrolling silence.
- `dsptb-cli` logs to stderr, so a log line can't break `--json` output.

## [0.1.0] - 2026-11-06

First release.

### Added

- Audio file (WAV, FLAC, MP3) and microphone sources, with a transport, looping and a waveform view.
- WASAPI shared and exclusive audio, and ASIO® drivers.
- A plugin system: modules are DLLs with a manifest, each in its own dockable panel, with undoable parameters.
- **Install from .zip**, which refuses unsafe zip files before writing anything.
- **Spectrogram** with linear, log and mel axes.
- **Level / Loudness Meter:** peak, RMS, true peak and EBU R128 loudness.
- **Gain**, a reference module.
- Sessions with autosave, export of processed audio to WAV, and recording of the input or output.
- `dsptb-cli` for test signals, file analysis, offline rendering, benchmarks and module validation.
- `dsptb-sofa` for inspecting SOFA HRTF files and cutting one distance out of a multi-distance file.
- A module SDK with a project template.

The 3D Spatializer is a separate download with its own release and changelog.

ASIO is a registered trademark of Steinberg Media Technologies GmbH.
