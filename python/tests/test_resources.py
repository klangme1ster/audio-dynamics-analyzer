"""Spec §2.2 — ADA-MEM and ADA-PERF. Slow: run with `pytest python/tests -m slow`."""

from __future__ import annotations

import time
import tracemalloc

import numpy as np
import psutil
import pytest
import soundfile as sf

from ada_core.engine import analyze_file

FS = 48000
MINUTES = 60


@pytest.fixture(scope="module")
def long_wav(tmp_path_factory):
    path = tmp_path_factory.mktemp("long") / "sixty_minutes.wav"
    rng = np.random.default_rng(3)
    t_block = 10 * FS
    with sf.SoundFile(path, "w", FS, 2, subtype="PCM_16") as f:
        for i in range(MINUTES * 6):
            n = np.arange(i * t_block, (i + 1) * t_block)
            tone = 0.3 * np.sin(2 * np.pi * 440 * n / FS)
            f.write(np.stack([tone, tone], axis=1) + 0.05 * rng.standard_normal((t_block, 2)))
    return path


@pytest.mark.slow
def test_memory_bound(long_wav):
    proc = psutil.Process()
    rss_before = proc.memory_info().rss
    tracemalloc.start()
    r = analyze_file(long_wav)
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    rss_growth = proc.memory_info().rss - rss_before
    assert r.integrated is not None
    assert peak < 64 * 2**20, f"tracemalloc peak {peak / 2**20:.1f} MB"
    assert rss_growth < 128 * 2**20, f"RSS growth {rss_growth / 2**20:.1f} MB"


@pytest.mark.slow
def test_throughput(long_wav):
    t0 = time.perf_counter()
    analyze_file(long_wav)
    elapsed = time.perf_counter() - t0
    speed = MINUTES * 60 / elapsed
    print(f"\nADA-PERF: {speed:.0f}x real-time ({elapsed:.1f} s for {MINUTES} min)")
    assert speed >= 100
