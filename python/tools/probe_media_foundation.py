"""Check how this machine's Media Foundation reads AAC, for the release workflow (Windows).

Encodes a short AAC file (FFmpeg's encoder writes 1024 priming samples and an edit list), reads
it with ada_file_cli (the plugin's reader) and with FFmpeg (the Python reference), and reports
whether the file opens and how far the two decodes are apart in samples. On GitHub Actions the
findings are printed as annotations, so they show on the run page without signing in, and
`available=true|false` is written to GITHUB_OUTPUT.

    python python/tools/probe_media_foundation.py
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / "python" / "tests"), str(REPO / "python")]

from ada_core.decode import open_audio  # noqa: E402
from test_decode import write_encoded  # noqa: E402
from test_parity import FILE_CLI, song  # noqa: E402

ON_ACTIONS = bool(os.environ.get("GITHUB_ACTIONS"))


def report(level: str, text: str) -> None:
    print(f"::{level}::{text}" if ON_ACTIONS else f"{level}: {text}")


def offset_samples(reference: np.ndarray, other: np.ndarray, max_lag: int = 4096) -> int:
    """Lag of `other` against `reference` (positive: `other` is late), first channel."""
    a, b = reference[:, 0].astype(float), other[:, 0].astype(float)
    start, length = max_lag, min(len(a), len(b)) - 2 * max_lag
    seg = a[start:start + length]
    return max(range(-max_lag, max_lag + 1), key=lambda lag: float(np.dot(seg, b[start + lag:start + lag + length])))


def main() -> int:
    work = pathlib.Path(tempfile.mkdtemp())
    path, dump = work / "probe.m4a", work / "probe.f32"
    write_encoded(path, song(48000, 2, 3), 48000, "aac", "fltp", {"aac_pns": "0"})
    ok, message, lag = False, "", None
    try:
        r = subprocess.run([str(FILE_CLI), str(path), "--dump", str(dump)], capture_output=True, text=True,
                           timeout=120, env={**os.environ, "ADA_MF_DEBUG": "1"})
        ok, message = r.returncode == 0, " / ".join(r.stderr.strip().splitlines())
    except (OSError, subprocess.TimeoutExpired) as e:  # CLI missing or hanging
        message = repr(e)
    if ok:
        _, blocks = open_audio(path)
        reference = np.concatenate(list(blocks))
        decoded = np.fromfile(dump, dtype=np.float32).reshape(-1, reference.shape[1])
        lag = offset_samples(reference, decoded)
        report("notice", f"Media Foundation reads AAC; offset to FFmpeg {lag} samples "
                         f"({len(decoded)} vs {len(reference)} samples). Reader: {message}")
    else:
        report("warning", f"Media Foundation cannot read AAC on this machine, M4A tests skipped: {message}")
    if "GITHUB_OUTPUT" in os.environ:
        with open(os.environ["GITHUB_OUTPUT"], "a") as out:
            out.write(f"available={'true' if ok else 'false'}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
