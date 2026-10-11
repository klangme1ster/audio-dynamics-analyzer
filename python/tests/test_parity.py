"""ADA-PARITY: the C++ engine (via build/.../ada_cli) must match this Python reference.

Skipped when ada_cli has not been built (cmake --build build --config Release).
Set ADA_CLI to point at a specific executable.
"""

from __future__ import annotations

import json
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

from conftest import analyze_array
import signals

REPO = Path(__file__).resolve().parents[2]


def find_cli() -> Path | None:
    if os.environ.get("ADA_CLI"):
        return Path(os.environ["ADA_CLI"])
    names = ("ada_cli.exe", "ada_cli")
    found = [p for p in (REPO / "build").rglob("ada_cli*") if p.name in names and p.is_file()]
    return max(found, key=lambda p: p.stat().st_mtime) if found else None


CLI = find_cli()
needs_cli = pytest.mark.skipif(CLI is None, reason="ada_cli not built (cmake --build build --config Release)")


def run_cpp(x: np.ndarray, fs: int, tmp_path: Path, block: str) -> dict:
    raw = tmp_path / "signal.f32"
    np.ascontiguousarray(x, dtype=np.float32).tofile(raw)
    out = subprocess.run([str(CLI), str(raw), str(fs), str(x.shape[1]), block],
                         capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def to_db(a) -> np.ndarray:
    return np.asarray(a, dtype=float)  # ada_cli prints NaN / Infinity / -Infinity explicitly


def assert_close_db(cpp, py, tol, name):
    cpp, py = np.asarray(cpp, float), np.asarray(py, float)
    assert cpp.shape == py.shape, (name, cpp.shape, py.shape)
    both = np.isfinite(cpp) & np.isfinite(py)
    for kind in (np.isnan, np.isposinf, np.isneginf):
        assert np.array_equal(kind(cpp), kind(py)), f"{name}: {kind.__name__} pattern differs"
    if both.any():
        worst = float(np.max(np.abs(cpp[both] - py[both])))
        assert worst <= tol, f"{name}: max difference {worst:.4g} > {tol}"


def sub_grid_peaks(sub: np.ndarray, valid_from: int, n_bins: int) -> np.ndarray:
    """Per bin, max of the 10 ms read-outs ending inside it (as the C++ frame fields)."""
    s = np.array(sub, float)
    s[:valid_from] = -np.inf
    return s[:n_bins * 10].reshape(n_bins, 10).max(axis=1)


CASES = {
    "t3341-1": (lambda: signals.t3341_case(1), 48000),
    "t3341-3": (lambda: signals.t3341_case(3), 48000),
    "t3341-4": (lambda: signals.t3341_case(4), 48000),
    "t3341-5": (lambda: signals.t3341_case(5), 48000),
    "t3341-9": (lambda: signals.t3341_case(9), 48000),
    "t3341-12": (lambda: signals.t3341_case(12), 48000),
    "t3342-4": (lambda: signals.t3342_case(4), 48000),
    "tp-16-44k": (lambda: signals.peak_test_sine(44100, 4, 0.5, 45.0, fade_s=0.05), 44100),
    "tp-19-48k": (lambda: signals.peak_test_sine(48000, 4, 1.41, 45.0, fade_s=0.05), 48000),
    "isp-88k": (lambda: signals.peak_test_sine(88200, 4, math.sqrt(2), 45.0, seconds=8, fade_s=0.05), 88200),
    "isp-192k": (lambda: signals.peak_test_sine(192000, 4, math.sqrt(2), 45.0, seconds=8, fade_s=0.05), 192000),
    "mono": (lambda: signals.sine(48000, 20, -23.0, channels=1), 48000),
    "pink-noise": (lambda: signals.pink_noise(48000, int(40.0437 * 48000)), 48000),
    "silence-gap": (lambda: np.concatenate([signals.sine(48000, 10, -20.0), np.zeros((480000, 2)),
                                            signals.sine(48000, 10, -20.0)]), 48000),
    "tail": (lambda: np.concatenate([signals.pink_noise(44100, 10 * 44100),
                                     signals.peak_test_sine(44100, 4, 1.0, 45.0, seconds=0.03)]), 44100),
    # window with K-filter tail energy but no peak (TP 3 s = -inf): never a measured bin
    "filter-tail-no-peak": (lambda: filter_tail_signal(), 48000),
    # a NaN and an inf in the input must not stop the metering
    "non-finite-input": (lambda: non_finite_signal(), 48000),
}


def filter_tail_signal() -> np.ndarray:
    fs, H = 48000, 4800
    x = np.zeros((30 * H - 6 + 10 * fs, 1))
    n = np.arange(int(0.1 * fs))
    start = 30 * H - 6 - len(n)
    x[start:start + len(n), 0] = 0.9 * np.sign(np.sin(2 * np.pi * 10 * n / fs))
    return x


def non_finite_signal() -> np.ndarray:
    x = signals.sine(48000, 20, -20.0)
    x[int(5.0026 * 48000), 0] = np.nan
    x[int(9.99999 * 48000), 1] = np.inf
    return x


@needs_cli
@pytest.mark.parametrize("block", ["4096", "random"])
@pytest.mark.parametrize("case", list(CASES))
def test_cpp_matches_python(case, block, tmp_path):
    make, fs = CASES[case]
    x = make().astype(np.float32).astype(np.float64)  # both engines see identical float32 input
    py = analyze_array(x, fs)
    cpp = run_cpp(x, fs, tmp_path, block)

    n = len(py.short_term)
    assert cpp["frames"] == py.frames
    assert cpp["oversampling"] == py.oversampling
    assert_close_db(to_db(cpp["momentary"]), py.momentary, 0.01, "momentary")
    assert_close_db(to_db(cpp["short_term"]), py.short_term, 0.01, "short_term")
    assert_close_db(to_db(cpp["true_peak_3s"]), py.true_peak_3s, 0.01, "true_peak_3s")
    with np.errstate(divide="ignore"):
        assert_close_db(to_db(cpp["bin_peak_dbtp"]), 20 * np.log10(py.bin_peak), 0.01, "bin_peak")
    assert_close_db(to_db(cpp["momentary_peak"]), sub_grid_peaks(py.momentary_10ms, 39, n), 0.01,
                    "momentary_peak")
    assert_close_db(to_db(cpp["short_term_peak"]), sub_grid_peaks(py.short_term_10ms, 299, n), 0.01,
                    "short_term_peak")

    # PSR: compare wherever Python's statistics gate lets a bin through
    gated = np.isfinite(py.psr)
    cpp_psr = np.asarray(cpp["psr"], float)
    if gated.any():
        assert np.max(np.abs(cpp_psr[gated] - py.psr[gated])) <= 0.01

    ps, cs = py.summary(), cpp["summary"]
    tolerances = {"integrated_lufs": 0.02, "lra": 0.05, "true_peak_max_dbtp": 0.01, "plr": 0.03,
                  "max_momentary_lufs": 0.01, "max_short_term_lufs": 0.01, "psr_min": 0.01,
                  "psr_mean": 0.01, "psr_p10": 0.01, "peak_near_ceiling_pct": 0.5,
                  "psr_green_pct": 0.5, "psr_yellow_pct": 0.5, "psr_orange_pct": 0.5, "psr_red_pct": 0.5}
    for key, tol in tolerances.items():
        if ps[key] is None:
            assert cs[key] is None, key
        else:
            assert cs[key] is not None, key
            assert abs(cs[key] - ps[key]) <= tol, (key, cs[key], ps[key])


def test_generated_cpp_tables_are_current():
    tool = REPO / "python" / "tools" / "gen_cpp_tables.py"
    assert subprocess.run([sys.executable, str(tool), "--check"]).returncode == 0
