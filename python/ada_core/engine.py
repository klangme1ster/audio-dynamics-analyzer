"""100 ms grid loudness / true-peak / PSR engine (spec §3.4-3.9)."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.ndimage import maximum_filter1d

from .kweight import KWeighting
from .true_peak import TruePeakDetector

ABSOLUTE_GATE = -70.0
INTEGRATED_RELATIVE_GATE = -10.0
LRA_RELATIVE_GATE = -20.0
PSR_ZONES = (("green", 12.0, np.inf), ("yellow", 10.0, 12.0),
             ("orange", 8.0, 10.0), ("red", -np.inf, 8.0))


def power_to_lufs(p):
    with np.errstate(divide="ignore"):
        return -0.691 + 10.0 * np.log10(p)


def window_sum(x: np.ndarray, width: int) -> np.ndarray:
    """Sum of x[n-width+1 .. n] with zeros before the start.

    Direct convolution instead of a cumulative-sum difference, which would lose
    precision in quiet passages after loud ones.
    """
    return np.convolve(x, np.ones(width))[:len(x)]


def percentile_index(count: int, p: float) -> int:
    """EBU Tech 3342 reference rule: floor((count - 1) * p + 0.5)."""
    return int(np.floor((count - 1) * p + 0.5))


def _opt(v) -> float | None:
    return None if v is None or not np.isfinite(v) else float(v)


@dataclass
class Result:
    sample_rate: int
    channels: int
    frames: int
    oversampling: int
    # per 100 ms bin n, stamped at (n + 1) * 0.1 s
    momentary: np.ndarray
    short_term: np.ndarray
    bin_peak: np.ndarray          # linear
    true_peak_3s: np.ndarray      # dBTP
    psr: np.ndarray               # NaN where gated
    # per 10 ms sub-bin u, stamped at (u + 1) * 0.01 s
    momentary_10ms: np.ndarray
    short_term_10ms: np.ndarray
    integrated: float | None
    lra: float | None
    true_peak_max: float          # dBTP (-inf for digital silence)
    max_momentary: float | None
    max_short_term: float | None
    plr: float | None
    psr_min: float | None
    psr_mean: float | None
    psr_p10: float | None
    psr_zone_percent: dict[str, float | None] = field(default_factory=dict)

    @property
    def duration(self) -> float:
        return self.frames / self.sample_rate

    @property
    def times(self) -> np.ndarray:
        return (np.arange(len(self.short_term)) + 1) * 0.1

    def summary(self) -> dict:
        """JSON-safe per-file report fields (spec §3.9); undefined values are None."""
        return {
            "duration_s": round(self.duration, 3),
            "sample_rate": self.sample_rate,
            "channels": self.channels,
            "oversampling": self.oversampling,
            "integrated_lufs": _opt(self.integrated),
            "lra": _opt(self.lra),
            "true_peak_max_dbtp": _opt(self.true_peak_max),
            "plr": _opt(self.plr),
            "max_momentary_lufs": _opt(self.max_momentary),
            "max_short_term_lufs": _opt(self.max_short_term),
            "psr_min": _opt(self.psr_min),
            "psr_mean": _opt(self.psr_mean),
            "psr_p10": _opt(self.psr_p10),
            **{f"psr_{zone}_pct": _opt(pct) for zone, pct in self.psr_zone_percent.items()},
        }


class Analyzer:
    """Streaming analyzer: feed (frames, channels) blocks of any length, then finish()."""

    def __init__(self, sample_rate: int, channels: int):
        if sample_rate % 100 != 0:
            raise ValueError(f"sample rate {sample_rate} Hz is not a multiple of 100 Hz")
        if channels not in (1, 2):
            raise ValueError(f"{channels} channels not supported (mono or stereo only)")
        self.fs = sample_rate
        self.channels = channels
        self.bin_size = sample_rate // 10
        self.sub_size = sample_rate // 100
        self.kweight = KWeighting(sample_rate, channels)
        self.true_peak = TruePeakDetector(sample_rate, channels)
        self.frames = 0
        self._sub_energy: list[np.ndarray] = []   # (sub-bins, channels) per chunk
        self._partial = np.zeros(channels)        # energy of the open sub-bin
        self._partial_len = 0
        self._peaks: list[np.ndarray] = []

    def process(self, x: np.ndarray) -> None:
        x = np.asarray(x, dtype=np.float64)
        if x.ndim == 1:
            x = x[:, None]
        if x.shape[1] != self.channels:
            raise ValueError(f"expected {self.channels} channels, got {x.shape[1]}")
        n = len(x)
        if n == 0:
            return
        self.frames += n
        self._peaks.append(self.true_peak.process(x))

        sq = self.kweight.process(x) ** 2
        need = self.sub_size - self._partial_len
        if n < need:
            self._partial += sq.sum(axis=0)
            self._partial_len += n
            return
        first = self._partial + sq[:need].sum(axis=0)
        full = (n - need) // self.sub_size
        end = need + full * self.sub_size
        body = sq[need:end].reshape(full, self.sub_size, self.channels).sum(axis=1)
        self._sub_energy.append(np.vstack([first, body]))
        self._partial = sq[end:].sum(axis=0)
        self._partial_len = n - end

    def finish(self) -> Result:
        H, h = self.bin_size, self.sub_size
        closed, tail_peak = self.true_peak.flush(self.frames)
        bin_peak = np.concatenate(self._peaks + [closed])

        sub = (np.vstack(self._sub_energy) if self._sub_energy
               else np.zeros((0, self.channels)))
        n_bins = self.frames // H
        assert len(bin_peak) == n_bins, (len(bin_peak), n_bins)

        # channel weights G_i = 1.0 for mono and stereo
        sub_power = sub.sum(axis=1)
        bin_power = sub_power[:n_bins * 10].reshape(n_bins, 10).sum(axis=1)

        block_power = window_sum(bin_power, 4) / (4 * H)        # 400 ms
        st_power = window_sum(bin_power, 30) / (30 * H)         # 3 s
        momentary = power_to_lufs(block_power)
        short_term = power_to_lufs(st_power)
        momentary_10ms = power_to_lufs(window_sum(sub_power, 40) / (40 * h))
        short_term_10ms = power_to_lufs(window_sum(sub_power, 300) / (300 * h))

        integrated = self._integrated(block_power[3:])
        lra, lra_gate = self._lra(st_power[29:])

        tp_3s_lin = maximum_filter1d(bin_peak, size=30, origin=14, mode="constant", cval=0.0) \
            if n_bins else bin_peak
        with np.errstate(divide="ignore"):
            true_peak_3s = 20.0 * np.log10(tp_3s_lin)
            tp_max_lin = max(bin_peak.max(initial=0.0), tail_peak)
            true_peak_max = float(20.0 * np.log10(tp_max_lin))

        gate = np.zeros(n_bins, bool)
        if lra_gate is not None:
            gate[29:] = (short_term[29:] >= ABSOLUTE_GATE) & (short_term[29:] >= lra_gate)
        with np.errstate(invalid="ignore"):
            psr = np.where(gate, true_peak_3s - short_term, np.nan)
        gated = np.sort(psr[gate])

        zones: dict[str, float | None] = {}
        for name, lo, hi in PSR_ZONES:
            zones[name] = (float(np.mean((gated >= lo) & (gated < hi)) * 100.0)
                           if gated.size else None)

        def finite_max(a):
            a = a[np.isfinite(a)]
            return float(a.max()) if a.size else None

        return Result(
            sample_rate=self.fs, channels=self.channels, frames=self.frames,
            oversampling=self.true_peak.factor,
            momentary=momentary, short_term=short_term, bin_peak=bin_peak,
            true_peak_3s=true_peak_3s, psr=psr,
            momentary_10ms=momentary_10ms, short_term_10ms=short_term_10ms,
            integrated=integrated, lra=lra, true_peak_max=true_peak_max,
            max_momentary=finite_max(momentary_10ms[39:]),
            max_short_term=finite_max(short_term_10ms[299:]),
            plr=(true_peak_max - integrated
                 if integrated is not None and np.isfinite(true_peak_max) else None),
            psr_min=float(gated[0]) if gated.size else None,
            psr_mean=float(gated.mean()) if gated.size else None,
            psr_p10=float(gated[percentile_index(gated.size, 0.10)]) if gated.size else None,
            psr_zone_percent=zones,
        )

    @staticmethod
    def _integrated(z: np.ndarray) -> float | None:
        """BS.1770 two-stage gating over 400 ms block powers (spec §3.5)."""
        l = power_to_lufs(z)
        abs_kept = z[l > ABSOLUTE_GATE]
        if abs_kept.size == 0:
            return None
        rel_gate = power_to_lufs(abs_kept.mean()) + INTEGRATED_RELATIVE_GATE
        kept = z[(l > ABSOLUTE_GATE) & (l > rel_gate)]
        return float(power_to_lufs(kept.mean())) if kept.size else None

    @staticmethod
    def _lra(st_power: np.ndarray) -> tuple[float | None, float | None]:
        """EBU Tech 3342 loudness range (spec §3.6). Returns (LRA, relative gate)."""
        s = power_to_lufs(st_power)
        abs_mask = s >= ABSOLUTE_GATE
        if not abs_mask.any():
            return None, None
        rel_gate = float(power_to_lufs(st_power[abs_mask].mean())) + LRA_RELATIVE_GATE
        kept = np.sort(s[abs_mask & (s >= rel_gate)])
        if kept.size < 2:
            return None, rel_gate
        lo = kept[percentile_index(kept.size, 0.10)]
        hi = kept[percentile_index(kept.size, 0.95)]
        return float(hi - lo), rel_gate


def analyze_file(path: str | Path, block_seconds: float = 5.0) -> Result:
    """Stream an audio file through the analyzer with bounded memory."""
    info = sf.info(str(path))
    analyzer = Analyzer(info.samplerate, info.channels)
    blocksize = int(block_seconds * 10) * analyzer.bin_size
    for block in sf.blocks(str(path), blocksize=blocksize, dtype="float32", always_2d=True):
        analyzer.process(block)
    return analyzer.finish()
