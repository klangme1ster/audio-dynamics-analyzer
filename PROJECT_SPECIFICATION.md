# Audio Dynamics Analyzer (ADA) — Technical Specification

## 1. Overview & Architecture
ADA is a two-stage loudness and dynamics analysis suite for mono, stereo and 5.0 / 5.1 / 7.1 surround program material. It measures second-by-second transient density (micro-dynamics, PSR) alongside full-program dynamic range (macro-dynamics, PLR and LRA).

* **Stage 1 — Python reference engine & batch scanner.** Multi-core CLI, target $\ge 100\times$ real-time per CPU core. Audio is streamed; memory is O(duration / 10 ms) for the metric arrays (< 10 MB per hour) plus a fixed per-chunk working set. Stage 1 is the **numerical reference** for Stage 2.
* **Stage 2 — C++20 / JUCE 8 plugin & standalone app.** One codebase built as VST3 (DAWs, Audacity ≥ 3.2) and Standalone (faster-than-real-time drag-and-drop file analysis). The DSP engine is a direct port of the Stage 1 definitions in §3; no third-party loudness library runs in the product (libebur128 is used only as a test oracle, §2.2).

**Scope:** mono, stereo and the surround layouts of §3.11 (5.0, 5.1, 7.1). Sample rates must be integer multiples of 100 Hz (so both the 100 ms bin and the 10 ms sub-bin are whole numbers of samples): 32, 44.1, 48, 88.2, 96, 176.4, 192 kHz. Other rates are rejected with an explicit error. A multichannel source whose layout cannot be determined (§3.11) is rejected until the layout is supplied or confirmed; ADA never guesses silently.

---

## 2. Acceptance Criteria (Validation Suite)
The DSP definitions in §3 exist to pass these tests. An implementation is complete only when every applicable row passes. Tests are written **before** the algorithm they verify.

"Synth" = generated in the test fixture from the parameters in the referenced document. "File" = official WAV from the EBU loudness test set, stored under `python/tests/fixtures/ebu/` (not committed; tests `skip` with a message when absent). "Live" = requires real-time streaming, tested only in Stage 2.

### 2.1 External compliance signals (EBU Tech 3341 V4, Tech 3342 V4)
Levels are sine **peak** levels in dBFS, applied to both channels of a stereo signal (case 6: per channel as listed in the file).

| ID | Source | Signal | Metric | Expected | Tolerance |
|---|---|---|---|---|---|
| T3341-1 | Tech 3341 case 1 | Synth: stereo 1 kHz, −23 dBFS, 20 s | M, S, I | −23.0 LUFS | ±0.1 LU |
| T3341-2 | case 2 | Synth: stereo 1 kHz, −33 dBFS, 20 s | M, S, I | −33.0 LUFS | ±0.1 LU |
| T3341-3 | case 3 | Synth: −36 / −23 / −36 dBFS, 10 / 60 / 10 s | I | −23.0 LUFS | ±0.1 LU |
| T3341-4 | case 4 | Synth: −72 / −36 / −23 / −36 / −72 dBFS, 10 / 10 / 60 / 10 / 10 s | I (absolute gate) | −23.0 LUFS | ±0.1 LU |
| T3341-5 | case 5 | Synth: −26 / −20 / −26 dBFS, 20 / 20.1 / 20 s | I (relative gate) | −23.0 LUFS | ±0.1 LU |
| T3341-6 | case 6 | File: 5.0 (`seq-3341-6-5channels`, order L R C Ls Rs; layout supplied) and 5.1 (`seq-3341-6-6channels-WAVEEX`, silent LFE; layout read from the channel mask `0x3F`). Peaks L/R −28, C −24, Ls/Rs −30 dBFS | I, Max M, Max S (S on the 10 ms sub-grid for t ≥ 3 s) | −23.0 LUFS | ±0.1 LU |
| T3341-7, -8 | cases 7, 8 | File: authentic programme | I | −23.0 LUFS | ±0.1 LU |
| T3341-9 | case 9 | Synth: (1.34 s at −20; 1.66 s at −30 dBFS) × 5 | S on 10 ms sub-grid (§3.4), every value for t ≥ 3 s; Max S | −23.0 LUFS | ±0.1 LU |
| T3341-10 | case 10 | File | Max S (10 ms sub-grid) | −23.0 LUFS | ±0.1 LU |
| T3341-11 | case 11 | Live (Stage 2 only) | S | per Tech 3341 | ±0.1 LU |
| T3341-12 | case 12 | Synth: (0.18 s at −20; 0.22 s at −30 dBFS) × 25 | M on 10 ms sub-grid, every value for t ≥ 1 s; Max M | −23.0 LUFS | ±0.1 LU |
| T3341-13 | case 13 | File | Max M (10 ms sub-grid) | −23.0 LUFS | ±0.1 LU |
| T3341-14 | case 14 | Live (Stage 2 only) | M | per Tech 3341 | ±0.1 LU |
| T3341-TP-a | cases 15–18 | Synth per Tech 3341, 50 ms raised-cosine fades (an abrupt onset is a step whose reconstruction overshoots) | Max TP | −6.0 dBTP | +0.2 / −0.4 dB |
| T3341-TP-b | case 19 | Synth per Tech 3341 | Max TP | +3.0 dBTP | +0.2 / −0.4 dB |
| T3341-TP-c | cases 20–23 | File (needs the EBU anti-aliasing filter, not synthesizable) | Max TP | 0.0 dBTP | +0.2 / −0.4 dB |
| T3342-1 | Tech 3342 case 1 | Synth: stereo 1 kHz, −20 / −30 dBFS, 20 s each | LRA | 10 LU | ±1 LU |
| T3342-2 | case 2 | Synth: −20 / −15 dBFS, 20 s each | LRA | 5 LU | ±1 LU |
| T3342-3 | case 3 | Synth: −40 / −20 dBFS, 20 s each | LRA | 20 LU | ±1 LU |
| T3342-4 | case 4 | Synth: −50 / −35 / −20 / −35 / −50 dBFS, 20 s each | LRA | 15 LU | ±1 LU |
| T3342-5 | case 5 | File: authentic programme | LRA | 5 LU | ±1 LU |
| T3342-6 | case 6 | File: authentic programme | LRA | 15 LU | ±1 LU |

ITU-R BS.2217 compliance files may be added as further "File" rows once obtained; no BS.2217 signal is synthesized.

### 2.2 ADA-specific tests
| ID | Purpose | Signal | Pass condition |
|---|---|---|---|
| ADA-MONO | Mono weighting | 1 kHz sine, −23 dBFS, **mono**, 20 s | I = −26.0 ±0.1 LUFS (a dual-mono sum would wrongly read −23.0) |
| ADA-ISP | Inter-sample peak | Sine at $f_s/4$, phase 45°, sample peak 0 dBFS, 20 s, at 44.1 and 48 kHz | Max TP = +3.01 dBTP, +0.2 / −0.4 dB (Annex 2 filter reads ≈ +3.06) |
| ADA-RATE | Oversampling factor per rate | ADA-ISP at 88.2, 96, 176.4, 192 kHz | $L$ per §3.3 asserted on detector config; TP within tolerance at 88.2 / 96 kHz |
| ADA-KCOEF | K-weighting coefficients | — | §3.2 formulas at 48 kHz reproduce the §3.2 table to $\le 10^{-12}$ absolute |
| ADA-FIR | Annex 2 filter integrity | — | 48 coefficients; $h[47-k] = h[k]$; phase DC gains $\sum_j h_p[j]$ = [1.00158691, 0.97302246, 0.97302246, 1.00158691] for $p = 0..3$ (±1e-8) |
| ADA-ALIGN | Causal window alignment | 48 kHz, 20 s: −30 dBFS 1 kHz bed + 10 ms 3 kHz burst at −6 dBFS fully inside bin $n_0 = 50$, ≥ 20 ms from both bin edges | $S$ and $\text{TP}_{3s}$ rise by > 3 dB at bin $n_0$ and return to the bed value at bin $n_0 + 30$; outside bins $n_0 .. n_0+29$ both differ from a bed-only run by $\le 10^{-6}$ dB |
| ADA-CHUNK | Chunk invariance | 60.0437 s pink noise (not a multiple of $H$), chunk sizes 1, 5, 7.3 s, a random-size sequence (1–50 000 frames), and one block | All per-bin and per-sub-bin arrays and report fields agree to $\le 10^{-9}$ dB / LU |
| ADA-TAIL | Final partial bin | 10.05 s file with a single full-scale inter-sample peak in the last 50 ms | Peak is included in $\text{TP}_{\max}$; bin count = 100 |
| ADA-WARMUP | Warm-up & silence | (a) 2 s file; (b) 30 s file with 10 s digital silence in the middle | (a) PSR stats = n/a, LRA = n/a; (b) silent bins excluded from PSR stats; no ±inf/NaN in any report field |
| ADA-CEIL | Peak density near the ceiling | (a) stereo 1 kHz −20 dBFS, 20 s; (b) −20 / −30 dBFS, 20 s each; (c) 2 s file | (a) 100 %; (b) 171 / 371 = 46.1 ±0.3 %; (c) n/a |
| ADA-M4A | M4A / AAC decoding | (a) 10 s 16-bit noise + tone as WAV and ALAC M4A; (b) 1 kHz −23 dBFS, 20 s, AAC 256 kbit/s, mono and stereo | (a) all arrays and report fields identical (≤ 1e-9); (b) duration 20 s ±30 ms, I = −23.0 (stereo) / −26.0 (mono) ±0.1 LUFS |
| ADA-SURR-W | Surround channel weights | 5.1 and 7.1, 1 kHz sine, −23 dBFS, 20 s on **one** channel, the others silent; once per channel | I = −26.0 (L, R, C) / −24.51 (Ls, Rs, Lb, Rb) ±0.1 LUFS; LFE alone: I `n/a` (silent) |
| ADA-LFE | LFE: out of the loudness, in the true peak | 5.1: −30 dBFS pink noise bed on the other channels; run once with the LFE silent and once with 50 Hz at 0 dBFS in the LFE | $M$, $S$, their 10 ms arrays, $I$, LRA, Max M and Max S identical (≤ 1e-9); with the 0 dBFS LFE sine $\text{TP}_{\max}$ = 0.0 dBTP (+0.2 / −0.4 dB) against the quiet bed's peak without it, and PLR follows |
| ADA-SURR-TP | Surround true peak | 7.1, ADA-ISP signal (§2.2) on one channel at a time, others silent | $\text{TP}_{\max}$ = +3.01 dBTP, +0.2 / −0.4 dB for every channel, the LFE included (alone, it reads as silence for loudness) |
| ADA-NOFOLD | No downmix | 5.1: identical −23 dBFS 1 kHz on L and R, once in phase and once with R inverted (everything else silent) | I identical (≤ 1e-9), and equal to ADA-SURR-W's L-alone reading + 3.01 LU (two channels) ±0.05 |
| ADA-LAYOUT | Layout resolution (§3.11) | (a) 6- and 8-channel WAV with `WAVE_FORMAT_EXTENSIBLE` masks `0x3F`, `0x60F` (5.1) and `0x63F` (7.1); (b) 5-, 6-, 8-channel PCM WAV without mask; (c) 3, 4, 7 channels without mask; (d) user-supplied role list in permuted order; (e) layout whose role count differs from the channel count | (a) layout and weights follow the mask; (b) rejected with an error naming the suggested layout (5.0 / 5.1 / 7.1); accepted when that layout is supplied or confirmed; (c) rejected, no suggestion; (d) results identical (≤ 1e-9) to the same signal in file order with the matching role list; (e) rejected |
| ADA-SURR-CHUNK | Chunk invariance, multichannel | ADA-CHUNK sizes on 5.1 pink noise | As ADA-CHUNK |
| ADA-PLOT | Plot geometry | Any result; 2160 / 1200 / 1000 px at 16:10 / 4:3 / 2:1; PDF and SVG | PNG is exactly width × width/aspect pixels; vector formats render |
| ADA-MEM | Memory bound | 60 min stereo 48 kHz WAV, streamed from disk | `tracemalloc` peak < 64 MB **and** process RSS growth < 128 MB during analysis |
| ADA-PERF | Throughput | Same 60 min WAV | ≥ 100× real-time on one core, including decode (benchmark marker, not run in CI by default) |
| ADA-PARITY | Stage 1 ↔ Stage 2 | All Synth signals above (including 5.1 and 7.1 pink-noise and tone signals with an active LFE) plus one MP3 and one FLAC file, through the C++ engine's test executable; M4A (Windows): 15 s drum-like test signal as AAC 256 kbit/s (stereo 44.1 kHz, mono 48 kHz; encoded without PNS) and ALAC, through the plugin's file reader (`ada_file_cli`); 5.0 / 5.1 / 7.1 WAV (masks, and a supplied layout for files without one) and AAC 5.1 M4A through the same reader | per-bin $M$, $S$, $\text{TP}_{3s}$, PSR within 0.01 dB; $I$ within 0.02 LU; LRA within 0.05 LU; $\text{TP}_{\max}$ within 0.01 dB; M4A: same frame count (priming removed by both). AAC with PNS: only $S$ (0.05 dB), $I$ and LRA are compared, since the decoder generates the substituted noise |
| ADA-ORACLE | Independent cross-check (C++) | T3341-1…5 signals through libebur128 (test build only) | ADA and libebur128 $I$ agree within 0.1 LU |
| ADA-RT-ALLOC | Real-time safety (C++) | 10 min of random-length host blocks (1–4096 frames), with resets and transport toggles | Zero calls to global `operator new` / `operator delete` (and, in MSVC debug builds, zero `_CrtSetAllocHook` events) inside `processBlock()` after `prepareToPlay()` |
| ADA-RT-RESET | Reset correctness (C++) | Reset at random sample positions during T3341-3 | After reset, metrics equal a fresh engine started at the next bin boundary |

---

## 3. DSP Definitions

### 3.1 Notation & control grid
* $f_s$ sample rate. **Bin** $H = f_s/10$ samples (100 ms); **sub-bin** $h = f_s/100$ samples (10 ms). Input sample index $k = 0, 1, \dots$; bin $n$ covers $[nH, (n+1)H)$, sub-bin $u$ covers $[uh, (u+1)h)$.
* Per-bin metrics are stamped $t_n = (n+1)\cdot 0.1$ s, the **end** of bin $n$.
* Only complete bins are emitted. A trailing partial bin at end of stream contributes to $\text{TP}_{\max}$ only (consistent with BS.1770 discarding incomplete blocks).
* Channel weights: mono $N = 1$, $G_1 = 1.0$ (never summed as dual-mono). Stereo $N = 2$, $G_1 = G_2 = 1.0$. Surround: per channel role as in §3.11 ($G = 1.0$ for L, R, C; $G = 1.41$ for the surround and rear channels); the LFE is not measured. Channels are never folded down; their weighted powers are summed.

### 3.2 K-weighting (ITU-R BS.1770-5, unchanged from -4)
Two cascaded biquads (second-order sections), state initialized to **zero** at stream start or reset (not `sosfilt_zi`, which assumes a pre-existing steady state). Computed in float64.

Reference coefficients at 48 kHz (BS.1770 Tables 1 and 2):

| | $b_0$ | $b_1$ | $b_2$ | $a_1$ | $a_2$ |
|---|---|---|---|---|---|
| Stage 1 (shelf) | 1.53512485958697 | −2.69169618940638 | 1.19839281085285 | −1.69065929318241 | 0.73248077421585 |
| Stage 2 (RLB HP) | 1.0 | −2.0 | 1.0 | −1.99004745483398 | 0.99007225036621 |

For any $f_s$, with $K = \tan(\pi f_0 / f_s)$ (same derivation as libebur128):

* **Stage 1:** $f_0 = 1681.974450955533$ Hz, $G = 3.999843853973347$ dB, $Q = 0.7071752369554196$, $V_h = 10^{G/20}$, $V_b = V_h^{0.4996667741545416}$, $a_0 = 1 + K/Q + K^2$:
  $b_0 = (V_h + V_b K/Q + K^2)/a_0$, $b_1 = 2(K^2 - V_h)/a_0$, $b_2 = (V_h - V_b K/Q + K^2)/a_0$, $a_1 = 2(K^2 - 1)/a_0$, $a_2 = (1 - K/Q + K^2)/a_0$.
* **Stage 2:** $f_0 = 38.13547087602444$ Hz, $Q = 0.5003270373238773$, $a_0 = 1 + K/Q + K^2$:
  $b = [1, -2, 1]$, $a_1 = 2(K^2 - 1)/a_0$, $a_2 = (1 - K/Q + K^2)/a_0$.

### 3.3 True Peak (BS.1770-5 Annex 2)
* **Filter:** the Annex 2 48-coefficient interpolation filter, copied verbatim, as 4 polyphase branches of 12 taps: $h_p[j] = h[4j + p]$, i.e. column $p$ of the Annex 2 table. The prototype is symmetric (linear phase, centre 23.5).
* **Oversampling factor $L$:**
  * $f_s \le 48$ kHz: $L = 4$, phases $p \in \{0,1,2,3\}$.
  * $48 < f_s \le 96$ kHz: $L = 2$, phases $p \in \{0, 2\}$ (an exact subset of the 4× interpolator).
  * $f_s > 96$ kHz: $L = 1$, sample peak $|x[k]|$.
  * *Documented deviation:* BS.1770 Annex 2 associates the "dB TP" label with an oversampled rate ≥ 192 kHz. 32 kHz (128 kHz), 44.1 kHz and 88.2 kHz (176.4 kHz) and 176.4 kHz ($L=1$) fall below that. ADA follows the Annex 2 reference filter and reports $L$ in every result.
* **Polyphase output:** $y_p[k] = \sum_{j=0}^{11} h_p[j]\, x[k-j]$, per channel on the **unweighted** input, with 11 samples of state carried across chunks (zero at stream start or reset).
* **Delay compensation:** $y_p[k]$ represents input time $k - (23.5 - p)/4$, i.e. 5.125–5.875 samples late. All phases are attributed to input time $k - D$ with $D = 6$ for $L \in \{2, 4\}$ and $D = 0$ for $L = 1$; residual error 0.125–0.875 input samples. Output $y_p[k]$ belongs to bin $\lfloor (k - D)/H \rfloor$.
* **Bin closing:** consequently $P[n]$ is final only after input sample $(n+1)H + D - 1$ has been processed. Stage 1 carries the running maximum of the last open bin across chunks; Stage 2 emits each frame $D$ samples after the bin boundary (§4). At end of stream, $D$ zeros are fed so the last complete bin and the trailing partial bin are closed.
* **Per-bin peak:** $P[n] = \max |y_p[k]|$ over channels, active phases and outputs attributed to bin $n$. In dBTP: $20\log_{10} P[n]$.
* $\text{TP}_{\max}$ = max over all bins and the trailing partial bin.

### 3.4 Loudness on the grid
Per channel $i$, K-weighted signal $x_{K,i}$. Sub-bin energy $e_i[u] = \sum_{k=uh}^{(u+1)h-1} x_{K,i}[k]^2$; bin energy $E_i[n] = \sum_{u=10n}^{10n+9} e_i[u]$. Energies before stream start are zero.

Window loudness over the $W$ bins ending at bin $n$:
$$\ell_W[n] = -0.691 + 10\log_{10}\Big( \sum_i G_i \frac{1}{WH} \sum_{m=n-W+1}^{n} E_i[m] \Big)$$

* **Momentary** $M[n] = \ell_4[n]$ (400 ms). **Short-term** $S[n] = \ell_{30}[n]$ (3000 ms). Both cover $[t_n - W\cdot 0.1\text{ s},\ t_n)$ exactly in input samples.
* **Max M / Max S** (report fields and T3341-9…13): the same formula evaluated on the 10 ms sub-grid (windows of 40 / 300 sub-bins ending at every sub-bin $u \ge 39$ / $u \ge 299$). A 100 ms read-out misses up to 50 ms of a short burst and fails case 13.
* $10\log_{10}(0) = -\infty$ is handled explicitly (no warnings, no NaN propagation).
* Non-finite input samples (NaN, ±inf) are replaced by 0 before any processing; otherwise a single bad sample would leave the K-filter state NaN and stop the loudness measurement for good.

### 3.5 Integrated Loudness $I$ (BS.1770 gating)
* Gating block $j$ = bins $j-3 .. j$ for $j \ge 3$ (400 ms, 75 % overlap). Block power $z_j = \sum_i G_i \frac{1}{4H}\sum_{m=j-3}^{j} E_i[m]$, loudness $l_j = -0.691 + 10\log_{10} z_j$.
* Absolute gate: $J_a = \{ j : l_j > -70 \}$.
* Relative threshold $\Gamma_r = -0.691 + 10\log_{10}(\overline{z}_{J_a}) - 10$, where $\overline{z}_{J_a}$ is the **arithmetic mean of block powers** (never the mean of $l_j$).
* $J = \{ j \in J_a : l_j > \Gamma_r \}$; $I = -0.691 + 10\log_{10}(\overline{z}_J)$. $J = \emptyset$ → `n/a`.

### 3.6 Loudness Range LRA (EBU Tech 3342)
* Short-term values $S[n]$ for $n \ge 29$ (complete 3 s windows), one per 100 ms (Tech 3342 requires ≥ 10 Hz; libebur128's 1 s hop is not used).
* Absolute gate: keep $S[n] \ge -70$ LUFS. Relative threshold $\Gamma_{r,\text{LRA}} = -0.691 + 10\log_{10}(\text{mean power of kept windows}) - 20$; keep $S[n] \ge \Gamma_{r,\text{LRA}}$.
* Sort kept values ascending ($v_0 .. v_{c-1}$). Percentile $p$ uses index $\lfloor (c-1)\,p + 0.5 \rfloor$ (Tech 3342 reference). $\text{LRA} = v_{\text{idx}(0.95)} - v_{\text{idx}(0.10)}$. $c < 2$ → `n/a`.

### 3.7 PSR (Peak-to-Short-Term Ratio)
$$\text{TP}_{3s}[n] = 20\log_{10} \max_{m=n-29}^{n} P[m], \qquad \text{PSR}[n] = \text{TP}_{3s}[n] - S[n]$$

* **Identical windows:** both terms cover bins $n-29 .. n$, i.e. $[t_n - 3\text{ s}, t_n)$; peaks to within the sub-sample residual of §3.3. The K-weighting filter's own phase response is part of the BS.1770 loudness definition and is not compensated. Verified by ADA-ALIGN.
* **Statistics gate:** bin $n$ contributes to PSR statistics only if $n \ge 29$, $S[n] \ge -70$ LUFS and $S[n] \ge \Gamma_{r,\text{LRA}}$ (same gates as LRA), and the PSR is finite (a window holding only K-filter tail energy but no peak has $\text{TP}_{3s} = -\infty$). Gated bins remain in the time series as NaN (gaps in plots).
* **Zones (heuristic, not standardized):** Green $\ge 12$ dB · Yellow $[10, 12)$ · Orange $[8, 10)$ · Red $< 8$.

### 3.8 PLR (Peak-to-Loudness Ratio)
$\text{PLR} = 20\log_{10}\text{TP}_{\max} - I$. `n/a` if $I$ is `n/a`.

### 3.9 Peak density near the ceiling
Share of PSR-gated bins (§3.7) whose bin peak is within 0.5 dB of the file's maximum:
$$100 \cdot \frac{|\{n \in \text{gated} : P[n] \ge \text{TP}_{\max,\text{lin}} \cdot 10^{-0.5/20}\}|}{|\text{gated}|}\ \%$$
Brickwall limiting and clipping stack peaks at the ceiling and push this towards 100 %; open material stays low. `n/a` when no bin passes the gate or the file is digital silence.

### 3.10 Report fields (per file)
$I$, LRA, $\text{TP}_{\max}$ (dBTP), PLR, Max M, Max S, Min / Mean / P10 PSR (Mean = arithmetic mean of gated dB values; P10 with the §3.6 index rule), peak density near the ceiling (§3.9), percentage of gated duration per PSR zone, duration, sample rate, channels, layout (name and role list, §3.11), $L$. Undefined values are `null` in JSON and empty in CSV.

### 3.11 Channel layouts
A **layout** is an ordered list of channel roles, one per input channel. Roles: `L`, `R`, `C`, `LFE`, `Ls`, `Rs` (left / right surround), `Lb`, `Rb` (left / right rear).

| Layout | Channels | Roles in file order |
|---|---|---|
| mono | 1 | `C` (G = 1.0) |
| stereo | 2 | `L R` |
| 5.0 | 5 | `L R C Ls Rs` (the order of the EBU case-6 file) |
| 5.1 | 6 | `L R C LFE Ls Rs` (WAV order; the masks `0x3F` (back pair) and `0x60F` (side pair) both map here, the pair taking the roles `Ls Rs`) |
| 7.1 | 8 | `L R C LFE Lb Rb Ls Rs` (WAV order, mask `0x63F`) |

* **Weights:** $G = 1.0$ for `L`, `R`, `C`; $G = 1.41$ (+1.5 dB) for `Ls`, `Rs`, `Lb`, `Rb`; the `LFE` is excluded from the loudness sum (BS.1770). The LFE is not K-weighted and does not enter $E_i$.
* **True peak:** the maximum over all channels, the LFE included (§3.3). A peak in the LFE can clip a downstream converter even though it does not count toward loudness. The LFE influences only the true-peak-based fields ($\text{TP}$, PSR, PLR, clipping), never a loudness field (ADA-LFE).
* **Resolution order:** (1) a layout supplied by the user (`--layout NAME` or an explicit role list such as `L,R,C,LFE,Ls,Rs`; in the GUI a layout selector) overrides everything; (2) the container's channel mask or layout where it has one (WAVE_FORMAT_EXTENSIBLE `dwChannelMask`, FLAC / Ogg channel mask, PyAV layout, host bus layout, WASAPI speaker mask); (3) otherwise a multichannel source is **rejected**, and the error names the most plausible layout for the channel count (5 → 5.0, 6 → 5.1, 8 → 7.1; no suggestion for other counts). The suggestion is only a proposal: it is applied once the user passes it via `--layout` or confirms it in the GUI. The role list is always shown with the result so a wrong mapping is visible.
* **Validation:** the role count must equal the channel count, roles must be unique, and a layout needs at least one non-LFE channel; otherwise it is rejected with an explicit error.
* Mono and stereo files need no layout and behave exactly as before (ADA-MONO, all T3341 / T3342 rows).

---

## 4. Stage 2 Engine & Real-Time Constraints
* **Engine:** `DynamicsEngine` implements §3 directly: biquad K-filter (double state), `TruePeakDetector` with the identical Annex 2 table and $D$, sub-bin energy accumulators, a 300-slot sub-bin energy ring (3 s), a 30-slot bin peak ring.
* **$I$ and LRA without unbounded storage:** fixed histograms allocated in `prepare()`, 0.01 LU bins from −70 to +10 LUFS (8000 bins). Each bin holds a count and the sum of linear powers, so the relative-gate means are exact except for the one bin containing the threshold. $I$ uses 400 ms block powers, LRA uses 10 Hz $S$ values. Percentiles are read from the cumulative count. This bounds memory for arbitrarily long sessions and keeps ADA-PARITY within 0.02 / 0.05 LU.
* **Grid alignment:** `processBlock()` tracks the absolute sample position with its own counter, independent of host block size. Host buffers are split at sub-bin boundaries $uh$ (M/S sub-grid read-outs; $M$, $S$ for bin $n$ latched at $(n+1)H$) and at $(n+1)H + D$, where $P[n]$ closes and the `DynamicsMetrics` frame for bin $n$ is emitted. Reported plugin latency: 0 samples (the audio passes through undelayed; each frame is emitted $D$ samples after its bin boundary).
* **Reset:** requested via an atomic flag from the UI; executed by the audio thread at the next bin boundary by zeroing filter states, accumulators, rings, histograms and the sample counter (no allocation, no locks, no thread signalling). Clears $I$, LRA, $\text{TP}_{\max}$, PLR, PSR statistics and the history graph. Triggers: Reset button; a host position jump (option `resetOnPlay`, default on), decided by `ada::TransportTracker` (`src/engine/TransportTracker.h`, tests in `tests/cpp/test_transport.cpp`) from `getPlayHead()` in `processBlock()`: playback starting more than 0.25 s after or more than 2 s before where the measured audio last ended, or a jump of more than 0.25 s while playing. Resuming up to 2 s *before* that point is a continuation: hosts that process ahead of playback (Cubase ASIO-Guard) restart from the audible position, and the audio already measured is skipped until playback passes the point where measuring stopped, so nothing is counted twice. (Known limitation: pausing within the host's look-ahead of a cycle end, when the plugin has already processed the wrap to the loop start, still starts a new measurement.) Pausing and resuming in place, cycle/loop laps (a jump that lands within 0.25 s of the loop start while cycle is on) and hosts without position information continue the measurement; any other jump with cycle on starts a new one. A host deactivating and reactivating the plugin (`prepareToPlay` again) with unchanged sample rate and channel count also continues; only changed settings start a new measurement. The editor shows the reason for the last automatic restart.
* **Freeze while stopped:** when the host reports a stopped transport, `processBlock()` skips the engine (option `freezeWhenStopped`, default on), so the display keeps the last values instead of measuring silence; audio passes through unchanged. The standalone app ignores the transport (JUCE's standalone wrapper reports one that never plays) and measures whenever live analysis is on.
* **Display:** loudness readouts below the −70 LUFS absolute gate show "< −70"; every readout explains itself in a hover tooltip.
* **Telemetry:** fixed-capacity single-producer/single-consumer FIFO (`juce::AbstractFifo` + `std::array`); on overflow the frame is dropped and an atomic counter incremented.
* **Frame content:** each frame carries the **raw** PSR (TP 3 s − S, NaN only for $n < 29$) together with the running LRA relative gate, the per-bin maxima of the 10 ms M and S read-outs, and a reset epoch. The §3.7 statistics gate is applied by the consumer: live displays use the running gate, offline summaries use the final gate (`ada::summarize`), which reproduces Stage 1 exactly.
* **History:** a pre-allocated ring of 54 000 frames (90 min at 10 Hz) on the message thread, owned by the processor so it keeps filling while the editor is closed; older frames are overwritten. It also keeps the live PSR statistics since reset (minimum, P10 from a 0.1 dB histogram, zone shares), which therefore cover the whole measurement even beyond 90 minutes.
* **Live analysis switch** (`liveAnalysis`, default on): when off, `processBlock()` keeps tracking the transport but does not feed the engine. **Standalone output** ("Play input to output", off at every start and not saved, so a restart never begins with feedback): the standalone app clears its output unless the input is to be played, so it can measure the input without feedback; a plugin in a DAW always passes audio through unchanged.
* **Delivery targets:** `ada::deliveryTargets` / `ada::evaluateTarget` (`src/engine/DeliveryTargets.h`) compare the integrated loudness and max true peak of the current view with a chosen target (streaming: deviations are warnings, the service turns louder tracks down; broadcast: deviations outside the tolerance are problems). Spotify's stricter −2 dBTP ceiling applies to masters louder than −14 LUFS. The plugin judges the values as displayed (rounded to 0.1 dB); limits are inclusive. The selection (by preset name) and custom values are saved with the plugin state.
* **Windows output capture** (standalone app on Windows): `ada::OutputMonitor` (`src/plugin/OutputMonitor.cpp`) opens a render endpoint with WASAPI in shared loopback mode, converts the mix (float32 / PCM 16, 24, 32 bit; the endpoint's speaker mask gives the §3.11 layout) with `ada::interleavedToPlanar` and feeds its own `DynamicsEngine` on a capture thread; frames reach the message thread through a lock-free FIFO and fill a separate 90-minute history. While the device plays nothing Windows delivers no packets; silence is then filled in from the clock so the time axis keeps running. ASIO playback bypasses the Windows mixer and is not captured.
* **Channels and layouts:** `DynamicsEngine::prepare(sampleRate, layout)` takes the §3.11 layout; per-channel weights and the LFE (peak only) are fixed at `prepare()` (`maxChannels` = 8, state arrays fixed-size, no allocation in `process()`). Bus layouts offered by the plugin: mono, stereo, 5.0, 5.1, 7.1. A layout the host cannot report is never guessed on the audio thread; the editor shows the channel count with a layout selector (default: the §3.11 suggestion, marked unconfirmed until the user accepts it).
* **Offline file analysis** (standalone app and plugin): a separate `DynamicsEngine` on a background thread decodes the file with JUCE's readers at full speed. Frames reach the message thread through a lock-free FIFO (the worker waits when it is full) and fill a per-file history (sized from `lengthInSamples` + 5 %, since the length is an estimate for compressed formats; the graph keeps at most 6 h) plus a complete frame list, from which `ada::summarize` computes the file's report fields exactly as Stage 1 does. Mono, stereo and surround files at a multiple of 100 Hz are accepted; a multichannel file without a channel mask asks for a layout (§3.11).

---

## 5. Normative References
* **ITU-R BS.1770-5** (11/2023): Algorithms to measure audio programme loudness and true-peak audio level. (K-weighting and Annex 2 filter identical to BS.1770-4.)
* **ITU-R BS.2217**: Compliance material for the verification of loudness and true-peak meters (optional additional test files).
* **EBU R 128** (v4.0, 2020): Loudness normalisation and permitted maximum level of audio signals.
* **EBU Tech 3341** (V4, 11/2023): Loudness Metering: 'EBU Mode' metering to supplement EBU R 128.
* **EBU Tech 3342** (V4): Loudness Range: a measure to supplement EBU R 128 loudness normalisation.
