# Changelog: Audio Analysis

## [0.1.0] - Unreleased

First release. `minHostVersion` is 0.2.0, the first app version that runs offline modules, which File Analysis needs.

### Added

- **Pitch:** frequency, note and cents over time.
- **Onset:** detected onsets and the novelty curve, with an approximate tempo.
- **MFCC:** a scrolling heatmap of the coefficients.
- **Spectral Features:** centroid, spread, rolloff, flatness, flux, zero-crossing rate and RMS.
- **File Analysis:** loudness, peaks, DC offset and a whole-file spectrogram.
- **Export CSV** in each, written to the `exports` folder in the app's data folder.
