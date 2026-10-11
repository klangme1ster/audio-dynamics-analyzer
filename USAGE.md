# Using ADA

## Installation

Requires Python ≥ 3.11. Run from the repository root:

```bash
python -m venv .venv
# Windows (PowerShell): .venv\Scripts\Activate.ps1
# macOS / Linux:        source .venv/bin/activate
pip install -e "python[dev]"
```

This installs two commands, `ada-scan` and `ada-plot`, and the Python package `ada_core`.

**Supported input:**
* Formats: WAV, FLAC, AIFF, OGG and MP3 (through libsndfile).
* Channels: mono or stereo.
* Sample rates: any multiple of 100 Hz, such as 32, 44.1, 48, 88.2, 96, 176.4 or 192 kHz.

Other channel counts or sample rates are reported as errors and skipped.

---

## `ada-scan`: batch scanner

```bash
ada-scan PATH [PATH ...] [options]
```

`PATH` can be any mix of files and folders. Folders are searched recursively.

| Option | Meaning |
|---|---|
| `-j N`, `--jobs N` | Number of parallel worker processes (default: all CPU cores) |
| `--export-csv FILE` | Write a CSV report, one row per file |
| `--export-json FILE` | Write a JSON report (a list of objects) |
| `--plot DIR` | Save a dynamics plot per file into `DIR` |
| `--plot-format png\|pdf\|svg` | Plot format (default `png`) |

**Examples**

```bash
# Whole library, all cores, CSV for a spreadsheet
ada-scan ~/Music --export-csv library.csv

# One album with plots as PDF
ada-scan "Albums/Some Album" --plot plots --plot-format pdf

# A few files, 4 workers, JSON for scripting
ada-scan a.wav b.flac c.mp3 -j 4 --export-json report.json
```

**Terminal table**
* The PSR columns are coloured by zone: green, yellow, orange or red.
* `TP max` is yellow above −1 dBTP and red above 0 dBTP.
* The last column shows the zone of the 10th-percentile PSR, which describes the most compressed parts of the track.
* Files that can't be read appear as `error`, with the reason listed below the table. They don't stop the scan.

**Exit code:** `0` on success. `1` if no audio was found, or if every file failed.

---

## `ada-plot`: single-file plots

```bash
ada-plot track.flac other.wav -o plots --format png
```

Each plot has four parts:

* **Loudness and peak:** short-term loudness (LUFS) and the 3-second true peak (dBTP) on a shared dB axis.
* **Peak-to-short-term ratio:** PSR over time, drawn over the labelled zone bands. Gaps are periods excluded from the statistics, such as the first 3 seconds and silence.
* **Distribution:** the percentage of the track's measured duration in each 1 dB PSR band.
* **Summary:** integrated loudness, LRA, maximum true peak, PLR, and PSR minimum, 10th percentile and mean.

---

## Report fields

Fields appear in CSV and JSON. A missing value is empty in CSV and `null` in JSON. That happens, for example, when a file is shorter than 3 s or is silent.

| Field | Unit | Meaning |
|---|---|---|
| `file` | — | Absolute path |
| `duration_s`, `sample_rate`, `channels` | s, Hz, — | File properties |
| `oversampling` | × | True-peak oversampling used: 4, 2, or 1 (above 96 kHz) |
| `integrated_lufs` | LUFS | Integrated loudness, BS.1770 gated |
| `lra` | LU | Loudness range, EBU Tech 3342 |
| `true_peak_max_dbtp` | dBTP | Highest inter-sample peak |
| `plr` | dB | Peak-to-loudness ratio: `true_peak_max − integrated` |
| `max_momentary_lufs` | LUFS | Loudest 400 ms window |
| `max_short_term_lufs` | LUFS | Loudest 3 s window |
| `psr_min`, `psr_mean`, `psr_p10` | dB | PSR statistics over the measured duration; `psr_p10` is the 10th percentile |
| `psr_green_pct` … `psr_red_pct` | % | Share of the measured duration in each zone |
| `plot` | — | Plot path, if `--plot` was given |
| `error` | — | Reason the file could not be analysed (only on failed rows) |

### Reading the results

* **PSR** describes the music moment to moment. Limiting is audible as a low PSR: below 8 dB for long stretches means the transients have been flattened.
* **PLR** is the track-level equivalent: peak level compared with average loudness. Modern loud masters often sit at 8–11 dB, and dynamic recordings at 14 dB or more.
* **PSR P10** is usually more informative than the minimum. It describes the most compressed 10 % of the track without being skewed by a single moment.
* **Streaming targets:** Spotify and YouTube normalise to about −14 LUFS, and Apple Music to about −16 LUFS, typically with a −1 dBTP true-peak ceiling. A track much louder than its target is turned down on playback, so whatever was given up in PSR to make it loud buys nothing.

---

## Python API

```python
from ada_core import analyze_file, Analyzer

r = analyze_file("track.flac")
print(r.integrated, r.lra, r.true_peak_max, r.plr)
print(r.summary())            # JSON-safe dict with the report fields

r.times                       # bin end times in seconds (10 per second)
r.short_term, r.true_peak_3s  # LUFS / dBTP per 100 ms
r.psr                         # dB per 100 ms; NaN where excluded

# Streaming from your own source (blocks of any length, shape (frames, channels))
a = Analyzer(48000, 2)
for block in blocks:
    a.process(block)
result = a.finish()
```

To get the plot as a Matplotlib figure, use `ada_core.visualizer.plot_result(result, title)`.

---

## Running the tests

```bash
pytest python/tests            # fast suite, about 10 s
pytest python/tests -m slow -s # 60-minute memory and throughput tests, about 80 s
```

Some EBU compliance cases need the official EBU loudness test-set audio files, which can't be redistributed. Download the zip from <https://tech.ebu.ch/publications/ebu_loudness_test_set>, then run:

```bash
python python/tools/fetch_ebu_testset.py path/to/ebu-loudness-test-set.zip
pytest python/tests            # the previously skipped file-based cases now run
```
