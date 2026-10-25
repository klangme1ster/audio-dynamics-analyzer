"""Spec §2.1 — EBU Tech 3341 / 3342 compliance, plus ADA-MONO."""

from __future__ import annotations

import numpy as np
import pytest

from ada_core.engine import analyze_file
from conftest import analyze_array, ebu_files, t3341_regex, t3342_regex
import signals

FS = 48000
TOL_LU = 0.1


def tp_ok(measured: float, expected: float) -> bool:
    return expected - 0.4 <= measured <= expected + 0.2


# --- Tech 3341: M, S, I -----------------------------------------------------

@pytest.mark.parametrize("case,level", [(1, -23.0), (2, -33.0)])
def test_t3341_steady_tone(case, level):
    r = analyze_array(signals.t3341_case(case, FS), FS)
    assert r.integrated == pytest.approx(level, abs=TOL_LU)
    assert np.all(np.abs(r.momentary[3:] - level) <= TOL_LU)
    assert np.all(np.abs(r.short_term[29:] - level) <= TOL_LU)


@pytest.mark.parametrize("case", [3, 4, 5])
def test_t3341_gating(case):
    r = analyze_array(signals.t3341_case(case, FS), FS)
    assert r.integrated == pytest.approx(-23.0, abs=TOL_LU)


def test_t3341_9_short_term_constant():
    r = analyze_array(signals.t3341_case(9, FS), FS)
    s = r.short_term_10ms[299:]  # windows ending at t >= 3 s
    assert np.all(np.abs(s - (-23.0)) <= TOL_LU)
    assert r.max_short_term == pytest.approx(-23.0, abs=TOL_LU)


def test_t3341_12_momentary_constant():
    r = analyze_array(signals.t3341_case(12, FS), FS)
    m = r.momentary_10ms[99:]  # windows ending at t >= 1 s
    assert np.all(np.abs(m - (-23.0)) <= TOL_LU)
    assert r.max_momentary == pytest.approx(-23.0, abs=TOL_LU)


# --- Tech 3341: true peak (cases 15-19 synthesized) -------------------------

@pytest.mark.parametrize("fs", [44100, 48000])
@pytest.mark.parametrize("case", [15, 16, 17, 18, 19])
def test_t3341_true_peak(case, fs):
    div, amp, phase = signals.T3341_TRUE_PEAK[case]
    r = analyze_array(signals.peak_test_sine(fs, div, amp, phase, fade_s=0.05), fs)
    expected = 3.0 if case == 19 else -6.0
    assert tp_ok(r.true_peak_max, expected), r.true_peak_max


# --- Tech 3342: LRA ---------------------------------------------------------

@pytest.mark.parametrize("case,expected", [(1, 10.0), (2, 5.0), (3, 20.0), (4, 15.0)])
def test_t3342_lra(case, expected):
    r = analyze_array(signals.t3342_case(case, FS), FS)
    assert r.lra == pytest.approx(expected, abs=1.0)


# --- ADA-MONO ---------------------------------------------------------------

def test_ada_mono_not_dual_mono():
    x = signals.sine(FS, 20, -23.0, channels=1)
    r = analyze_array(x, FS)
    assert r.integrated == pytest.approx(-26.0, abs=TOL_LU)


# --- File-based EBU cases (skip when the test set is absent) ----------------

@pytest.mark.ebu_files
@pytest.mark.parametrize("case", [7, 8])
def test_t3341_file_integrated(case):
    for path in ebu_files(t3341_regex(case)):
        assert analyze_file(path).integrated == pytest.approx(-23.0, abs=TOL_LU), path.name


@pytest.mark.ebu_files
def test_t3341_10_max_short_term():
    for path in ebu_files(t3341_regex(10)):
        assert analyze_file(path).max_short_term == pytest.approx(-23.0, abs=TOL_LU), path.name


@pytest.mark.ebu_files
def test_t3341_13_max_momentary():
    for path in ebu_files(t3341_regex(13)):
        assert analyze_file(path).max_momentary == pytest.approx(-23.0, abs=TOL_LU), path.name


@pytest.mark.ebu_files
@pytest.mark.parametrize("case", [20, 21, 22, 23])
def test_t3341_file_true_peak(case):
    for path in ebu_files(t3341_regex(case)):
        tp = analyze_file(path).true_peak_max
        assert tp_ok(tp, 0.0), (path.name, tp)


@pytest.mark.ebu_files
@pytest.mark.parametrize("case,expected", [(5, 5.0), (6, 15.0)])
def test_t3342_file_lra(case, expected):
    for path in ebu_files(t3342_regex(case)):
        assert analyze_file(path).lra == pytest.approx(expected, abs=1.0), path.name


# --- Tech 3341 case 6: 5.0 / 5.1 (surround, spec §3.11) -----------------------

def test_t3341_6_five_channels_with_supplied_layout():
    f = ebu_files(r"3341-6-5channels")[0]
    with pytest.raises(ValueError, match="5.0"):  # no channel mask: rejected, 5.0 suggested
        analyze_file(f)
    r = analyze_file(f, layout="5.0")
    assert r.channels == 5
    assert r.integrated == pytest.approx(-23.0, abs=TOL_LU)
    assert r.max_momentary == pytest.approx(-23.0, abs=TOL_LU)
    assert r.max_short_term == pytest.approx(-23.0, abs=TOL_LU)


def test_t3341_6_wavex_layout_from_channel_mask():
    f = ebu_files(r"3341-6-6channels-WAVEEX")[0]
    r = analyze_file(f)  # mask 0x3F: 5.1, no layout argument needed
    assert (r.channels, r.layout) == (6, "5.1")
    assert r.integrated == pytest.approx(-23.0, abs=TOL_LU)
    # the silent LFE must not change anything relative to the 5.0 file
    r5 = analyze_file(ebu_files(r"3341-6-5channels")[0], layout="5.0")
    np.testing.assert_allclose(r.short_term, r5.short_term, rtol=0, atol=1e-9, equal_nan=True)
