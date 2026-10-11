# Audio Dynamics Analyzer (ADA) — Implementation Plan

Test IDs (T3341-*, T3342-*, ADA-*) refer to §2 of `PROJECT_SPECIFICATION.md`. Every algorithm step lists the tests it must turn green; those tests are written first.

## Phase 0: Repository Setup
- [x] **0.1 Version control**
  - Check `git rev-parse --show-toplevel`; run `git init` only if the folder is not yet a repository.
  - Root `.gitignore`: `.venv/`, `__pycache__/`, `*.egg-info/`, `.pytest_cache/`, `build/`, `cmake-build-*/`, `python/tests/fixtures/ebu/`, generated reports.
- [x] **0.2 Layout**
  - `python/` (package `ada_core`, `tests/`, `tools/`), `src/engine/` + `src/plugin/` (C++), `tests/cpp/`; libebur128 (test-only) and doctest are fetched by CMake.

---

## Phase 1: Python Reference Engine (`python/ada_core`)

- [x] **1.1 Packaging (`python/pyproject.toml`)**
  - Build backend: hatchling. Python ≥ 3.11.
  - Dependencies: `numpy>=1.24`, `scipy>=1.10`, `soundfile>=0.12`, `rich>=13.0`, `matplotlib>=3.7`. Dev extra: `pytest>=7.0`, `psutil` (RSS check in ADA-MEM).
  - `[tool.pytest.ini_options] testpaths = ["tests"]`; markers `ebu_files` and `benchmark`.
  - `pip install -e "python[dev]"`; run with `pytest python/tests`.
- [x] **1.2 Validation suite first (`python/tests/`)**
  - `signals.py`: synthesizers for every "Synth" row of spec §2.1 and every ADA-* signal in §2.2.
  - `test_compliance.py`: T3341-*, T3342-*, ADA-MONO (Live rows are Stage 2 only).
  - `test_internals.py`: ADA-KCOEF, ADA-FIR, ADA-ISP, ADA-RATE, ADA-ALIGN, ADA-CHUNK, ADA-TAIL, ADA-WARMUP.
  - `test_resources.py`: ADA-MEM (`tracemalloc` + `psutil` RSS), ADA-PERF (`@pytest.mark.benchmark`).
  - `tools/fetch_ebu_testset.py`: downloads the EBU loudness test set into `tests/fixtures/ebu/` on request (never during a test run). File-based tests skip with a clear message when absent.
  - Expected state at the end of this step: all tests fail or skip.
- [x] **1.3 K-weighting (`ada_core/kweight.py`)** → ADA-KCOEF
  - `kweight_sos(fs) -> ndarray (2, 6)` using spec §3.2 formulas.
  - `KWeighting(fs, channels)`: `scipy.signal.sosfilt(sos, x, axis=0, zi=self.zi)`, `zi` shape `(2, 2, channels)`, zeros at start/reset.
- [x] **1.4 True Peak (`ada_core/true_peak.py`)** → ADA-FIR, ADA-ISP, ADA-RATE, T3341-TP-*
  - Annex 2 table as a module constant (48 values, verbatim), phases per spec §3.3.
  - `TruePeakDetector(fs, channels)`: per active phase `scipy.signal.lfilter(h_p, [1.0], x, axis=0, zi=state_p)`, `zi` shape `(11, channels)`. No `resample_poly`/`upfirdn` (stateless → chunk-edge errors).
  - Attributes outputs to bins with delay $D$; keeps the running max of the open bin across calls; `flush()` feeds $D$ zeros and returns the trailing partial-bin peak separately.
- [x] **1.5 Grid engine (`ada_core/engine.py`)** → T3341-1…13, T3342-*, ADA-MONO, ADA-ALIGN, ADA-CHUNK, ADA-TAIL, ADA-WARMUP
  - `Analyzer(fs, channels).process(chunk)` accepts **any** chunk length; `.finish() -> Result`.
  - Rejects `fs % 100 != 0` and channels ∉ {1, 2}.
  - Input via `soundfile.blocks(path, blocksize=50*H, dtype="float32", always_2d=True)` (last block may be short — handled by the engine, no `fill_value`).
  - Per chunk: K-weight → square → `np.add.reduceat` at sub-bin boundaries, carrying the partial sub-bin sum to the next chunk → append sub-bin energies $e_i[u]$ (2 ch × 360 000 float64 per hour ≈ 5.8 MB).
  - `finish()`: bins from sub-bins; $M$, $S$ and the 10 ms sub-grid values via `np.convolve` with a ones-window (implicit zero padding; a cumulative-sum difference loses precision in quiet passages after loud ones); $\text{TP}_{3s}$ via `scipy.ndimage.maximum_filter1d(P, size=30, origin=14, mode="constant", cval=0.0)` (window $n-29..n$; ADA-ALIGN also asserts against a brute-force reference); gating for $I$, LRA, PSR statistics per spec §3.5–3.7.
  - `np.errstate(divide="ignore")` around log10; every report field is finite or `None`.
- [x] **1.6 Batch scanner CLI (`ada_core/scanner.py`, entry point `ada-scan`)** → ADA-MEM, ADA-PERF
  - Recursive scan for WAV, FLAC, AIFF, OGG, MP3 (libsndfile) and M4A / AAC (PyAV, `ada_core/decode.py`; streams frame by frame, container edit lists remove encoder priming).
  - `concurrent.futures.ProcessPoolExecutor`; each worker opens its own file and returns the `Result` (summary + 10 Hz arrays, ≈ 3 MB/h), never raw audio.
  - Flags: `-j/--jobs`, `--export-csv FILE`, `--export-json FILE`, `--plot DIR`, `--plot-format png|pdf|svg` (plots are rendered inside the worker).
  - CSV and JSON per spec §3.9; `rich` progress bar and summary table. Unreadable files are reported, not fatal.
  - If ADA-PERF misses 100×: profile first; candidates are float32 TP filtering and `reduceat` vs `reshape` on whole-sub-bin spans.

---

## Phase 2: Python Visualizer
- [x] **2.1 Timeline (`ada_core/visualizer.py`)** — shared time axis at $t_n$: $S$, $\text{TP}_{3s}$, PSR; PSR colored by zone; gated bins as gaps.
- [x] **2.2 Histogram** — percentage of gated duration per 1 dB PSR bin, zone-colored.
- [x] **2.3 `ada-plot` CLI** writing PNG/PDF/SVG per file.
- [x] **2.4 Compression views & plot size** → ADA-CEIL, ADA-PLOT
  - Loudness-vs-PSR 2D density (0.5 LU × 0.5 dB cells, share of gated duration, single-hue sequential scale).
  - Histogram of bin peaks relative to $\text{TP}_{\max}$ (0.25 dB steps, clipped at −12 dB) with the 0.5 dB ceiling window shaded; report field `peak_near_ceiling_pct`.
  - `--width PX` (default 2160) and `--aspect W:H` (default 16:10). Fixed 14.4 in layout; the pixel width sets the DPI, and there is no tight cropping, so output size is exact.

---

## Phase 3: C++ / JUCE Engine
- [x] **3.1 CMake (`CMakeLists.txt`)**
  - CMake ≥ 3.24 (tested with 4.4 and Visual Studio Build Tools 2026); JUCE via `FetchContent` pinned to `8.0.15` (an unpinned fetch pulls JUCE 9).
  - `juce_add_plugin(... FORMATS VST3 Standalone)`; `JUCE_USE_MP3AUDIOFORMAT=1`, `JUCE_USE_WINDOWS_MEDIA_FORMAT=1` (for 3.6).
  - Engine as a plain C++20 static library `ada_engine` (`src/engine/`, no JUCE dependency) shared by the plugin and the tests. Options `ADA_BUILD_PLUGIN`, `ADA_BUILD_TESTS`.
  - Test executable `ada_engine_tests` (doctest) registered with CTest. doctest `v2.4.12` and libebur128 `v1.2.6` are fetched as sources only (their own CMake scripts never run); libebur128 is linked into the tests for ADA-ORACLE only.
- [x] **3.2 `TruePeakDetector` (`src/engine/TruePeakDetector.h`)** → ADA-FIR, ADA-ISP, ADA-RATE
  - Port of spec §3.3 with fixed-size state. `src/engine/GeneratedTables.h` (Annex 2 table, delay, K-weighting prototype constants) is generated by `python/tools/gen_cpp_tables.py`; a pytest fails if it is out of date.
- [x] **3.3 `DynamicsEngine` (`src/engine/DynamicsEngine.h/.cpp`)** → ADA-PARITY, ADA-RT-RESET, T3341-11, T3341-14
  - `prepare(fs, channels)` allocates everything (two 8000-bin `LoudnessHistogram`s); `process()` works per sample, so host block size never matters.
  - Frames carry the **raw** PSR and the running LRA relative gate; consumers apply the spec §3.7 statistics gate (live: running gate; offline: final gate via `ada::summarize`, which reproduces Python exactly).
  - `requestReset()` sets an atomic flag; the reset runs inside `process()` at the next bin start by zeroing state.
  - `PluginProcessor` (`src/plugin/`) calls `prepare()` in `prepareToPlay()`, `process()` in `processBlock()`, and requests a reset on transport start (parameter `resetOnPlay`, default on).
- [x] **3.4 Lock-free telemetry (`src/plugin/MetricsFifo.h`)**
  - SPSC FIFO on `juce::AbstractFifo` with `std::array<DynamicsMetrics, 1024>` (≈ 100 s at 10 Hz); full FIFO → drop frame, count it; never wait.
  - `DynamicsMetrics` (see `src/engine/DynamicsEngine.h`): bin index, reset epoch, M, S, per-bin 10 ms peaks of M and S, max M / max S since reset, bin peak, TP 3 s, TP max, raw PSR, I, LRA, LRA gate, clip flag.
  - Minimal editor with numeric readouts and hover tooltips, Reset button, "reset when playback starts elsewhere" and "freeze while stopped" toggles, "< −70" display below the absolute gate (full GUI: Phase 4). The standalone build ignores the transport.
- [x] **3.5 Real-time & parity tests** → ADA-RT-ALLOC, ADA-RT-RESET, ADA-PARITY, ADA-ORACLE, T3341-11, T3341-14
  - `tests/cpp/`: 16 doctest cases, including NaN/inf input, re-prepare epochs, and reset at 48 kHz (D = 6) and 192 kHz (D = 0). ADA-RT-ALLOC replaces global `operator new`/`delete` with counting versions; a thread-local flag around `process()` turns any allocation into a failure (10 min of audio, random blocks 1–4096, resets). The MSVC debug `_CrtSetAllocHook` variant is not implemented yet.
  - ADA-PARITY: `ada_cli` runs the engine on raw float32 audio and prints JSON; `python/tests/test_parity.py` compares 17 signals × 2 block modes (per-bin values, summary and PSR zone shares; NaN, +inf and −inf must match exactly) against Python (skips if `ada_cli` is not built). The MP3 and FLAC parity files follow with 3.6, when the C++ side gets its own decoder.
- [ ] **3.6 Standalone offline reader**
  - `juce::AudioFormatManager::registerBasicFormats()` → `createReaderFor(file)` (raw pointer; own it in `std::unique_ptr`) → `juce::AudioFormatReader::read` in fixed-size blocks on a `juce::Thread`, driving a separate `DynamicsEngine` instance at full CPU speed.
  - M4A/AAC: enable `JUCE_USE_WINDOWS_MEDIA_FORMAT=1` (Windows; macOS reads it through CoreAudio); add an AAC and an ALAC file to the ADA-PARITY set.
  - Frames go to a history vector on that thread (reserve from `lengthInSamples`, capped at 6 h; growth allowed off the audio thread).

---

## Phase 4: GUI & Polish
- [ ] **4.1 Main layout (`src/gui/MainComponent.cpp`)** — resizable vector UI, dark theme, `juce::FileDragAndDropTarget` in Standalone.
- [ ] **4.2 History graph (`src/gui/HistoryGraph.cpp`)** — reads the 54 000-frame ring (spec §4); wheel zoom, drag pan; PSR zone fills; `juce::Path` rendering.
- [ ] **4.3 Readouts (`src/gui/MetricDisplays.cpp`)** — current PSR, Min PSR, PLR, $I$, $S$, Max M, $\text{TP}_{\max}$, LRA; clip indicator (default 0.0 dBTP, configurable); Reset button.
- [ ] **4.4 PSR histogram (`src/gui/DynamicHistogram.cpp`)** — same binning as Phase 2.2.
- [ ] **4.5 Delivery target presets** — user-editable defaults: Spotify −14 LUFS / −1 dBTP, Apple Music −16 LUFS / −1 dBTP, YouTube −14 LUFS / −1 dBTP. Check current platform documentation before release.
