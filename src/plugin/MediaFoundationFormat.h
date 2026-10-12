#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>

namespace ada
{

// M4A / AAC reader through Windows Media Foundation (AAC and ALAC in MP4, raw ADTS AAC).
// JUCE has no AAC decoder on Windows; Media Foundation ships with Windows, so nothing extra is
// installed or licensed. Decodes to 32-bit float at the file's own rate and channel count (never
// resampled). Built for sequential reading (file analysis): reading backwards restarts the
// decoder, reading ahead decodes and discards. Only files opened from disk are supported.
//
// The decoder differs from the FFmpeg decoder used by the Python reference (spec ADA-PARITY,
// USAGE.md "M4A files"): sample values differ very slightly for AAC, and the length can differ by
// a few milliseconds where the two treat encoder priming and padding differently.
class MediaFoundationAudioFormat : public juce::AudioFormat
{
public:
    MediaFoundationAudioFormat();

    juce::Array<int> getPossibleSampleRates() override { return {}; }
    juce::Array<int> getPossibleBitDepths() override { return {}; }
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    bool isCompressed() override { return true; }

    juce::AudioFormatReader* createReaderFor (juce::InputStream* source, bool deleteStreamIfOpeningFails) override;

    // Reading only
    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>&,
                                                              const juce::AudioFormatWriterOptions&) override
    {
        return nullptr;
    }
};

// Gapless information of an MP4 / M4A file, which Media Foundation does not apply itself: the
// encoder's priming samples to drop at the start and the real length. Taken from the audio
// track's edit list (as FFmpeg does) or, without one, from iTunes' iTunSMPB tag. Values are in
// samples at `sampleRate`. Both empty for files without this information (e.g. raw ADTS AAC).
struct Mp4Timing
{
    juce::int64 primingSamples = 0;
    std::optional<juce::int64> lengthSamples;
};
Mp4Timing readMp4Timing (juce::InputStream& in, double sampleRate);

// Registers JUCE's basic formats plus, on Windows, the Media Foundation M4A / AAC reader. Used by
// the file analysis and by the command-line tools, so both read files the same way.
void registerAudioFormats (juce::AudioFormatManager& formats);

} // namespace ada
