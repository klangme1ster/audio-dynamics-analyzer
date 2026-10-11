"""Test-signal synthesizers for spec §2 (EBU Tech 3341 / 3342 and ADA-* signals).

Levels are sine peak levels in dBFS, as in Tech 3341. All generators return
float64 arrays of shape (frames, channels).
"""

from __future__ import annotations

import numpy as np


def db_to_amp(db: float) -> float:
    return 10.0 ** (db / 20.0)


def sine(fs: int, seconds: float, level_db: float, freq: float = 1000.0,
         phase_deg: float = 0.0, channels: int = 2, start: int = 0) -> np.ndarray:
    n = np.arange(start, start + int(round(seconds * fs)))
    x = db_to_amp(level_db) * np.sin(2 * np.pi * freq * n / fs + np.deg2rad(phase_deg))
    return np.repeat(x[:, None], channels, axis=1)


def tone_sequence(fs: int, segments: list[tuple[float, float]], freq: float = 1000.0,
                  channels: int = 2) -> np.ndarray:
    """Concatenate (seconds, level_db) tone segments with continuous phase."""
    parts, start = [], 0
    for seconds, level in segments:
        part = sine(fs, seconds, level, freq, channels=channels, start=start)
        parts.append(part)
        start += len(part)
    return np.concatenate(parts)


def peak_test_sine(fs: int, freq_div: float, amplitude: float, phase_deg: float,
                   seconds: float = 5.0, channels: int = 2, fade_s: float = 0.0) -> np.ndarray:
    """Tech 3341 cases 15-19: sine at fs/freq_div with the given linear amplitude and phase.

    An abrupt start is a step whose reconstruction overshoots; fade_s applies raised-cosine
    fades so only the steady tone is measured, as Tech 3341 intends.
    """
    n = np.arange(int(seconds * fs))
    x = amplitude * np.sin(2 * np.pi * n / freq_div + np.deg2rad(phase_deg))
    nf = int(fade_s * fs)
    if nf:
        ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(nf) / nf)
        x[:nf] *= ramp
        x[-nf:] *= ramp[::-1]
    return np.repeat(x[:, None], channels, axis=1)


def pink_noise(fs: int, frames: int, level_db: float = -20.0, channels: int = 2,
               seed: int = 1) -> np.ndarray:
    rng = np.random.default_rng(seed)
    white = rng.standard_normal((frames, channels))
    spec = np.fft.rfft(white, axis=0)
    f = np.fft.rfftfreq(frames, 1.0 / fs)
    f[0] = f[1]
    spec /= np.sqrt(f)[:, None]
    x = np.fft.irfft(spec, n=frames, axis=0)
    return x * (db_to_amp(level_db) / np.sqrt(np.mean(x ** 2)))


# --- Tech 3341 ---------------------------------------------------------------

def t3341_case(case: int, fs: int = 48000) -> np.ndarray:
    seq = {
        1: [(20, -23.0)],
        2: [(20, -33.0)],
        3: [(10, -36.0), (60, -23.0), (10, -36.0)],
        4: [(10, -72.0), (10, -36.0), (60, -23.0), (10, -36.0), (10, -72.0)],
        5: [(20, -26.0), (20.1, -20.0), (20, -26.0)],
        9: [(1.34, -20.0), (1.66, -30.0)] * 5,
        12: [(0.18, -20.0), (0.22, -30.0)] * 25,
    }[case]
    return tone_sequence(fs, seq)


# (freq divisor, amplitude, phase) for Tech 3341 cases 15-19
T3341_TRUE_PEAK = {
    15: (4, 0.50, 0.0),
    16: (4, 0.50, 45.0),
    17: (6, 0.50, 60.0),
    18: (8, 0.50, 67.5),
    19: (4, 1.41, 45.0),
}


# --- Tech 3342 ---------------------------------------------------------------

def t3342_case(case: int, fs: int = 48000) -> np.ndarray:
    seq = {
        1: [(20, -20.0), (20, -30.0)],
        2: [(20, -20.0), (20, -15.0)],
        3: [(20, -40.0), (20, -20.0)],
        4: [(20, -50.0), (20, -35.0), (20, -20.0), (20, -35.0), (20, -50.0)],
    }[case]
    return tone_sequence(fs, seq)
