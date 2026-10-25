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
* Formats: WAV, FLAC, AIFF, OGG and MP3 (through libsndfile), plus M4A (AAC or ALAC) and raw AAC (through PyAV, which bundles FFmpeg; no separate install needed).
* Lossy files are measured as decoded. AAC and MP3 encoding typically raises the true peak by up to about 1 dB compared with the master; that is real and is why streaming services ask for −1 dBTP.
* Channels: mono or stereo.
* Sample rates: any multiple of 100 Hz, such as 32, 44.1, 48, 88.2, 96, 176.4 or 192 kHz.

Other channel counts or sample rates are reported as errors and skipped.

### How channels are measured

The same in `ada-scan`, the plugin and the standalone app:
* **Stereo is not folded down to mono.** As BS.1770 prescribes, left and right are K-weighted separately, and their *powers* are added (weight 1.0 each). A sum to mono, (L+R)/2, would read out-of-phase content such as wide reverbs or stereo synths too quiet.
* **True peak** is oversampled per channel; the higher of the two counts. PSR and PLR use these two values, so they are stereo-correct as well.
* **Mono** files count as one channel with weight 1.0. They are not duplicated to dual-mono (that would read 3 dB louder): a −23 dBFS mono sine reads −26 LUFS, the same sine on both channels of a stereo file −23 LUFS.
* **Surround (5.1, 7.1, ...) is not supported yet.** It would need the BS.1770 channel weights (1.41 for the surround channels, LFE excluded). Surround files are rejected; the standalone app's *Output* view measures surround devices on their front left and right channels only.

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
| `--plot-width PX` | Plot width in pixels (default 2160) |
| `--plot-aspect W:H` | Plot aspect ratio (default `16:10`, giving 2160 × 1350) |

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
* `Zone (P10)` shows the zone of the 10th-percentile PSR, which describes the most compressed parts of the track.
* `Ceil %` is the share of time whose peaks come within 0.5 dB of the track's maximum. High values point to brickwall limiting or clipping.
* Files that can't be read appear as `error`, with the reason listed below the table. They don't stop the scan.

**Exit code:** `0` on success. `1` if no audio was found, or if every file failed.

---

## `ada-plot`: single-file plots

```bash
ada-plot track.flac other.wav -o plots --format png --width 2160 --aspect 16:10
```

The layout is fixed and `--width` only sets the resolution, so a 1200 px plot is a smaller copy of a 2160 px one. Each plot has six parts:

* **Loudness and peak:** short-term loudness (LUFS) and the 3-second true peak (dBTP) on a shared dB axis.
* **Peak-to-short-term ratio:** PSR over time, drawn over the labelled zone bands. A dashed line marks the PSR P10 value (the level the track drops below 10 % of the time), continued through the distribution beside it. Gaps are periods excluded from the statistics, such as the first 3 seconds and silence.
* **Distribution:** the percentage of the track's measured duration in each 1 dB PSR band.
* **Loudness vs. PSR:** a density map. Each cell shows how much of the track sits at that combination of short-term loudness and PSR. In a limited master the cloud slopes down as loudness rises and then flattens at a PSR floor set by the limiter. Open material keeps its PSR across loudness.
* **Peaks relative to the ceiling:** a histogram of each 100 ms peak, measured relative to the track's maximum true peak. Limiting stacks peaks in the shaded 0.5 dB band, while dynamic material spreads them out. The panel title gives the share of time inside the band.
* **Summary:** integrated loudness, LRA, maximum true peak, PLR, PSR minimum / 10th percentile / mean, and the share of time with peaks near the ceiling.

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
| `peak_near_ceiling_pct` | % | Share of the measured duration with peaks within 0.5 dB of the max true peak |
| `plot` | — | Plot path, if `--plot` was given |
| `error` | — | Reason the file could not be analysed (only on failed rows) |

### Reading the results

* **PSR** describes the music moment to moment. Limiting is audible as a low PSR: below 8 dB for long stretches means the transients have been flattened.
* **PLR** is the track-level equivalent: peak level compared with average loudness. Loud modern masters often sit at 8–10 dB, typical modern masters at 10–14 dB, and dynamic recordings at 14 dB or more.
* **PSR P10** is usually more informative than the minimum. It describes the most compressed 10 % of the track without being skewed by a single moment.
* **Streaming targets:** Spotify and YouTube normalise to about −14 LUFS, and Apple Music to about −16 LUFS, typically with a −1 dBTP true-peak ceiling. A track much louder than its target is turned down on playback, so whatever was given up in PSR to make it loud buys nothing.

---

## Plugin (VST3 / standalone)

Build it as described in the [README](README.md#building-the-plugin-work-in-progress), then copy `Audio Dynamics Analyzer.vst3` to `C:\Program Files\Common Files\VST3` (Windows) and insert it on a track or the master bus. It passes audio through unchanged.

**Readouts.** Hover over any value for an explanation.

| Readout | Meaning |
|---|---|
| PSR | Current peak-to-short-term ratio over the last 3 s. Blank during the first 3 s and in silence or very quiet passages |
| PSR min | Lowest PSR since the last reset |
| Short-term / Momentary | Loudness over the last 3 s / 400 ms. Below the −70 LUFS gate they read "< −70" |
| Integrated | Gated average loudness since the last reset |
| LRA | Loudness range since the last reset |
| True peak 3 s | Highest true peak in the last 3 s; shows CLIP above the clip threshold (default 0 dBTP, host-automatable parameter) |
| Max true peak | Highest true peak since the last reset |
| PLR | Max true peak minus integrated loudness |

**Graphs.**
* **History** (centre): short-term loudness (blue) and the 3-second true peak (orange) on top, PSR over the coloured zone bands below, on one time axis. Scroll the mouse wheel to zoom (10 s up to 90 min live, up to 6 h for a file), drag to scroll back in time, double-click to follow live again. Hovering shows the exact values at that moment. The history keeps recording while the plugin window is closed.
* **PSR meter** (right): the current PSR as a bar in its zone colour. The solid line marks the lowest PSR since reset, the dashed line P10, the PSR the track stays below 10 % of the time.
* **Zone bar** (bottom): how much of the measured time since reset fell into each PSR zone. A large red or orange share means most of the track is heavily limited.

### What good values look like

Only the broadcast limits (EBU R128, ATSC A/85) and Spotify's normalisation level and true-peak advice are published specifications; the other streaming figures are widely reported measurements. The remaining ranges are practical guidance, and they depend on the genre:
* Electronic music, metal and modern pop are often deliberately dense and sit lower on PSR and PLR.
* Jazz, acoustic and classical recordings sit higher.

Compare a mix with good references in the same genre rather than chasing a single number.

**Micro-dynamics: how hard the transients are limited**

| Value | Healthy | Not so healthy | Why |
|---|---|---|---|
| **PSR** (now) | ≥ 12 dB open, 10–12 dB punchy commercial | 8–10 dB heavy limiting; < 8 dB over-limited | PSR is how far the peaks rise above the average loudness of the same 3 seconds. A limiter shaves the peaks off to raise the average, which shrinks the gap. Below about 8 dB, drum hits and attacks are flattened, distortion becomes likely, and the mix tends to sound loud but small and tiring. |
| **PSR min** | ≥ 8 dB | < 8 dB (red zone) | The most compressed 3 seconds, usually the loudest chorus or drop. A short dip is normal there; a very low value means the climax is the most squashed part, so it hits less hard than it should. |
| **P10** (dashed line on the meter) | ≥ 10 dB (green or yellow) | < 10 dB (orange or red) | The PSR the track stays below for 10 % of the measured time. Unlike the minimum, one extreme moment doesn't move it, so it shows whether limiting dominates large parts of the track rather than a single moment. |
| **Zone bar** | Mostly green and yellow | More than about a third red | It shows how much of the track is affected. A few seconds in orange or red during the loudest parts is common; red over most of the bar means the whole track is limited hard. |

**Macro-dynamics and loudness**

| Value | Healthy | Not so healthy | Why |
|---|---|---|---|
| **Integrated** | Close to your delivery target: about −14 LUFS for Spotify, YouTube and Tidal; about −16 LUFS for Apple Music; −23 LUFS for EBU R128 broadcast; −24 LKFS for US broadcast (ATSC A/85) | Much louder than the target, e.g. −8 LUFS for streaming | Streaming services turn loud tracks down to their target. A −8 LUFS master plays back about 6 dB quieter, as loud as a −14 LUFS master, but it has lost its transients in the process. With normalisation on (the default on most services), being louder gains nothing. |
| **Short-term / Momentary** | No fixed target; they follow the music | Loud sections sitting on one value with no movement | These show the loudness right now. They are useful for comparing sections, for example chorus against verse; a flat line across all sections means the arrangement's dynamics were compressed away. |
| **LRA** | Pop, rock and EDM: about 3–8 LU; jazz, acoustic and film: 8–15 LU; classical: 10–20 LU or more | Below about 3 LU for acoustic music; above about 15–20 LU for pop or for TV and radio | LRA is the spread between quiet and loud passages (verse vs. chorus, scene vs. scene). Very low values on acoustic material point to heavy bus compression. Very high values make listeners reach for the volume control, and quiet parts get lost in a car or on headphones on the go. |
| **PLR** | ≥ 14 dB dynamic; 10–14 dB typical modern master | 8–10 dB loud; < 8 dB crushed | PLR is the whole track's headroom between its highest peak and its average loudness. It is the track-level counterpart to PSR. |

**Peaks**

| Value | Healthy | Not so healthy | Why |
|---|---|---|---|
| **Max true peak** | ≤ −1 dBTP for streaming and broadcast (EBU R128, Spotify); ≤ −2 dBTP for masters louder than −14 LUFS (Spotify) and for US broadcast (ATSC A/85) | Above −1 dBTP; anything above 0 dBTP clips | Lossy encoding (AAC, MP3, Ogg) changes the waveform and adds overshoot, typically up to about 1 dB, and more at low bitrates. In a quick test during development, a limited master went from −0.4 to +0.7 dBTP after AAC encoding at 256 kbit/s. Without headroom, that overshoot clips in the listener's decoder or DAC. |
| **True peak 3 s** | Same limits as above, on the loudest passages | CLIP indicator lit | This is the peak half of PSR. It also shows where in the song the peaks come close to the ceiling. |

**Transport behaviour (inside a DAW).**
* **Freeze while stopped** (default on): when you stop or pause, the values stay on screen instead of measuring the silence. A "Frozen" badge shows this state. Turn it off to meter live input while the transport is stopped.
* **New measurement when playback jumps** (default on): pausing and continuing from the same spot keeps the measurement running (also when the host resumes slightly earlier than where it processed up to, as Cubase does with ASIO-Guard; the overlap is not counted twice), and so does cycle/loop playback. Jumping elsewhere, for example back to the song start (also while playing), begins a new measurement. The reason for the last automatic restart is shown next to the switches; hover over it for details.
* **Reset** button: start a new measurement at any time (Live and Output views).

**Views.** The switch at the top left changes what the window shows: **Live** (the audio input), **Output** (what a Windows output device plays; standalone app on Windows only) and **File**. All of them keep running in the background: the live history keeps recording while you look at a file or an output, and so on. **File** becomes available once a file has been opened.

**Live analysis** (switch at the top, live view): turns measuring of the input on and off. When off, the display keeps the last values and shows "Live analysis stopped"; nothing is measured.

**Delivery target** (corner under the PSR meter). Choose where the track is going and ADA checks it in every view (live, output and file):

| Target | Loudness | True-peak ceiling | Verdict |
|---|---|---|---|
| Spotify | −14 LUFS (±1 LU counts as on target) | −1 dBTP; −2 dBTP for masters louder than −14 LUFS | Louder: "will be turned down by X dB"; quieter: "plays quieter than other tracks" |
| Apple Music | −16 LUFS | −1 dBTP | as above |
| YouTube | −14 LUFS | −1 dBTP | as above |
| Tidal | −14 LUFS | −1 dBTP | as above |
| EBU R 128 broadcast | −23 LUFS ±0.5 LU | −1 dBTP | Outside the tolerance or above the ceiling is a problem |
| ATSC A/85 (US TV) | −24 LKFS ±2 dB | −2 dBTP | as above |
| Custom | your value (−40 to 0 LUFS) | your value (−10 to 0 dBTP) | like streaming; **Edit custom target...** in the list changes the values |

* Each verdict has an icon (✓ on target, ! warning, ✕ problem) and a short line, e.g. "Loudness: 5.4 LU too loud" or "Peak: 0.7 dB too high". Hover over the box for the full sentences, e.g. "−8.6 LUFS: 5.4 LU louder than −14.0, will be turned down by 5.4 dB". Peaks above 0 dBTP are flagged as clipping after encoding. The check uses the values as displayed (rounded to 0.1), so a −1.0 dBTP readout passes a −1 dBTP ceiling.
* The history graph shows the loudness target and the ceiling that applies as dashed lines.
* The choice (and custom values) is saved with the plugin or the standalone app, and follows when the host restores a preset.
* Platform figures change over time. Only Spotify's figures and the broadcast standards are published by their owners; the others are widely reported measurements (see [What good values look like](#what-good-values-look-like)).

**Window size.** The window remembers its size: the standalone app between sessions, the plugin inside a DAW with the project.

**Standalone app.**
* It has no transport, so it measures its audio input whenever **Live analysis** is on (pick the device under *Options*).
* It measures the input *without* playing it: the output stays silent unless you turn on **Play input to output**, which is off at every start. That avoids feedback, so JUCE's feedback protection (input mute) is switched off; leave "Mute audio input" in the *Options* audio settings unticked, or the analysis receives silence. With speakers instead of headphones, turning playback on can cause feedback.
* **Output view: measuring what Windows plays.** Pick an output device in the list (*Default output* follows the Windows setting) and ADA measures everything that device plays, for example Spotify, a browser or a media player. It starts measuring the default output the first time you open the view; **Output analysis** turns it off and on, and **Reset** starts a new measurement. Pauses in playback are measured as silence, so the time axis keeps running.
  * This uses Windows' loopback capture, which sees the audio after the Windows mixer, including per-application volume. Programs that play through ASIO (e.g. Cubase) bypass the Windows mixer and cannot be measured this way: use the plugin on Cubase's master bus, or your interface's loopback input in the **Live** view.
  * Surround devices are measured on their front left and right channels only (no surround loudness, see [How channels are measured](#how-channels-are-measured)). The device must run at a sample rate that is a multiple of 100 Hz (44.1, 48, 96 kHz, ...).
  * Start the app directly in this view with `"Audio Dynamics Analyzer.exe" --output`.
* To measure what another program plays through ASIO, choose your audio interface's loopback channel (or a virtual audio cable) as the input under *Options* and use the **Live** view.
* The window remembers its position between sessions. The title bar shows what you are looking at, e.g. "Audio Dynamics Analyzer – song.flac (3:45)", "– output: Default output (Speakers)" or "– live input".

**Analysing a file.** Drop an audio file anywhere on the window, or click **Open file...** (WAV, AIFF, FLAC, Ogg, MP3, WMA, and on Windows M4A (AAC or Apple Lossless) and raw AAC). This works in the standalone app and inside the plugin, for example to check a reference track while mixing. The file is analysed in the background at several hundred times real time; the graph fills as it goes and then zooms out to the whole file.
* The readouts switch to the file's results: PSR mean, PSR min and PSR P10, max short-term (Max S) and max momentary (Max M) loudness, integrated loudness, LRA, max true peak and PLR. For WAV, AIFF and FLAC these are the same values `ada-scan` reports; for MP3 they can differ slightly (different decoder). For M4A see below.

**M4A files.** The plugin and the standalone app decode M4A with Windows' own decoder (Media Foundation); `ada-scan` uses FFmpeg. Nothing extra needs to be installed. What to expect when you compare the two:
* **Normally the same values.** Both remove the encoder's start-up samples ("priming", about 20–50 ms of silence that AAC encoders put at the start) as the file describes them, either in the MP4 edit list or in iTunes' gapless tag (`iTunSMPB`). In tests with AAC at 128 and 256 kbit/s, Apple Lossless and real recordings, both gave the same loudness, true peak and PSR to within 0.01 dB.
* **Peaks can differ for files coded with noise substitution (PNS).** Some encoders, notably FFmpeg's built-in AAC encoder at its default settings, store noisy parts (cymbals, snares, hiss) as "insert noise of this level here". The decoder generates that noise itself, so the exact waveform, and with it the true peak, PSR and peak density, differs between any two decoders and players, by several dB at single peaks in a test. Loudness (I, S, M, LRA) is unaffected. Apple's encoders (iTunes, Music, Logic, `afconvert`) do not normally use PNS.
* **Length can differ by a few samples** (under 1 ms in tests) when a file has no gapless information and the two decoders estimate its end differently. Files without gapless information (many phone recorders) keep the priming in both. Raw `.aac` files (ADTS, no MP4 container) have no stored length; Windows' own estimate can be seconds short, so ADA counts the AAC frames instead and gets the same length as FFmpeg.
* This describes Windows. A macOS build would read M4A through Apple's own decoder (JUCE's Core Audio reader); that is not tested in this project.
* The meter shows the PSR over the file (mean as the bar, min and P10 as markers), and the zone bar the whole file.
* Switch back with **Live**; the live history kept recording meanwhile.
* The standalone app also opens a file given on the command line, so you can use Explorer's *Open with* on an audio file: `"Audio Dynamics Analyzer.exe" song.flac`.
* Files must be mono or stereo at a sample rate that is a multiple of 100 Hz (32, 44.1, 48, 88.2, 96, 176.4, 192 kHz); others are rejected with a message.

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
