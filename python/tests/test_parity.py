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

from ada_core.layout import parse_layout
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


def run_cpp(x: np.ndarray, fs: int, tmp_path: Path, block: str, layout: str | None = None) -> dict:
    raw = tmp_path / "signal.f32"
    np.ascontiguousarray(x, dtype=np.float32).tofile(raw)
    cmd = [str(CLI), str(raw), str(fs), str(x.shape[1]), block] + ([layout] if layout else [])
    out = subprocess.run(cmd, capture_output=True, text=True, check=True)
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


def surround_noise(roles: str, lfe_level_db: float | None = None) -> np.ndarray:
    """Pink noise per channel (different seeds), a level step in the middle; optional loud LFE."""
    names = roles.split(",")
    parts = [signals.pink_noise(48000, int(30.0437 * 48000), -26.0 + 8.0 * (i % 3), channels=1, seed=10 + i)
             for i in range(len(names))]
    x = np.hstack(parts)
    x[len(x) // 2:] *= 0.4
    if lfe_level_db is not None and "LFE" in names:
        x[:, names.index("LFE")] = signals.sine(48000, 30.0437, lfe_level_db, 50.0, channels=1)[:len(x), 0]
    return x


def surround_isp() -> np.ndarray:
    """Inter-sample peaks on Ls (7.1), a quiet bed on the other channels."""
    names = "L,R,C,LFE,Lb,Rb,Ls,Rs".split(",")
    isp = signals.peak_test_sine(48000, 4, math.sqrt(2), 45.0, seconds=8, channels=1, fade_s=0.05)[:, 0]
    x = 0.01 * signals.pink_noise(48000, len(isp), channels=len(names), seed=5)
    x[:, names.index("Ls")] = isp
    return x


# Surround cases carry a layout (spec 3.11): a name or a role list in file order
LAYOUTS = {
    "surround-5.1-noise": "5.1",
    "surround-5.1-lfe-active": "5.1",
    "surround-7.1-noise": "7.1",
    "surround-7.1-isp": "7.1",
    "surround-5.0-noise": "5.0",
    "surround-custom-order": "Ls,L,Rs,C,LFE,R",
}
CASES |= {
    "surround-5.1-noise": (lambda: surround_noise("L,R,C,LFE,Ls,Rs"), 48000),
    "surround-5.1-lfe-active": (lambda: surround_noise("L,R,C,LFE,Ls,Rs", lfe_level_db=0.0), 48000),
    "surround-7.1-noise": (lambda: surround_noise("L,R,C,LFE,Lb,Rb,Ls,Rs"), 48000),
    "surround-7.1-isp": (surround_isp, 48000),
    "surround-5.0-noise": (lambda: surround_noise("L,R,C,Ls,Rs"), 48000),
    "surround-custom-order": (lambda: surround_noise("Ls,L,Rs,C,LFE,R", lfe_level_db=-6.0), 48000),
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
    layout = LAYOUTS.get(case)
    py = analyze_array(x, fs, layout=parse_layout(layout) if layout else None)
    cpp = run_cpp(x, fs, tmp_path, block, layout)

    n = len(py.short_term)
    assert (cpp["layout"], cpp["channel_roles"]) == (py.layout, py.channel_roles)
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


# --- decoded files (ADA-PARITY, M4A): the plugin's file analysis vs the Python scanner ----------
# C++ decodes M4A with Windows Media Foundation, Python with FFmpeg (PyAV). Both drop the encoder
# priming given by the container and give identical samples (ALAC: lossless; AAC: the two decoders
# agree), so the standard tolerances apply. Exception: AAC files coded with PNS (perceptual noise
# substitution; FFmpeg's encoder uses it by default) let the decoder generate noise, so peaks
# differ between any two decoders; loudness still matches (test_m4a_pns_loudness_matches_python).

def find_file_cli() -> Path | None:
    if os.environ.get("ADA_FILE_CLI"):
        return Path(os.environ["ADA_FILE_CLI"])
    found = [p for p in (REPO / "build").rglob("ada_file_cli*")
             if p.name in ("ada_file_cli.exe", "ada_file_cli") and p.is_file()]
    return max(found, key=lambda p: p.stat().st_mtime) if found else None


FILE_CLI = find_file_cli()
needs_file_cli = pytest.mark.skipif(FILE_CLI is None or sys.platform != "win32",
                                    reason="ada_file_cli not built (M4A reading needs Windows)")


def song(fs: int, channels: int, seconds: int = 15) -> np.ndarray:
    """Drum-like transients over noise, a quiet, a limited loud and a medium section."""
    t = np.arange(seconds * fs) / fs
    rng = np.random.default_rng(3)
    beat, snare = np.mod(t, 0.5), np.mod(t - 0.25, 0.5)
    x = (0.9 * np.exp(-beat / 0.03) * np.sin(2 * np.pi * 55 * beat)
         + 0.5 * np.exp(-snare / 0.05) * rng.standard_normal(len(t)) * (snare < 0.2))
    x = x[:, None] + 0.2 * signals.pink_noise(fs, len(t), level_db=-20.0, channels=channels)
    gain = np.repeat([0.3, 2.5, 0.8], -(-len(t) // 3))[:len(t), None]
    return np.tanh(x * gain * 1.5) / 1.5


def analyse_both(case: str, tmp_path: Path, codec: str, sample_format: str, fs: int, channels: int,
                 options: dict[str, str] | None = None, extension: str = ".m4a") -> tuple:
    from ada_core.engine import analyze_file
    from test_decode import write_encoded

    x = song(fs, channels)
    if sample_format == "s16p":
        x = np.round(np.clip(x, -1, 1) * 32767) / 32767
    path = tmp_path / f"{case}{extension}"
    write_encoded(path, x, fs, codec, sample_format, options)
    py = analyze_file(path)
    out = subprocess.run([str(FILE_CLI), str(path)], capture_output=True, text=True, check=True)
    assert "Media Foundation" in out.stderr  # read by the M4A reader, not another format
    cpp = json.loads(out.stdout)
    # the container's priming is dropped and the length is the same
    assert cpp["frames"] == py.frames
    assert cpp["sample_rate"] == fs and cpp["channels"] == channels
    assert (cpp["layout"], cpp["channel_roles"]) == (py.layout, py.channel_roles)
    return py, cpp


M4A_CASES = {  # name: (codec, sample format, fs, channels, encoder options[, file extension])
    "aac-44k-stereo": ("aac", "fltp", 44100, 2, {"aac_pns": "0"}),
    # raw ADTS AAC: no container length; Windows' estimate can be seconds short, ADA counts frames
    "adts-44k-stereo": ("aac", "fltp", 44100, 2, {"aac_pns": "0"}, ".aac"),
    "aac-48k-mono": ("aac", "fltp", 48000, 1, {"aac_pns": "0"}),
    "alac-48k-stereo": ("alac", "s16p", 48000, 2, None),
    # surround: the layout comes from the stream's channel configuration (Media Foundation's speaker mask);
    # PyAV cannot encode 8 channels, so 7.1 AAC has no case here
    "aac-48k-5.1": ("aac", "fltp", 48000, 6, {"aac_pns": "0"}),
}


@needs_file_cli
@pytest.mark.parametrize("case", list(M4A_CASES))
def test_m4a_file_analysis_matches_python(case, tmp_path):
    py, cpp = analyse_both(case, tmp_path, *M4A_CASES[case])
    n = len(py.short_term)
    assert_close_db(to_db(cpp["momentary"]), py.momentary, 0.01, "momentary")
    assert_close_db(to_db(cpp["short_term"]), py.short_term, 0.01, "short_term")
    assert_close_db(to_db(cpp["true_peak_3s"]), py.true_peak_3s, 0.01, "true_peak_3s")
    with np.errstate(divide="ignore"):
        # Windows' multichannel AAC decoder differs a little more from FFmpeg's in quiet bins
        # (0.07 dB on the CI runner); the 3 s true peak and the summary stay within 0.01 dB
        assert_close_db(to_db(cpp["bin_peak_dbtp"]), 20 * np.log10(py.bin_peak),
                        0.1 if case == "aac-48k-5.1" else 0.01, "bin_peak")
    assert_close_db(to_db(cpp["short_term_peak"]), sub_grid_peaks(py.short_term_10ms, 299, n), 0.01,
                    "short_term_peak")

    ps, cs = py.summary(), cpp["summary"]
    tolerances = {"integrated_lufs": 0.02, "lra": 0.05, "true_peak_max_dbtp": 0.01, "plr": 0.03,
                  "max_momentary_lufs": 0.01, "max_short_term_lufs": 0.01, "psr_min": 0.01,
                  "psr_mean": 0.01, "psr_p10": 0.01, "psr_green_pct": 0.5, "psr_yellow_pct": 0.5,
                  "psr_orange_pct": 0.5, "psr_red_pct": 0.5}
    for key, tol in tolerances.items():
        if ps[key] is None:
            assert cs[key] is None, key
        else:
            assert cs[key] is not None, key
            assert abs(cs[key] - ps[key]) <= tol, (key, cs[key], ps[key])


@needs_file_cli
def test_m4a_broken_edit_list_length_is_not_trusted(tmp_path):
    """An edit list claiming ~50 days must not turn into days of silence (MF duration wins)."""
    from test_decode import write_encoded

    path = tmp_path / "broken.m4a"
    write_encoded(path, song(48000, 2, 5), 48000, "aac", "fltp", {"aac_pns": "0"})
    data = bytearray(path.read_bytes())
    i = data.find(b"elst")
    assert i > 0 and data[i + 4] == 0  # version 0: entry = duration (u32), media time, rate
    data[i + 12:i + 16] = bytes([255, 255, 255, 255])
    path.write_bytes(bytes(data))
    out = subprocess.run([str(FILE_CLI), str(path)], capture_output=True, text=True, check=True,
                         timeout=120)  # an untrusted length would decode for hours
    frames = json.loads(out.stdout)["frames"]
    assert abs(frames - 5 * 48000) <= 48000 // 10 + 1024


@needs_file_cli
def test_m4a_pns_loudness_matches_python(tmp_path):
    """With PNS the decoders' noise differs: loudness matches, peaks are not compared."""
    py, cpp = analyse_both("aac-pns", tmp_path, "aac", "fltp", 48000, 2, {"aac_pns": "1"})
    assert_close_db(to_db(cpp["short_term"]), py.short_term, 0.05, "short_term")
    ps, cs = py.summary(), cpp["summary"]
    assert abs(cs["integrated_lufs"] - ps["integrated_lufs"]) <= 0.02
    assert abs(cs["lra"] - ps["lra"]) <= 0.05


# --- Surround files through the file reader (spec 3.11, ADA-PARITY) --------------------------------

SURROUND_FILES = {  # name: (layout name, WAV channel mask, layout argument for both engines or None)
    "wav-5.1-mask": ("5.1", 0x3F, None),
    "wav-7.1-mask": ("7.1", 0x63F, None),
    "wav-5.0-mask": ("5.0", 0x37, None),
    "wav-5.1-side-mask": ("5.1", 0x60F, None),
    "wav-6ch-no-mask-with-layout": ("5.1", 0, "5.1"),
    "wav-6ch-custom-order": ("custom", 0, "Ls,L,Rs,C,LFE,R"),
}


@needs_file_cli
@pytest.mark.parametrize("case", list(SURROUND_FILES))
def test_surround_wav_file_analysis_matches_python(case, tmp_path):
    from ada_core.engine import analyze_file
    from test_surround import write_wavex

    name, mask, layout = SURROUND_FILES[case]
    roles = layout if layout and "," in layout else {"5.1": "L,R,C,LFE,Ls,Rs", "7.1": "L,R,C,LFE,Lb,Rb,Ls,Rs",
                                                      "5.0": "L,R,C,Ls,Rs"}[name]
    x = surround_noise(roles, lfe_level_db=-6.0)
    x = np.round(np.clip(x, -1, 1) * 32768) / 32768
    path = tmp_path / f"{case}.wav"
    write_wavex(path, x, mask)
    py = analyze_file(path, layout=parse_layout(layout) if layout else None)
    cmd = [str(FILE_CLI), str(path)] + (["--layout", layout] if layout else [])
    cpp = json.loads(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout)
    assert (cpp["layout"], cpp["channel_roles"], cpp["channels"]) == (py.layout, py.channel_roles, py.channels)
    assert cpp["frames"] == py.frames
    assert_close_db(to_db(cpp["short_term"]), py.short_term, 0.01, "short_term")
    assert_close_db(to_db(cpp["true_peak_3s"]), py.true_peak_3s, 0.01, "true_peak_3s")
    ps, cs = py.summary(), cpp["summary"]
    assert abs(cs["integrated_lufs"] - ps["integrated_lufs"]) <= 0.02
    assert abs(cs["true_peak_max_dbtp"] - ps["true_peak_max_dbtp"]) <= 0.01


@needs_file_cli
def test_surround_wav_file_without_layout_is_refused(tmp_path):
    from test_surround import write_wavex

    path = tmp_path / "six.wav"
    write_wavex(path, 0.1 * surround_noise("L,R,C,LFE,Ls,Rs")[:48000], 0)
    out = subprocess.run([str(FILE_CLI), str(path)], capture_output=True, text=True)
    assert out.returncode != 0 and "5.1" in out.stderr


@needs_file_cli
def test_alac_surround_needs_a_layout_in_the_file_reader(tmp_path):
    """Media Foundation's ALAC decoder reports no speaker mask: the file reader asks for a layout."""
    from ada_core.engine import analyze_file
    from test_decode import write_encoded

    x = np.round(song(48000, 6, 5) * 32767) / 32767
    path = tmp_path / "alac51.m4a"
    write_encoded(path, x, 48000, "alac", "s16p")
    refused = subprocess.run([str(FILE_CLI), str(path)], capture_output=True, text=True)
    assert refused.returncode != 0 and "5.1" in refused.stderr
    out = subprocess.run([str(FILE_CLI), str(path), "--layout", "5.1"], capture_output=True, text=True, check=True)
    cpp, py = json.loads(out.stdout), analyze_file(path)
    assert (cpp["layout"], cpp["frames"]) == (py.layout, py.frames)
    assert abs(cpp["summary"]["integrated_lufs"] - py.summary()["integrated_lufs"]) <= 0.02
