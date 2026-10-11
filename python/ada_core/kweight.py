"""ITU-R BS.1770 K-weighting filter (spec §3.2)."""

from __future__ import annotations

import math

import numpy as np
from scipy.signal import sosfilt

# Analog prototype parameters (same derivation as libebur128); at 48 kHz they
# reproduce the BS.1770 Table 1/2 coefficients to machine precision.
SHELF_F0 = 1681.974450955533
SHELF_GAIN_DB = 3.999843853973347
SHELF_Q = 0.7071752369554196
SHELF_VB_EXP = 0.4996667741545416
HIGHPASS_F0 = 38.13547087602444
HIGHPASS_Q = 0.5003270373238773


def kweight_sos(fs: float) -> np.ndarray:
    """Second-order sections (2, 6) for the two K-weighting stages at sample rate fs."""
    k = math.tan(math.pi * SHELF_F0 / fs)
    vh = 10.0 ** (SHELF_GAIN_DB / 20.0)
    vb = vh ** SHELF_VB_EXP
    a0 = 1.0 + k / SHELF_Q + k * k
    shelf = [
        (vh + vb * k / SHELF_Q + k * k) / a0,
        2.0 * (k * k - vh) / a0,
        (vh - vb * k / SHELF_Q + k * k) / a0,
        1.0,
        2.0 * (k * k - 1.0) / a0,
        (1.0 - k / SHELF_Q + k * k) / a0,
    ]

    k = math.tan(math.pi * HIGHPASS_F0 / fs)
    a0 = 1.0 + k / HIGHPASS_Q + k * k
    highpass = [
        1.0, -2.0, 1.0,
        1.0,
        2.0 * (k * k - 1.0) / a0,
        (1.0 - k / HIGHPASS_Q + k * k) / a0,
    ]
    return np.array([shelf, highpass])


class KWeighting:
    """Streaming K-weighting for (frames, channels) blocks; state starts at zero."""

    def __init__(self, fs: float, channels: int):
        self.sos = kweight_sos(fs)
        self.channels = channels
        self.reset()

    def reset(self) -> None:
        self.zi = np.zeros((self.sos.shape[0], 2, self.channels))

    def process(self, x: np.ndarray) -> np.ndarray:
        y, self.zi = sosfilt(self.sos, x, axis=0, zi=self.zi)
        return y
