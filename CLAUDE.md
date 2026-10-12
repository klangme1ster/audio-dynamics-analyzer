# Audio Dynamics Analyzer (ADA) — AI Developer Guidelines

`PROJECT_SPECIFICATION.md` is normative; its §2 (validation suite) defines "done". `IMPLEMENTATION_PLAN.md` gives the build order.

## Tech Stack
* **Stage 1 (Python reference engine & scanner):** Python 3.11+, NumPy, SciPy, SoundFile, PyAV (M4A/AAC), Rich, Matplotlib, Pytest.
* **Stage 2 (Plugin & Desktop App):** C++20, JUCE 8 (pinned `8.0.x` tag), CMake 3.24+. Own DSP engine ported from Stage 1; `libebur128` only as a test oracle.

## Workflow
* Write or update the spec §2 test for a behavior before implementing it.
* Python is the numerical reference; C++ must match it within the ADA-PARITY tolerances.
* Check `git rev-parse --show-toplevel` before any git operation; do not assume the repo exists.

## Real-Time Audio & Threading Guardrails (C++)
1. **Audio thread:** inside `processBlock()` no `new`/`delete`, `malloc`/`free`, container growth (`push_back`, `resize`), locks, thread signalling (`notify`, `WaitableEvent`), or blocking calls. Enforced by ADA-RT-ALLOC.
2. **Lifecycle:** allocate everything in `prepareToPlay()`, release in `releaseResources()`.
3. **Reset:** atomic request flag; executed in-place at the next bin boundary by zeroing state. Never re-create engine objects on the audio thread.
4. **Unbounded sessions:** integrated loudness and LRA use fixed 0.01 LU histograms, never growing lists.
5. **Telemetry:** audio → UI only through the fixed-size `juce::AbstractFifo` queue or atomics; on overflow drop, never wait.

## Python Guardrails
1. **No pure-Python sample loops.** Use NumPy / SciPy C-extensions (`sosfilt`, `lfilter`, `np.add.reduceat`, `maximum_filter1d`). Loops over chunks are fine.
2. **Bounded memory.** Stream with `soundfile.blocks`; the engine accepts any chunk length and carries filter state (`zi`), partial sub-bin sums and the open true-peak bin across chunks. Never load or oversample a whole track. No `resample_poly`/`upfirdn` for true peak (stateless).
3. `scipy.ndimage.maximum_filter1d` must be called with `mode="constant"` (default `"reflect"` is wrong for a causal window).

## DSP Rules (details in spec §3)
* **Grid:** bin $n$ = samples $[nH, (n+1)H)$, $H = f_s/10$; sub-bin $h = f_s/100$ (for Max M / Max S). Stamp = end of bin. Only complete bins are emitted; a trailing partial bin counts toward $\text{TP}_{\max}$ only.
* **PSR alignment:** $\text{PSR}[n] = \text{TP}_{3s}[n] - S[n]$, both over bins $n-29..n$. True-peak outputs are attributed to input time $k - D$ ($D = 6$ for 2×/4×, 0 for 1×), so bin $n$'s peak closes $D$ samples after the boundary.
* **Gating:** relative gates use the mean of block **powers**, never the mean of LUFS values. $I$ gates with `>`, LRA with `≥` and percentile index $\lfloor (c-1)p + 0.5 \rfloor$.
* **Mono:** $N = 1$, $G = 1.0$; a −23 dBFS mono sine reads −26 LUFS. Never sum as dual-mono.
* **True peak:** BS.1770 Annex 2 48-tap filter; 4× for $f_s \le 48$ kHz, 2× (phases 0, 2) for $\le 96$ kHz, 1× above. One coefficient source for Python and C++.

## Build & Test Reference
* Install Python package (plus CMake for the C++ build): `pip install -e "python[dev,cpp]"`
* C++ toolchain on Windows: Visual Studio Build Tools 2026 (C++ workload); CMake comes from the `cpp` extra
* After changing K-weighting or true-peak constants in Python: `python python/tools/gen_cpp_tables.py`
* Run Python tests: `pytest python/tests` (EBU file tests skip unless `python/tools/fetch_ebu_testset.py` was run)
* Configure CMake: `cmake -B build -S . -DCMAKE_BUILD_TYPE=Release`
* Build Standalone and VST3: `cmake --build build --config Release -j`
* Run C++ tests (engine + file analysis): `ctest --test-dir build -C Release`
* Inspect the GUI without a host: `build/src/plugin/ada_view_snapshot_artefacts/Release/ada_view_snapshot.exe out.png [width] [height]`
* Python ↔ C++ parity: `pytest python/tests/test_parity.py` (needs the built `ada_cli`)
