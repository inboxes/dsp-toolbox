#!/usr/bin/env python3
"""NumPy model of the tuning estimate and key detection in dsp_common.

Dev-only: not part of the build or CI. Use it to sanity-check C++ results, such as margins and tuning
offsets, before chasing a failing test. Needs numpy (tools/requirements.txt).

    python modules/common/tools/pitch_model.py
"""
import numpy as np

MAJOR = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
MINOR = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])
FS = 48000
FFT = 8192
HOP = 4096


def chord(notes, fs, seconds=1.0, amp=0.1, harmonics=1, a4=440.0):
    n = np.arange(int(round(seconds * fs)))
    x = np.zeros(len(n))
    for m in notes:
        f0 = a4 * 2 ** ((m - 69) / 12)
        for h in range(1, harmonics + 1):
            if f0 * h < fs / 2:
                x += (amp / h) * np.sin(2 * np.pi * f0 * h * n / fs)
    return x


def progression(root, minor, fs, **kw):
    """I-IV-V-I, or i-iv-V-i with a major V, each chord 1 s, played twice."""
    third = 3 if minor else 4
    chords = [(root, root + third, root + 7), (root + 5, root + 5 + third, root + 12),
              (root + 7, root + 11, root + 14), (root, root + third, root + 7)]
    return np.concatenate([chord(list(c) + [c[0] - 12], fs, **kw) for _ in range(2) for c in chords])


def power_frames(x, n=FFT, hop=HOP):
    w = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(n) / n)  # periodic Hann
    return np.array([np.abs(np.fft.rfft(x[s:s + n] * w)) ** 2 for s in range(0, len(x) - n + 1, hop)])


def peaks(frames, fs=FS, n=FFT, fmin=55.0, fmax=1760.0, floor=0.01):
    """Interpolated spectral peaks of all frames, as (hz, magnitude)."""
    out = []
    for p in frames:
        s = np.sqrt(p)
        kmin = max(1, int(np.ceil(fmin * n / fs)))
        kmax = min(len(s) - 2, int(np.floor(fmax * n / fs)))
        threshold = floor * s[kmin:kmax + 1].max()
        for k in range(kmin, kmax + 1):
            if s[k] > s[k - 1] and s[k] >= s[k + 1] and s[k] >= threshold and s[k] > 0:
                a, b, c = np.log(s[k - 1:k + 2] + 1e-30)
                d = 0.5 * (a - c) / (a - 2 * b + c)
                out.append(((k + d) * fs / n, np.exp(b - 0.25 * (a - c) * d)))
    return out


def estimate_tuning(pk):
    """Returns (offset cents, confidence). The offset is the mode of a circular histogram of deviations."""
    histogram = np.zeros(100)
    z = 0j
    w = 0.0
    for f, m in pk:
        midi = 69 + 12 * np.log2(f / 440)
        dev = 100 * (midi - np.floor(midi + 0.5))
        histogram[int(np.floor(dev + 50)) % 100] += m
        z += m * np.exp(2j * np.pi * dev / 100)
        w += m
    kernel = np.array([1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1], float)
    smooth = sum(k * np.roll(histogram, i - 5) for i, k in enumerate(kernel))
    i = int(np.argmax(smooth))
    a, b, c = smooth[(i - 1) % 100], smooth[i], smooth[(i + 1) % 100]
    denom = a - 2 * b + c
    d = 0.5 * (a - c) / denom if denom != 0 else 0
    cents = ((i + 0.5 + d - 50 + 50) % 100) - 50
    return cents, abs(z) / w if w > 0 else 0.0


def chroma(pk, a4):
    """Pitch classes with a cos^2 split between the two nearest notes."""
    out = np.zeros(12)
    for f, m in pk:
        semitones = 12 * np.log2(f / a4) + 9
        low = np.floor(semitones)
        frac = semitones - low
        out[int(low) % 12] += m * np.cos(np.pi * frac / 2) ** 2
        out[int(low + 1) % 12] += m * np.sin(np.pi * frac / 2) ** 2
    return out


def score(c):
    """Returns (root, mode, correlation, margin). The margin ignores the winner's relative key."""
    ranked = sorted(((np.corrcoef(c, np.roll(profile, r))[0, 1], r, mode)
                     for mode, profile in (('maj', MAJOR), ('min', MINOR)) for r in range(12)), reverse=True)
    best, root, mode = ranked[0]
    relative = ((root + 9) % 12, 'min') if mode == 'maj' else ((root + 3) % 12, 'maj')
    margin = best - next(r for r in ranked[1:] if (r[1], r[2]) != relative)[0]
    return root, mode, best, margin


def main():
    print('Tuning estimate:')
    for a4, harmonics in [(440, 1), (432, 1), (446, 1), (452, 1), (428, 1), (440, 6), (432, 6), (452, 6)]:
        truth = ((1200 * np.log2(a4 / 440) + 50) % 100) - 50
        errors = []
        for minor in (False, True):
            for root in range(48, 60, 3):
                pk = peaks(power_frames(progression(root, minor, FS, harmonics=harmonics, a4=a4)))
                errors.append(((estimate_tuning(pk)[0] - truth + 50) % 100) - 50)
        print(f'  A4={a4} harmonics={harmonics}: true {truth:+.1f} c, max |error| {np.max(np.abs(errors)):.2f} c')
    noise = np.random.default_rng(1).uniform(-0.1, 0.1, FS * 10)
    print(f'  confidence on noise: {estimate_tuning(peaks(power_frames(noise)))[1]:.3f}')

    print('Key, 24 progressions:')
    for a4 in (452, 428):
        result = {'fixed 440': [0, 9.0], 'estimated': [0, 9.0]}
        for minor in (False, True):
            for root in range(48, 60):
                pk = peaks(power_frames(progression(root, minor, FS, a4=a4)))
                cents, _ = estimate_tuning(pk)
                for label, reference in (('fixed 440', 440.0), ('estimated', 440 * 2 ** (cents / 1200))):
                    found, mode, r, margin = score(chroma(pk, reference))
                    right = (found, mode) == (root % 12, 'min' if minor else 'maj') and r >= 0.6 and margin >= 0.05
                    result[label][0] += not right
                    result[label][1] = min(result[label][1], margin)
        for label, (failures, worst) in result.items():
            print(f'  A4={a4} {label}: {failures}/24 fail, worst margin {worst:.3f}')


if __name__ == '__main__':
    main()
