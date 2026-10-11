"""Install the EBU loudness test set into python/tests/fixtures/ebu/.

The test set is distributed by the EBU (https://tech.ebu.ch/publications/ebu_loudness_test_set)
under its own terms, so it is not committed. Download the zip from that page, then run:

    python python/tools/fetch_ebu_testset.py path/to/ebu-loudness-test-set.zip

A direct URL to the zip also works in place of the path. Only .wav members are extracted
(flattened, no directories), and nothing is executed from the archive.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

FIXTURE_DIR = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "ebu"


def extract_wavs(zip_path: Path, dest: Path) -> list[Path]:
    dest.mkdir(parents=True, exist_ok=True)
    written = []
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            name = Path(info.filename).name
            if info.is_dir() or not name.lower().endswith(".wav"):
                continue
            target = dest / name
            with z.open(info) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
            written.append(target)
    return written


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("source", help="path or https URL of the EBU loudness test-set zip")
    args = p.parse_args(argv)

    if args.source.startswith(("http://", "https://")):
        with tempfile.TemporaryDirectory() as tmp:
            zip_path = Path(tmp) / "ebu.zip"
            urllib.request.urlretrieve(args.source, zip_path)
            files = extract_wavs(zip_path, FIXTURE_DIR)
    else:
        files = extract_wavs(Path(args.source), FIXTURE_DIR)

    print(f"Extracted {len(files)} WAV files to {FIXTURE_DIR}")
    return 0 if files else 1


if __name__ == "__main__":
    sys.exit(main())
