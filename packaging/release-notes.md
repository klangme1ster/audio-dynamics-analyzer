## What's new in 0.2.0

**Surround analysis: 5.0, 5.1 and 7.1.** Channels are weighted as BS.1770 prescribes (surround channels +1.5 dB, never folded down). The LFE is left out of the loudness but counts for the true peak.

- **Plugin:** accepts 5.0, 5.1 and 7.1 buses; the source line shows the layout, and a layout selector appears for 3+ channels.
- **Files:** the layout comes from the WAV channel mask or the M4A channel configuration. A multichannel file without one is not guessed: the app suggests a layout and you confirm or edit it. `ada-scan --layout 5.1` does the same.
- **Output view:** Windows devices with a 5.0, 5.1 or 7.1 speaker setup are measured in full.
- Mono and stereo results are unchanged.

## Download

**`AudioDynamicsAnalyzer-…-windows-x64.zip`** (below, under *Assets*) contains:

- **Audio Dynamics Analyzer.vst3**: the plugin. Copy the whole folder to `C:\Program Files\Common Files\VST3` and rescan plugins in your DAW.
- **Audio Dynamics Analyzer.exe**: the standalone app. No installation needed.
- **INSTALL.txt** with these steps.

Windows 10/11, 64-bit. Nothing else needs to be installed (on Windows "N" editions, M4A/AAC files need the Media Feature Pack).

**Windows warning:** the files are not code-signed. On first start Windows may show *"Windows protected your PC"*: click *More info → Run anyway*.

**How to read the values:** see [USAGE.md](https://github.com/klangme1ster/audio-dynamics-analyzer/blob/main/USAGE.md), including what good values look like.

**License:** ADA's own code is MIT. The binaries are built with the open-source edition of JUCE and are distributed under the GNU AGPLv3; the full source is this repository.
