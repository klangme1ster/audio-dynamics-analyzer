from __future__ import annotations

import re
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent))

from ada_core.engine import Analyzer, Result  # noqa: E402

EBU_DIR = Path(__file__).parent / "fixtures" / "ebu"


def analyze_array(x: np.ndarray, fs: int, chunk_frames: int | None = None,
                  layout=None) -> Result:
    """Run the engine on an in-memory signal, optionally in fixed-size chunks.

    layout: a ChannelLayout (or None for mono / stereo), spec §3.11.
    """
    a = Analyzer(fs, x.shape[1], layout) if layout is not None else Analyzer(fs, x.shape[1])
    step = chunk_frames or len(x) or 1
    for i in range(0, len(x), step):
        a.process(x[i:i + step])
    return a.finish()


def ebu_files(case_regex: str) -> list[Path]:
    """EBU test-set WAVs whose name matches the regex; skip the test if none exist."""
    pattern = re.compile(case_regex)
    files = sorted(p for p in EBU_DIR.glob("*.wav") if pattern.search(p.name))
    if not files:
        pytest.skip(f"EBU test-set file matching {case_regex!r} not found in {EBU_DIR} "
                    "(run python/tools/fetch_ebu_testset.py)")
    return files


def t3341_regex(case: int) -> str:
    # Matches e.g. seq-3341-7_..., seq-3341-2011-8_..., seq-3341-10-1-24bit.wav
    return rf"3341-(?:20\d\d-)?{case}(?![0-9])"


def t3342_regex(case: int) -> str:
    return rf"3342-{case}(?![0-9])"
