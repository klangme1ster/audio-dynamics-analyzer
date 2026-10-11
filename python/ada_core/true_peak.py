"""BS.1770 Annex 2 true-peak detection on the 100 ms bin grid (spec §3.3)."""

from __future__ import annotations

import numpy as np
from scipy.signal import lfilter

# ITU-R BS.1770-5 Annex 2 interpolation filter, verbatim: row j, column p is
# coefficient h[4j + p] of the 48-tap prototype; column p is polyphase branch p.
ANNEX2_TABLE = np.array([
    [0.0017089843750, -0.0291748046875, -0.0189208984375, -0.0083007812500],
    [0.0109863281250, 0.0292968750000, 0.0330810546875, 0.0148925781250],
    [-0.0196533203125, -0.0517578125000, -0.0582275390625, -0.0266113281250],
    [0.0332031250000, 0.0891113281250, 0.1015625000000, 0.0476074218750],
    [-0.0594482421875, -0.1665039062500, -0.2003173828125, -0.1022949218750],
    [0.1373291015625, 0.4650878906250, 0.7797851562500, 0.9721679687500],
    [0.9721679687500, 0.7797851562500, 0.4650878906250, 0.1373291015625],
    [-0.1022949218750, -0.2003173828125, -0.1665039062500, -0.0594482421875],
    [0.0476074218750, 0.1015625000000, 0.0891113281250, 0.0332031250000],
    [-0.0266113281250, -0.0582275390625, -0.0517578125000, -0.0196533203125],
    [0.0148925781250, 0.0330810546875, 0.0292968750000, 0.0109863281250],
    [-0.0083007812500, -0.0189208984375, -0.0291748046875, 0.0017089843750],
])

# Interpolated outputs lag the input by 5.125-5.875 samples; attribute them to k - D.
INTERPOLATOR_DELAY = 6


def oversampling_factor(fs: int) -> int:
    if fs <= 48000:
        return 4
    if fs <= 96000:
        return 2
    return 1


class TruePeakDetector:
    """Streaming per-bin true peak (linear) for (frames, channels) blocks.

    `process()` returns the peaks of bins that were completed by this call.
    `flush()` closes the stream and returns (remaining complete-bin peaks,
    trailing partial-bin peak).
    """

    def __init__(self, fs: int, channels: int):
        self.bin_size = fs // 10
        self.channels = channels
        self.factor = oversampling_factor(fs)
        phases = {4: (0, 1, 2, 3), 2: (0, 2), 1: ()}[self.factor]
        self.taps = [ANNEX2_TABLE[:, p] for p in phases]
        self.delay = INTERPOLATOR_DELAY if self.taps else 0
        self.reset()

    def reset(self) -> None:
        taps = ANNEX2_TABLE.shape[0]
        self.zi = [np.zeros((taps - 1, self.channels)) for _ in self.taps]
        self.consumed = 0      # input samples fed so far (including flush zeros)
        self.open_bin = 0
        self.open_max = 0.0

    def _magnitude(self, x: np.ndarray) -> np.ndarray:
        if not self.taps:
            return np.abs(x).max(axis=1)
        mag = None
        for i, h in enumerate(self.taps):
            y, self.zi[i] = lfilter(h, [1.0], x, axis=0, zi=self.zi[i])
            m = np.abs(y).max(axis=1)
            mag = m if mag is None else np.maximum(mag, m)
        return mag

    def process(self, x: np.ndarray) -> np.ndarray:
        n = len(x)
        if n == 0:
            return np.empty(0)
        mag = self._magnitude(np.asarray(x, dtype=np.float64))
        # output i represents input time tau = consumed + i - delay (clamped at 0)
        tau0 = self.consumed - self.delay
        self.consumed += n
        tau_last = tau0 + n - 1
        if tau_last < 0:
            self.open_max = max(self.open_max, float(mag.max()))
            return np.empty(0)

        first_bin = max(tau0, 0) // self.bin_size
        last_bin = tau_last // self.bin_size
        starts = [0] + [b * self.bin_size - tau0 for b in range(first_bin + 1, last_bin + 1)]
        seg_max = np.maximum.reduceat(mag, starts)
        if first_bin == self.open_bin:
            seg_max[0] = max(seg_max[0], self.open_max)
            closed = seg_max[:-1]
        else:  # previous call ended exactly on a bin boundary
            closed = np.concatenate([[self.open_max], seg_max[:-1]])

        self.open_bin, self.open_max = last_bin, float(seg_max[-1])
        return closed

    def flush(self, total_samples: int) -> tuple[np.ndarray, float]:
        closed = self.process(np.zeros((self.delay, self.channels)))
        if total_samples % self.bin_size == 0 and total_samples > 0:
            return np.append(closed, self.open_max), 0.0
        return closed, self.open_max
