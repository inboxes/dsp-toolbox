# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Spectrogram:** an optional note grid on the Log axis.
- `dsp_common`: tuning and note maths, a semitone-spaced pitch spectrum, and a tuning estimator.

### Changed

- Key detection estimates the recording's tuning instead of assuming A4 = 440 Hz.

## [0.1.0] - Unreleased

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
