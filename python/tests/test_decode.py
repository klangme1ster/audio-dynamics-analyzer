"""M4A / AAC decoding (ADA-M4A): lossless ALAC must match WAV exactly, AAC must be accurate."""

from __future__ import annotations

import av
import numpy as np
import pytest
import soundfile as sf

from ada_core.engine import analyze_file
from ada_core.scanner import find_audio
import signals

FS = 48000


def write_encoded(path, x: np.ndarray, fs: int, codec: str, sample_format: str,
                  options: dict[str, str] | None = None) -> None:
    """Encode a float (frames, channels) signal with PyAV (ALAC: s16p, AAC: fltp).

    options are encoder options, e.g. {"aac_pns": "0"} (FFmpeg's AAC encoder uses perceptual
    noise substitution by default: the decoder generates that noise, so peaks depend on it).
    """
    layout = "stereo" if x.shape[1] == 2 else "mono"
    planar = np.ascontiguousarray(x.T)
    if sample_format == "s16p":
        planar = np.round(planar * 32767).astype(np.int16)
    else:
        planar = planar.astype(np.float32)
    with av.open(str(path), "w") as out:
        stream = out.add_stream(codec, rate=fs, layout=layout, options=options or {})
        if codec == "aac":
            stream.bit_rate = 256_000
        step = 1024 if codec == "aac" else 4096
        for i in range(0, planar.shape[1], step):
            frame = av.AudioFrame.from_ndarray(planar[:, i:i + step], format=sample_format,
                                               layout=layout)
            frame.sample_rate = fs
            frame.pts = i
            out.mux(stream.encode(frame))
        out.mux(stream.encode(None))


def test_alac_matches_wav_exactly(tmp_path):
    x = signals.pink_noise(FS, 10 * FS, level_db=-18.0) + signals.sine(FS, 10, -30.0)
    pcm = np.round(np.clip(x, -1, 1) * 32767).astype(np.int16)
    wav, m4a = tmp_path / "ref.wav", tmp_path / "ref.m4a"
    # both files must hold the same integers: libsndfile's own float->int16 conversion
    # rounds differently from ours, so the WAV is written from the int16 array directly
    sf.write(wav, pcm, FS, subtype="PCM_16")
    write_encoded(m4a, pcm / 32767, FS, "alac", "s16p")

    a, b = analyze_file(wav), analyze_file(m4a)
    assert b.frames == a.frames
    for name in ("short_term", "momentary", "bin_peak", "psr", "short_term_10ms"):
        np.testing.assert_allclose(getattr(b, name), getattr(a, name), rtol=0, atol=1e-9,
                                   equal_nan=True, err_msg=name)
    assert b.summary() == pytest.approx(a.summary())


@pytest.mark.parametrize("channels", [1, 2])
def test_aac_loudness_and_duration(tmp_path, channels):
    x = signals.sine(FS, 20, -23.0, channels=channels)
    path = tmp_path / "tone.m4a"
    write_encoded(path, x, FS, "aac", "fltp")
    r = analyze_file(path)
    assert r.channels == channels and r.sample_rate == FS
    assert abs(r.duration - 20.0) < 0.03, r.duration      # encoder priming removed
    expected = -23.0 if channels == 2 else -26.0
    assert r.integrated == pytest.approx(expected, abs=0.1)


def test_scanner_finds_m4a(tmp_path):
    (tmp_path / "a.m4a").write_bytes(b"")
    (tmp_path / "b.AAC").write_bytes(b"")
    (tmp_path / "c.mp4").write_bytes(b"")
    assert sorted(p.name for p in find_audio([tmp_path])) == ["a.m4a", "b.AAC"]
