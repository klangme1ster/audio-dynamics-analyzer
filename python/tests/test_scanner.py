"""Batch scanner CLI: discovery, exports, plots and error handling (plan 1.6, 2.3)."""

from __future__ import annotations

import csv
import json

import soundfile as sf

from ada_core import scanner
import signals

FS = 48000


def test_scan_exports_and_plots(tmp_path):
    music = tmp_path / "music"
    (music / "sub").mkdir(parents=True)
    sf.write(music / "a.wav", signals.sine(FS, 5, -20.0), FS)
    sf.write(music / "sub" / "b.flac", signals.sine(FS, 5, -30.0, channels=1), FS)
    (music / "broken.wav").write_bytes(b"RIFFnot audio")
    (music / "notes.txt").write_text("ignored")

    out_json, out_csv, plots = tmp_path / "r.json", tmp_path / "r.csv", tmp_path / "plots"
    rc = scanner.main([str(music), "-j", "2", "--export-json", str(out_json),
                       "--export-csv", str(out_csv), "--plot", str(plots)])
    assert rc == 0

    rows = json.loads(out_json.read_text(encoding="utf-8"))
    assert len(rows) == 3
    by_name = {r["file"].replace("\\", "/").rsplit("/", 1)[-1]: r for r in rows}
    assert "error" in by_name["broken.wav"]
    assert abs(by_name["a.wav"]["integrated_lufs"] - (-20.0)) < 0.1
    assert abs(by_name["b.flac"]["integrated_lufs"] - (-33.0)) < 0.1
    assert by_name["a.wav"]["psr_min"] is not None

    with open(out_csv, newline="", encoding="utf-8") as f:
        assert len(list(csv.DictReader(f))) == 3
    assert sorted(p.name for p in plots.iterdir()) == ["a.png", "b.png"]


def test_scan_without_audio_fails(tmp_path):
    assert scanner.main([str(tmp_path)]) == 1
