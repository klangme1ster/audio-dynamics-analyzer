"""Spec §2.2 surround tests (ADA-SURR-W, ADA-LFE, ADA-SURR-TP, ADA-NOFOLD, ADA-LAYOUT,
ADA-SURR-CHUNK) and §3.11 channel layouts.

Written before the implementation (plan Phase 5): until ada_core.layout exists, this file
fails at import.
"""

from __future__ import annotations

import math
import struct

import numpy as np
import pytest
import soundfile as sf

from ada_core.engine import Analyzer, analyze_file
from ada_core.layout import (ChannelLayout, get_layout, layout_from_mask, parse_layout,
                             resolve_layout, suggest_layout)
from conftest import analyze_array
import signals

FS = 48000
TOL_LU = 0.1
G_SURROUND_DB = 10 * math.log10(1.41)  # +1.49 dB

L51 = get_layout("5.1").roles
L71 = get_layout("7.1").roles
ARRAYS = ("momentary", "short_term", "true_peak_3s", "psr", "momentary_10ms",
          "short_term_10ms", "bin_peak")


def assert_same(a, b, atol=1e-9):
    for name in ARRAYS:
        np.testing.assert_allclose(getattr(a, name), getattr(b, name), rtol=0, atol=atol,
                                   equal_nan=True, err_msg=name)
    sb = b.summary()
    for k, v in a.summary().items():
        if k in ("layout", "channel_roles"):  # a permuted custom layout names itself differently
            continue
        if isinstance(v, float):
            assert v == pytest.approx(sb[k], abs=atol), k
        else:
            assert v == sb[k], k


# --- §3.11 layouts --------------------------------------------------------------

def test_standard_layouts():
    assert get_layout("mono").roles == ("C",)
    assert get_layout("stereo").roles == ("L", "R")
    assert get_layout("5.0").roles == ("L", "R", "C", "Ls", "Rs")
    assert get_layout("5.1").roles == ("L", "R", "C", "LFE", "Ls", "Rs")
    assert get_layout("7.1").roles == ("L", "R", "C", "LFE", "Lb", "Rb", "Ls", "Rs")
    for name, weights in (("mono", [1.0]), ("stereo", [1.0, 1.0]),
                          ("5.1", [1, 1, 1, 0, 1.41, 1.41]),
                          ("7.1", [1, 1, 1, 0, 1.41, 1.41, 1.41, 1.41])):
        lay = get_layout(name)
        assert lay.channels == len(weights)
        np.testing.assert_allclose(lay.weights, weights, rtol=0, atol=0)  # LFE weight 0 = not measured


# --- ADA-SURR-W -----------------------------------------------------------------

@pytest.mark.parametrize("layout_name", ["5.1", "7.1"])
def test_channel_weights(layout_name):
    layout = get_layout(layout_name)
    for role in layout.roles:
        x = signals.tone_on(layout.roles, role, FS)
        r = analyze_array(x, FS, layout=layout)
        if role == "LFE":
            assert r.integrated is None, "LFE alone must read as silence"
        elif role in ("L", "R", "C"):
            assert r.integrated == pytest.approx(-26.0, abs=TOL_LU), role
        else:
            assert r.integrated == pytest.approx(-26.0 + G_SURROUND_DB, abs=TOL_LU), role


# --- ADA-LFE --------------------------------------------------------------------

def test_lfe_is_excluded():
    layout = get_layout("5.1")
    frames = 20 * FS
    bed = signals.pink_noise(FS, frames, -30.0, channels=6)
    bed[:, layout.roles.index("LFE")] = 0.0
    loud = bed.copy()
    loud[:, layout.roles.index("LFE")] = signals.sine(FS, 20, 0.0, 50.0, channels=1)[:, 0]
    ref, r = analyze_array(bed, FS, layout=layout), analyze_array(loud, FS, layout=layout)
    assert_same(r, ref)
    assert r.true_peak_max == ref.true_peak_max


# --- ADA-SURR-TP ----------------------------------------------------------------

@pytest.mark.parametrize("role", L71)
def test_surround_true_peak(role):
    layout = get_layout("7.1")
    isp = signals.peak_test_sine(FS, 4, math.sqrt(2), 45.0, seconds=20, channels=1, fade_s=0.05)[:, 0]
    x = signals.multichannel(len(isp), layout.roles, {role: isp})
    r = analyze_array(x, FS, layout=layout)
    if role == "LFE":
        assert r.true_peak_max == -math.inf  # LFE ignored, everything else silent
    else:
        assert 3.01 - 0.4 <= r.true_peak_max <= 3.01 + 0.2, (role, r.true_peak_max)


# --- ADA-NOFOLD -----------------------------------------------------------------

def test_no_downmix_phase_independent():
    layout = get_layout("5.1")
    tone = signals.sine(FS, 20, -23.0, channels=1)[:, 0]
    in_phase = signals.multichannel(len(tone), layout.roles, {"L": tone, "R": tone})
    inverted = signals.multichannel(len(tone), layout.roles, {"L": tone, "R": -tone})
    a, b = analyze_array(in_phase, FS, layout=layout), analyze_array(inverted, FS, layout=layout)
    assert_same(a, b)
    one = analyze_array(signals.tone_on(layout.roles, "L", FS), FS, layout=layout)
    assert a.integrated == pytest.approx(one.integrated + 3.01, abs=0.05)


# --- ADA-SURR-CHUNK -------------------------------------------------------------

def test_chunk_invariance_5_1():
    layout = get_layout("5.1")
    x = signals.pink_noise(FS, int(60.0437 * FS), channels=6)
    whole = analyze_array(x, FS, layout=layout)
    rng = np.random.default_rng(7)
    bounds = np.cumsum(rng.integers(1, 50_000, size=2000))
    bounds = bounds[bounds < len(x)]
    runs = [analyze_array(x, FS, int(s * FS), layout=layout) for s in (1, 5, 7.3)]
    a = Analyzer(FS, 6, layout)
    for chunk in np.split(x, bounds):
        a.process(chunk)
    runs.append(a.finish())
    for r in runs:
        assert_same(r, whole)


# --- ADA-LAYOUT -----------------------------------------------------------------

def write_wavex(path, x, mask):
    """Write 16-bit WAVE_FORMAT_EXTENSIBLE with an explicit dwChannelMask."""
    ch = x.shape[1]
    pcm = np.clip(np.round(x * 32768), -32768, 32767).astype("<i2").tobytes()
    guid = struct.pack("<IHH8s", 1, 0, 0x10, bytes.fromhex("800000aa00389b71"))
    fmt = struct.pack("<HHIIHHHHI", 0xFFFE, ch, FS, FS * ch * 2, ch * 2, 16, 22, 16, mask) + guid
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


@pytest.mark.parametrize("mask,name", [(0x3F, "5.1"), (0x60F, "5.1"), (0x63F, "7.1")])
def test_layout_from_wav_mask(tmp_path, mask, name):
    layout = get_layout(name)
    assert layout_from_mask(mask).name == name
    x = signals.pink_noise(FS, 2 * FS, -20.0, channels=layout.channels)
    x = np.round(x * 32768) / 32768  # exactly representable in the 16-bit file
    f = tmp_path / "m.wav"
    write_wavex(f, x, mask)
    r = analyze_file(f)  # no layout argument: the mask decides
    assert (r.channels, r.layout) == (layout.channels, name)
    assert_same(r, analyze_array(x.astype(np.float32), FS, layout=layout), atol=1e-6)


@pytest.mark.parametrize("channels,suggested", [(5, "5.0"), (6, "5.1"), (8, "7.1")])
def test_multichannel_without_mask_is_rejected_with_suggestion(tmp_path, channels, suggested):
    sug = suggest_layout(channels)
    assert sug is not None and sug.name == suggested
    with pytest.raises(ValueError, match=suggested):
        Analyzer(FS, channels)
    with pytest.raises(ValueError, match=suggested):
        resolve_layout(channels)
    f = tmp_path / "plain.wav"
    sf.write(f, 0.1 * signals.pink_noise(FS, FS, channels=channels), FS, subtype="PCM_16")
    with pytest.raises(ValueError, match=suggested):
        analyze_file(f)
    # the suggestion becomes valid once the user passes or confirms it
    assert analyze_file(f, layout=suggested).layout == suggested


@pytest.mark.parametrize("channels", [3, 4, 7])
def test_unsupported_channel_counts_have_no_suggestion(channels):
    assert suggest_layout(channels) is None
    with pytest.raises(ValueError, match="layout"):
        Analyzer(FS, channels)


def test_user_supplied_role_list_in_permuted_order():
    file_order = get_layout("5.1")
    x = signals.pink_noise(FS, 20 * FS, -25.0, channels=6)
    perm = (4, 0, 5, 2, 3, 1)  # column j of y is column perm[j] of x
    y = x[:, perm]
    roles = tuple(file_order.roles[i] for i in perm)
    custom = parse_layout(",".join(roles))
    assert isinstance(custom, ChannelLayout) and custom.roles == roles
    assert_same(analyze_array(y, FS, layout=custom), analyze_array(x, FS, layout=file_order))
    assert parse_layout("L,R,C,LFE,Ls,Rs").name == "5.1"  # standard order keeps its name


@pytest.mark.parametrize("bad", ["L,R,C", "L,L,C,LFE,Ls,Rs", "LFE", "L,R,X"])
def test_invalid_layouts_are_rejected(bad):
    with pytest.raises(ValueError):
        layout = parse_layout(bad)
        Analyzer(FS, 6, layout)


def test_role_count_must_match_channels():
    with pytest.raises(ValueError, match="channel"):
        Analyzer(FS, 5, get_layout("5.1"))


def test_mono_and_stereo_need_no_layout():
    assert Analyzer(FS, 1).channels == 1
    assert Analyzer(FS, 2).channels == 2
    r = analyze_array(signals.sine(FS, 20, -23.0, channels=1), FS)
    assert r.integrated == pytest.approx(-26.0, abs=TOL_LU) and r.layout == "mono"
    assert analyze_array(signals.sine(FS, 20, -23.0), FS).layout == "stereo"


def test_report_carries_layout():
    layout = get_layout("5.1")
    s = analyze_array(signals.tone_on(layout.roles, "C", FS), FS, layout=layout).summary()
    assert s["layout"] == "5.1" and s["channel_roles"] == "L,R,C,LFE,Ls,Rs"
