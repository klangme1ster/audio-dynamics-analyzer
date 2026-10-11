"""Spec §2.2 — ADA-specific tests of filters, alignment, chunking and edge cases."""

from __future__ import annotations

import json
import math

import numpy as np
import pytest

from ada_core.kweight import kweight_sos
from ada_core.true_peak import ANNEX2_TABLE, oversampling_factor, TruePeakDetector
from conftest import analyze_array
import signals


def tp_ok(measured: float, expected: float) -> bool:
    return expected - 0.4 <= measured <= expected + 0.2


# --- ADA-KCOEF ----------------------------------------------------------------

def test_kweight_coefficients_48k():
    sos = kweight_sos(48000)
    expected = np.array([
        [1.53512485958697, -2.69169618940638, 1.19839281085285,
         1.0, -1.69065929318241, 0.73248077421585],
        [1.0, -2.0, 1.0, 1.0, -1.99004745483398, 0.99007225036621],
    ])
    assert np.max(np.abs(sos - expected)) <= 1e-12


# --- ADA-FIR ------------------------------------------------------------------

def test_annex2_filter_integrity():
    assert ANNEX2_TABLE.shape == (12, 4)
    h = ANNEX2_TABLE.reshape(-1)  # h[4j + p] = table[j, p]
    assert h.size == 48
    assert np.array_equal(h, h[::-1])
    np.testing.assert_allclose(ANNEX2_TABLE.sum(axis=0),
                               [1.00158691, 0.97302246, 0.97302246, 1.00158691], atol=1e-8)


# --- ADA-ISP / ADA-RATE -------------------------------------------------------

@pytest.mark.parametrize("fs", [44100, 48000, 88200, 96000])
def test_inter_sample_peak(fs):
    x = signals.peak_test_sine(fs, 4, math.sqrt(2), 45.0, seconds=20, fade_s=0.05)
    assert np.max(np.abs(x)) == pytest.approx(1.0, abs=1e-6)
    r = analyze_array(x, fs)
    assert tp_ok(r.true_peak_max, 3.01), r.true_peak_max


@pytest.mark.parametrize("fs,factor", [(32000, 4), (44100, 4), (48000, 4), (88200, 2),
                                       (96000, 2), (176400, 1), (192000, 1)])
def test_oversampling_factor(fs, factor):
    assert oversampling_factor(fs) == factor
    assert TruePeakDetector(fs, 2).factor == factor


# --- ADA-ALIGN ----------------------------------------------------------------

def test_causal_window_alignment():
    fs, n0 = 48000, 50
    bed = signals.sine(fs, 20, -30.0)
    x = bed.copy()
    start = n0 * fs // 10 + int(0.03 * fs)  # burst at 5.03-5.04 s, >= 20 ms from bin edges
    burst = signals.sine(fs, 0.010, -6.0, freq=3000.0, start=start)
    x[start:start + len(burst)] += burst

    ref, r = analyze_array(bed, fs), analyze_array(x, fs)
    inside = np.zeros(len(r.short_term), bool)
    inside[n0:n0 + 30] = True

    for name in ("short_term", "true_peak_3s"):
        a, b = getattr(r, name), getattr(ref, name)
        valid = np.isfinite(a) & np.isfinite(b)
        assert np.all(a[inside] - b[inside] > 3.0), name
        assert np.all(np.abs(a - b)[~inside & valid] <= 1e-6), name
        assert np.array_equal(np.isfinite(a)[~inside], np.isfinite(b)[~inside]), name

    # brute-force reference for the 30-bin causal max window
    p = np.where(r.bin_peak > 0, r.bin_peak, 0.0)
    brute = np.array([p[max(0, n - 29):n + 1].max() for n in range(len(p))])
    with np.errstate(divide="ignore"):
        np.testing.assert_array_equal(r.true_peak_3s, 20 * np.log10(brute))


# --- ADA-CHUNK ----------------------------------------------------------------

def test_chunk_invariance():
    fs = 48000
    x = signals.pink_noise(fs, int(60.0437 * fs))
    whole = analyze_array(x, fs)

    rng = np.random.default_rng(7)
    sizes = rng.integers(1, 50_000, size=2000)
    bounds = np.cumsum(sizes)
    bounds = bounds[bounds < len(x)]

    runs = [analyze_array(x, fs, int(s * fs)) for s in (1, 5, 7.3)]
    from ada_core.engine import Analyzer
    a = Analyzer(fs, 2)
    for chunk in np.split(x, bounds):
        a.process(chunk)
    runs.append(a.finish())

    arrays = ("momentary", "short_term", "true_peak_3s", "psr", "momentary_10ms",
              "short_term_10ms", "bin_peak")
    for r in runs:
        for name in arrays:
            np.testing.assert_allclose(getattr(r, name), getattr(whole, name),
                                       rtol=0, atol=1e-9, equal_nan=True, err_msg=name)
        for k, v in whole.summary().items():
            w = r.summary()[k]
            if isinstance(v, float):
                assert w == pytest.approx(v, abs=1e-9), k
            else:
                assert w == v, k


# --- ADA-TAIL -----------------------------------------------------------------

def test_final_partial_bin_counts_toward_true_peak():
    fs = 48000
    x = np.zeros((int(10.05 * fs), 2))
    start = int(10.02 * fs)
    burst = signals.peak_test_sine(fs, 4, math.sqrt(2), 45.0, seconds=0.02)
    x[start:start + len(burst)] = burst
    r = analyze_array(x, fs)
    assert len(r.short_term) == 100
    assert np.all(r.bin_peak == 0.0)
    assert tp_ok(r.true_peak_max, 3.01), r.true_peak_max


# --- ADA-WARMUP ---------------------------------------------------------------

def _finite_or_none(summary: dict) -> bool:
    json.dumps(summary, allow_nan=False)  # raises on NaN / inf
    return True


def test_short_file_has_no_psr_stats():
    fs = 48000
    r = analyze_array(signals.sine(fs, 2, -20.0), fs)
    s = r.summary()
    assert s["psr_min"] is None and s["psr_mean"] is None and s["psr_p10"] is None
    assert s["lra"] is None
    assert s["integrated_lufs"] is not None
    assert _finite_or_none(s)


def test_silence_is_excluded_from_psr_stats():
    fs = 48000
    x = np.concatenate([signals.sine(fs, 10, -20.0), np.zeros((10 * fs, 2)),
                        signals.sine(fs, 10, -20.0)])
    r = analyze_array(x, fs)
    s = r.summary()
    assert _finite_or_none(s)
    # bins whose 3 s window is entirely inside the silence must be gated out
    silent_windows = np.arange(len(r.short_term))
    silent_windows = (silent_windows >= 129) & (silent_windows <= 199)
    assert np.all(np.isnan(r.psr[silent_windows]))
    # stereo 1 kHz sine at -20 dBFS: TP = -20 dBTP, S = -20 LUFS, so PSR = 0 dB in steady state;
    # windows straddling the silence are quieter with the same peak, so PSR there is higher
    assert s["psr_min"] == pytest.approx(0.0, abs=0.1)
    assert s["psr_mean"] > s["psr_min"]


# --- ADA-CEIL -----------------------------------------------------------------

def test_peak_near_ceiling_steady_tone():
    fs = 48000
    r = analyze_array(signals.sine(fs, 20, -20.0), fs)
    assert r.summary()["peak_near_ceiling_pct"] == pytest.approx(100.0)


def test_peak_near_ceiling_two_levels():
    fs = 48000
    r = analyze_array(signals.tone_sequence(fs, [(20, -20.0), (20, -30.0)]), fs)
    # gated bins 29..399 (371); loud bins 29..199 (171) have peaks at the ceiling
    assert r.summary()["peak_near_ceiling_pct"] == pytest.approx(171 / 371 * 100, abs=0.3)


def test_peak_near_ceiling_undefined_without_gated_bins():
    fs = 48000
    r = analyze_array(signals.sine(fs, 2, -20.0), fs)
    assert r.summary()["peak_near_ceiling_pct"] is None
