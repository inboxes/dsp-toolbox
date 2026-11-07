#!/usr/bin/env python3
"""Generate golden data for the dsp_common tests.

Dev-only tool.
Run with the pinned environment:

    pip install -r modules/common/tools/requirements.txt
    python modules/common/tools/gen_goldens.py

Sections: mel conversions, and mel filterbank / MFCC goldens.
Extend this file with new sections; add a
new section without changing the existing ones.

The generated JSON files are committed, so builds need no Python.
"""

import json
from pathlib import Path

import librosa
import numpy as np

TOOLS_DIR = Path(__file__).resolve().parent
GOLDEN_DIR = TOOLS_DIR.parent / "tests" / "golden"

# Frequencies fixed by the acceptance criteria: they straddle
# the Slaney scale's 1000 Hz linear/log join on both sides.
MEL_FREQUENCIES_HZ = [0.0, 100.0, 999.0, 1000.0, 1001.0, 4000.0, 20000.0]


def gen_mel_conversions() -> None:
    """Write tests/golden/mel_conversions.json from librosa.

    Slaney values come from librosa.hz_to_mel(htk=False) and the HTK
    values from librosa.hz_to_mel(htk=True), at full float64 precision
    (repr via json's default float encoding), so the C++ test can check
    the 1e-6 relative tolerance from the issue.
    """
    freqs = np.array(MEL_FREQUENCIES_HZ, dtype=np.float64)
    data = {
        "frequencies_hz": [float(f) for f in freqs],
        "slaney_mel": [float(m) for m in librosa.hz_to_mel(freqs, htk=False)],
        "htk_mel": [float(m) for m in librosa.hz_to_mel(freqs, htk=True)],
    }
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    out_path = GOLDEN_DIR / "mel_conversions.json"
    with out_path.open("w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")
    print(f"wrote {out_path}")


# ---------------------------------------------------------------------------
# Spectral features on goldenSignal(48000)
# ---------------------------------------------------------------------------

# Framing fixed by the acceptance criteria.
FEATURES_FS = 48000
FEATURES_N_FFT = 2048
FEATURES_HOP = 512


def _golden_signal(fs: int) -> np.ndarray:
    """The spec section 3 goldenSignal, sample for sample.

    0.5*sin(2*pi*440*n/fs) + 0.25*sin(2*pi*1250*n/fs) + lcgNoise(seed=1,
    amp=0.05), where the LCG state updates before each sample. Each term
    is rounded to float32 before the sum, exactly as the C++ test signal
    in tests/support/signals.hpp does, so both sides see identical input.
    """
    n = np.arange(fs, dtype=np.float64)
    sine_a = (0.5 * np.sin(2.0 * np.pi * 440.0 * n / fs)).astype(np.float32)
    sine_b = (0.25 * np.sin(2.0 * np.pi * 1250.0 * n / fs)).astype(np.float32)
    state = 1
    noise = np.empty(fs, dtype=np.float32)
    for i in range(fs):
        state = (1664525 * state + 1013904223) % 2**32
        noise[i] = np.float32(0.05 * (2.0 * (state / 2**32) - 1.0))
    total = sine_a.astype(np.float64) + sine_b.astype(np.float64) + noise.astype(np.float64)
    return total.astype(np.float32)


def gen_spectral_features() -> None:
    """Write tests/golden/spectral_features.json from librosa.

    Per-frame centroid, spread (spectral_bandwidth, p=2), rolloff
    (roll_percent=0.85), flatness (spectral_flatness, power=2) and ZCR
    (zero_crossing_rate, center=False) of goldenSignal(48000), fft 2048,
    hop 512, center=False — the acceptance framing. Values are
    stored at full float64 precision; the C++ test tolerance is
    1e-3 * max(1, |golden|).
    """
    y = _golden_signal(FEATURES_FS)
    spectrum = np.abs(
        librosa.stft(y, n_fft=FEATURES_N_FFT, hop_length=FEATURES_HOP, window="hann", center=False)
    )
    data = {
        "sample_rate": FEATURES_FS,
        "fft_size": FEATURES_N_FFT,
        "hop": FEATURES_HOP,
        "frames": int(spectrum.shape[1]),
        "centroid": [float(v) for v in librosa.feature.spectral_centroid(S=spectrum, sr=FEATURES_FS)[0]],
        "spread": [
            float(v) for v in librosa.feature.spectral_bandwidth(S=spectrum, sr=FEATURES_FS, p=2)[0]
        ],
        "rolloff": [
            float(v)
            for v in librosa.feature.spectral_rolloff(S=spectrum, sr=FEATURES_FS, roll_percent=0.85)[0]
        ],
        "flatness": [
            float(v) for v in librosa.feature.spectral_flatness(S=spectrum, power=2.0)[0]
        ],
        "zcr": [
            float(v)
            for v in librosa.feature.zero_crossing_rate(
                y, frame_length=FEATURES_N_FFT, hop_length=FEATURES_HOP, center=False
            )[0]
        ],
    }
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    out_path = GOLDEN_DIR / "spectral_features.json"
    with out_path.open("w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")
# Mel filterbank + MFCC goldens.
#
# The pipeline matched by the C++ implementation:
#   S = |librosa.stft(y, n_fft=2048, hop=512, window='hann', center=False)|^2
#   M = librosa.feature.melspectrogram(S=S, sr=48000, n_mels=40, fmin=0,
#                                      fmax=None, htk=False, norm='slaney')
#   L = librosa.power_to_db(M, ref=1.0, amin=1e-10, top_db=None)
#   C = scipy.fft.dct(L, axis=0, type=2, norm='ortho')[:13]
# librosa's 'hann' window is the periodic Hann (scipy get_window with
# fftbins=True): 0.5 - 0.5*cos(2*pi*i/N), the same formula as the SDK
# window used with the project's StftAnalyzer/RealFft.
# ---------------------------------------------------------------------------

from scipy.fft import dct as _scipy_dct

GOLDEN_FS = 48000
GOLDEN_N_FFT = 2048
GOLDEN_HOP = 512
GOLDEN_N_MELS = 40
GOLDEN_N_MFCC = 13


def golden_signal(fs: float) -> np.ndarray:
    """goldenSignal(fs) from spec section 3, as float32 samples.

    Mirrors tests/support/signals.hpp operation for operation: each
    component (two sines, LCG noise) is rounded to float32 first, the
    components are summed in float64, and the sum is rounded to float32,
    so the Python and C++ signals are bit-for-bit identical.
    """
    count = int(fs)
    n = np.arange(count, dtype=np.float64)
    sine1 = (0.5 * np.sin(2.0 * np.pi * 440.0 * n / fs)).astype(np.float32)
    sine2 = (0.25 * np.sin(2.0 * np.pi * 1250.0 * n / fs)).astype(np.float32)
    state = 1  # LCG seed (spec section 3)
    noise = np.empty(count, dtype=np.float64)
    for i in range(count):
        state = (1664525 * state + 1013904223) % (1 << 32)
        noise[i] = 0.05 * (2.0 * (state / 4294967296.0) - 1.0)
    total = sine1.astype(np.float64) + sine2.astype(np.float64) + noise.astype(np.float32).astype(np.float64)
    return total.astype(np.float32)


def _fmt_array(values: np.ndarray, fmt: str) -> str:
    return "[" + ",".join(fmt % float(v) for v in values) + "]"


def gen_mel_filterbank() -> None:
    """Write tests/golden/mel_filterbank.json from librosa.filters.mel.

    All three configurations the test checks: slaney/htk=False and
    none/htk=True (the acceptance pair) plus slaney/htk=True, the fourth
    combination, which the generator already loops over cheaply.
    Rows are stored sparsely -- first nonzero bin index plus the nonzero
    weights at %.9g -- because the dense 40 x 1025 matrices would break
    the issue's 200 kB golden-file budget; %.9g keeps the stored values
    within 5e-10 relative of librosa's float32 output, far inside the
    1e-6 absolute test tolerance.
    """
    configs = []
    for htk, norm in ((False, "slaney"), (True, None), (True, "slaney")):
        weights = librosa.filters.mel(
            sr=GOLDEN_FS, n_fft=GOLDEN_N_FFT, n_mels=GOLDEN_N_MELS, fmin=0.0, fmax=None, htk=htk, norm=norm
        )
        rows = []
        for row in weights:
            nonzero = np.nonzero(row)[0]
            # The sparse encoding assumes each row's nonzeros form one
            # contiguous run (true for triangular filters). Assert it, so a
            # future librosa change fails loudly here instead of silently
            # corrupting the golden.
            if nonzero.size and int(nonzero[-1]) - int(nonzero[0]) + 1 != int(nonzero.size):
                raise SystemExit("mel filterbank row has non-contiguous nonzero support; sparse encoding invalid")
            first = int(nonzero[0]) if nonzero.size else int(row.shape[0])
            values = row[nonzero] if nonzero.size else row[:0]
            rows.append('{"first": %d, "values": %s}' % (first, _fmt_array(values, "%.9g")))
        norm_json = '"slaney"' if norm == "slaney" else "null"
        configs.append(
            '{"htk": %s, "norm": %s, "rows": [%s]}' % ("true" if htk else "false", norm_json, ",".join(rows))
        )
    text = (
        '{\n"sample_rate": %d, "fft_size": %d, "n_mels": %d, "bins": %d, "fmin": 0.0, "fmax": %s,\n'
        '"configs": [%s]\n}\n'
        % (
            GOLDEN_FS,
            GOLDEN_N_FFT,
            GOLDEN_N_MELS,
            GOLDEN_N_FFT // 2 + 1,
            repr(float(GOLDEN_FS) / 2.0),
            ",".join(configs),
        )
    )
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    out_path = GOLDEN_DIR / "mel_filterbank.json"
    out_path.write_text(text, encoding="utf-8")
    print(f"wrote {out_path}")


def gen_mfcc_golden() -> None:
    """Write tests/golden/mfcc_golden.json: signal head, log-mel, MFCC.

    Values at %.9g; the C++ test tolerance is 1e-3 * max(1, |golden|),
    so the stored rounding (~5e-10 relative) is negligible. The first 32
    signal samples are stored at full float32 repr because the test
    compares them to 1e-7.
    """
    y = golden_signal(GOLDEN_FS)
    spectrum = np.abs(
        librosa.stft(y, n_fft=GOLDEN_N_FFT, hop_length=GOLDEN_HOP, window="hann", center=False)
    ) ** 2
    mel = librosa.feature.melspectrogram(
        S=spectrum, sr=GOLDEN_FS, n_mels=GOLDEN_N_MELS, fmin=0, fmax=None, htk=False, norm="slaney"
    )
    log_mel = librosa.power_to_db(mel, ref=1.0, amin=1e-10, top_db=None)
    mfcc = _scipy_dct(log_mel, axis=0, type=2, norm="ortho")[:GOLDEN_N_MFCC]
    frames = spectrum.shape[1]
    logmel_frames = ",".join(_fmt_array(log_mel[:, t], "%.9g") for t in range(frames))
    mfcc_frames = ",".join(_fmt_array(mfcc[:, t], "%.9g") for t in range(frames))
    signal_head = "[" + ",".join(repr(float(v)) for v in y[:32]) + "]"
    text = (
        '{\n"sample_rate": %d, "fft_size": %d, "hop": %d, "n_mels": %d, "n_mfcc": %d, "frames": %d,\n'
        '"signal_first32": %s,\n"logmel": [%s],\n"mfcc": [%s]\n}\n'
        % (GOLDEN_FS, GOLDEN_N_FFT, GOLDEN_HOP, GOLDEN_N_MELS, GOLDEN_N_MFCC, frames, signal_head, logmel_frames, mfcc_frames)
    )
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    out_path = GOLDEN_DIR / "mfcc_golden.json"
    out_path.write_text(text, encoding="utf-8")
    print(f"wrote {out_path}")


def main() -> None:
    gen_mel_conversions()
    gen_spectral_features()
    gen_mel_filterbank()
    gen_mfcc_golden()


if __name__ == "__main__":
    main()
