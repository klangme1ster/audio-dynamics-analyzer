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
  - Minimal editor with numeric readouts and hover tooltips, Reset button, "new measurement when playback jumps" and "freeze while stopped" toggles (jump logic in `ada::TransportTracker`, unit-tested; re-activation with unchanged settings keeps the measurement; restart reason shown), "< −70" display below the absolute gate (full GUI: Phase 4). The standalone build ignores the transport.
- [x] **3.5 Real-time & parity tests** → ADA-RT-ALLOC, ADA-RT-RESET, ADA-PARITY, ADA-ORACLE, T3341-11, T3341-14
  - `tests/cpp/`: 36 doctest cases (engine, real-time, history store, transport tracker), including NaN/inf input, re-prepare epochs, and reset at 48 kHz (D = 6) and 192 kHz (D = 0). ADA-RT-ALLOC replaces global `operator new`/`delete` with counting versions; a thread-local flag around `process()` turns any allocation into a failure (10 min of audio, random blocks 1–4096, resets). The MSVC debug `_CrtSetAllocHook` variant is not implemented yet.
  - ADA-PARITY: `ada_cli` runs the engine on raw float32 audio and prints JSON; `python/tests/test_parity.py` compares 17 signals × 2 block modes (per-bin values, summary and PSR zone shares; NaN, +inf and −inf must match exactly) against Python (skips if `ada_cli` is not built). The MP3 and FLAC parity files follow with 3.6, when the C++ side gets its own decoder.
- [x] **3.6 Offline file analysis (`src/plugin/FileAnalysis.h/.cpp`)** — in the standalone app and the plugin
  - `juce::AudioFormatManager::registerBasicFormats()` (WAV, AIFF, FLAC, Ogg, MP3 with `JUCE_USE_MP3AUDIOFORMAT=1`, WMA through the Windows Media Format SDK with `JUCE_USE_WINDOWS_MEDIA_FORMAT=1`; M4A/AAC through `ada::MediaFoundationAudioFormat`, see below; all registered by `ada::registerAudioFormats`) → `createReaderFor` → a `juce::Thread` reads 32 768-frame blocks into a separate `DynamicsEngine` at full CPU speed (≈ 300–500× real time).
  - Frames go to the message thread through a `MetricsFifo` (the worker waits when it is full, never drops) and fill a per-file `HistoryStore` (sized from `lengthInSamples` + 5 %, graph capped at 6 h) plus a complete frame list for `ada::summarize`, so the file results equal the Python scanner's.
  - Owned by the processor, so closing the plugin window keeps the result. Mono/stereo at a multiple of 100 Hz only; other files are rejected with a message.
  - Tests: `tests/cpp/test_file_analysis.cpp` (`ada_file_analysis_tests`, CTest): WAV and FLAC written with JUCE's writers give exactly the same frames as feeding the samples to the engine; T3341 case 3 and mono −26 LUFS; rejection of 4-channel and 22.05 kHz files; cancelling.
  - A decoding error part-way through is reported as a failure, not as a complete result. Each analysis has a run id; a new file always gets a fresh graph view, and the graph never points at a deleted store.
  - M4A / AAC (Windows, `src/plugin/MediaFoundationFormat.h/.cpp`): a `juce::AudioFormat` around Media Foundation's `IMFSourceReader` (AAC and ALAC in MP4, raw ADTS AAC), decoding to float at the native rate and channel count. Media Foundation ignores the MP4 edit list, so `ada::readMp4Timing` reads the encoder priming and the real length from the edit list (as FFmpeg does) or iTunes' `iTunSMPB` tag, and the reader drops / cuts accordingly. Raw ADTS `.aac` has no stored length and Media Foundation's estimate can be seconds short, so `ada::countAdtsSamples` counts the frames (1024 samples each, 2048 with SBR). Decoded blocks are placed by counting samples, not by Media Foundation's timestamps, which differ between Windows versions (Windows 11 stamps the first AAC frame 0, Windows Server 2022 +1024 samples, for the same samples); `python/tools/probe_media_foundation.py` checks this on the build machine. The decoder is created on the reading thread (COM per thread) and released there by the worker.
  - M4A tests: `readMp4Timing` on synthetic box trees (edit list, iTunSMPB, neither, truncated) in `test_file_analysis.cpp`; ADA-PARITY for AAC and ALAC files through `ada_file_cli` (file reader + engine, JSON like `ada_cli`) in `python/tests/test_parity.py`. Checked by hand on real files (phone recordings, iTunes-tagged files): identical values, lengths within a few samples. AAC with PNS (FFmpeg's encoder default) gives decoder-dependent peaks; documented in USAGE.md.
  - Still open: MP3 parity against the Python decoder (different decoders may differ slightly); macOS builds (M4A would go through JUCE's Core Audio reader) are untested.

---

## Phase 4: GUI & Polish
- [x] **4.1 Main layout (`src/plugin/PluginEditor.cpp`)** — resizable dark-theme layout: source bar ("Live | Output | File" view switch, Output: see 4.6; "Live analysis" on/off, "Open file..."), editor size saved in the plugin state, standalone title bar shows the current view (file name and duration, output device, or live input), standalone output silent unless "Play input to output" (JUCE's input mute is turned off), readouts strip, history + PSR meter, zone bar, controls; colours in `src/plugin/Theme.h`. `juce::FileDragAndDropTarget` with a drop overlay; the standalone app also analyses a file given on the command line (Explorer "Open with"). In file mode the readouts show the file summary (PSR mean / min / P10, Max S / Max M, I, LRA, max true peak, PLR) and tooltips switch to file wording and the graph zooms out to the whole file. Graph columns join up at any width (no dotted slopes after resizing).
- [x] **4.2 History graph (`src/plugin/HistoryGraph.cpp`)**
  - Data: `ada::HistoryStore` (`src/engine/`, no JUCE; tests in `tests/cpp/test_history.cpp`) keeps a 54 000-frame ring plus PSR statistics since reset. `ada::MetricsHistory` (owned by the processor, message thread) drains the FIFO 30×/s, so the history keeps growing while the editor is closed.
  - View: short-term loudness and 3 s true peak (auto-ranged dB axis) above PSR over labelled zone bands, shared m:ss axis. Wheel zoom 10 s – 90 min (6 h for files; limited to what the store holds), drag to scroll back, double-click to follow live, hover crosshair with values. More frames than pixels → per-column min/max envelope.
- [x] **4.3 Readouts & PSR meter (`src/plugin/PluginEditor.cpp`, `src/plugin/DynamicsViews.cpp`)** — readouts strip with hover help and clip indicator; vertical PSR meter: current value as a zone-coloured bar, solid marker for PSR min, dashed marker for P10 (0.1 dB resolution, never shown below the minimum).
- [x] **4.4 Zone distribution bar (`src/plugin/DynamicsViews.cpp`)** — stacked bar of the measured time per PSR zone since reset, with a labelled legend and percentages (identity never by colour alone). Replaces the planned PSR histogram.
- Developer tool `ada_view_snapshot` renders the views for a synthetic song into a PNG, for checking the GUI without a host.
- [x] **4.6 Windows output source (`src/plugin/OutputMonitor.h/.cpp`)** — standalone app, Windows only
  - WASAPI loopback capture of a chosen render endpoint (device list with "Default output"), own engine on a capture thread, own 90-minute history; silence filled in while nothing plays; device removal reported.
  - Editor: **Live | Output | File** view switch, device list, "Output analysis" on/off, Reset per view, title `– output: <device>`, `--output` command-line option.
  - Tests: sample conversion (`ada::interleavedToPlanar`, float32 / int16 / int24 / int32, surround → front L/R) in `tests/cpp/test_engine.cpp`; capture verified manually with a −35 dBFS test tone (measured −35.6 LUFS / −35.4 dBTP through the Windows mixer).
- [x] **4.5 Delivery target presets (`src/engine/DeliveryTargets.h`, `TargetPanel` in `src/plugin/DynamicsViews.cpp`)**
  - Presets: Spotify (−14 LUFS, −1 dBTP, −2 dBTP for masters louder than −14), Apple Music (−16 / −1), YouTube (−14 / −1), Tidal (−14 / −1), EBU R 128 (−23 ±0.5 LU / −1), ATSC A/85 (−24 ±2 / −2), Custom (dialog). Saved with the plugin state.
  - `ada::evaluateTarget` (pure, tested in `tests/cpp/test_engine.cpp`): streaming deviations are warnings ("turned down by X dB"), broadcast deviations problems; peaks above the ceiling warning (streaming) or problem (broadcast, or above 0 dBTP).
  - Compact target box in the corner under the PSR meter (selector, icon + short line per verdict, full sentences in the tooltip) in all three views; the zone bar spans the graph's width; dashed target and ceiling lines in the history graph.
  - Check current platform documentation before a release.

---

## Status and open items
As of 2026-10-25 all phases above are done. v0.1.0 was released from `ad56ed8` (Windows x64 zip: VST3 + standalone app, built and tested by `.github/workflows/release.yml`).

Channel handling (spec §1 scope, BS.1770): mono and stereo only, in the Python engine, the plugin bus layouts (`isBusesLayoutSupported`) and file analysis. Stereo is K-weighted per channel and the powers are summed with $G_i = 1.0$ (`DynamicsEngine::processFrame`), never folded down to mono; true peak is the maximum over channels. Mono is $N = 1$, not dual-mono (ADA-MONO). The Windows output source (4.6) takes only the front L/R of surround devices.

Open:
- [ ] **Surround (5.1, 7.1).** BS.1770 weights: 1.0 for L, R, C; 1.41 for the surround channels; LFE excluded. Needs a channel-layout mapping per source (file channel masks, host bus layouts, WASAPI speaker masks), a spec §1/§2 change (scope and new compliance tests, e.g. the multichannel cases of the EBU loudness test set) and the same change in Python and C++ (ADA-PARITY).
- [ ] **MP3 parity** of JUCE's decoder against the Python decoder (see 3.6).
- [ ] **macOS builds**, including M4A through JUCE's Core Audio reader (see 3.6); a CI job would be the first check.
- [ ] **Delivery targets:** check the platform values against current documentation before the next release (see 4.5).
