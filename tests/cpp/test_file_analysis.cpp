// File analysis through JUCE's audio readers (plan 3.6): decoding, block handling and the per-file
// summary must give the same result as feeding the samples to the engine directly.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "FileAnalysis.h"
#include "MediaFoundationFormat.h"
#include "Summary.h"
#include "TestSupport.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstring>
#include <memory>

// TestSupport.h declares these for the allocation test; this executable does not count.
namespace adatest
{
thread_local bool countAllocations = false;
std::atomic<long> allocationCount { 0 };
}

using namespace adatest;

namespace
{
juce::File tempDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ada_file_analysis_tests");
    dir.createDirectory();
    return dir;
}

void writeFile (const juce::File& file, juce::AudioFormat& format, const Signal& s, int bits)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (s.fs)
                             .withNumChannels ((int) s.channels.size())
                             .withBitsPerSample (bits);
    auto writer = format.createWriterFor (stream, options); // takes ownership of the stream
    REQUIRE (writer != nullptr);
    REQUIRE (writer->writeFromFloatArrays (s.pointers().data(), (int) s.channels.size(), (int) s.frames()));
}

Run analyseFile (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    CollectingSink sink;
    const auto final = ada::FileAnalysis::analyseReader (*reader, sink, [] { return false; });
    REQUIRE (final.has_value());
    Run r;
    r.final = *final;
    r.frames = std::move (sink.frames);
    r.summary = ada::summarize (r.frames, r.final);
    return r;
}

// Re-reads the written file into memory so both paths see identical (quantised) samples.
Signal readBack (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    Signal s { reader->sampleRate, std::vector<std::vector<float>> (reader->numChannels,
                                                                    std::vector<float> ((size_t) reader->lengthInSamples)) };
    juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);
    for (int ch = 0; ch < (int) reader->numChannels; ++ch)
        std::memcpy (s.channels[(size_t) ch].data(), buffer.getReadPointer (ch), sizeof (float) * s.frames());
    return s;
}

bool sameFrames (const std::vector<ada::DynamicsMetrics>& a, const std::vector<ada::DynamicsMetrics>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::memcmp (&a[i].momentaryLUFS, &b[i].momentaryLUFS,
                         offsetof (ada::DynamicsMetrics, isClipping) - offsetof (ada::DynamicsMetrics, momentaryLUFS)) != 0)
            return false;
    return true;
}
} // namespace

TEST_CASE ("WAV file analysis equals feeding the engine directly, and meets T3341")
{
    // 24-bit WAV of T3341 case 3 (gating) at 48 kHz, 10.05 s tail so the last bin is partial
    auto s = toneSequence (48000, { { 10, -36 }, { 60, -23 }, { 10.05, -36 } });
    const auto file = tempDir().getChildFile ("t3341-3.wav");
    juce::WavAudioFormat wav;
    writeFile (file, wav, s, 24);

    const auto fromFile = analyseFile (file);
    const auto direct = analyze (readBack (file), 4096);
    CHECK (sameFrames (fromFile.frames, direct.frames));
    CHECK (fromFile.final.frames == direct.final.frames);
    CHECK (fromFile.final.truePeakMaxDBTP == direct.final.truePeakMaxDBTP);
    REQUIRE (fromFile.summary.integrated.has_value());
    CHECK (std::abs (*fromFile.summary.integrated - (-23.0)) <= 0.1);
    CHECK (fromFile.frames.size() == 800);
    file.deleteFile();
}

TEST_CASE ("FLAC and mono files")
{
    auto s = toneSequence (44100, { { 20, -23 } }, 1);
    const auto file = tempDir().getChildFile ("mono.flac");
    juce::FlacAudioFormat flac;
    writeFile (file, flac, s, 24);
    const auto r = analyseFile (file);
    REQUIRE (r.summary.integrated.has_value());
    CHECK (std::abs (*r.summary.integrated - (-26.0)) <= 0.1); // ADA-MONO: no dual-mono summing
    CHECK (sameFrames (r.frames, analyze (readBack (file), 4096).frames));
    file.deleteFile();
}

TEST_CASE ("Unsupported files are rejected with a message")
{
    ada::FileAnalysis fa;
    CHECK_FALSE (fa.start (tempDir().getChildFile ("does-not-exist.wav")));
    CHECK (fa.state() == ada::FileAnalysis::State::failed);
    CHECK (fa.errorText().isNotEmpty());

    // 4-channel WAV
    Signal quad { 48000.0, std::vector<std::vector<float>> (4, std::vector<float> (4800, 0.1f)) };
    const auto quadFile = tempDir().getChildFile ("quad.wav");
    juce::WavAudioFormat wav;
    writeFile (quadFile, wav, quad, 16);
    CHECK_FALSE (fa.start (quadFile));
    CHECK (fa.errorText().contains ("4 channels"));

    // 22.05 kHz is not a multiple of 100 Hz
    const auto odd = toneSequence (22050, { { 1, -20 } });
    const auto oddFile = tempDir().getChildFile ("odd-rate.wav");
    writeFile (oddFile, wav, odd, 16);
    CHECK_FALSE (fa.start (oddFile));
    CHECK (fa.errorText().contains ("22050"));

    CHECK (fa.isSupportedFile (juce::File ("x.flac")));
    CHECK (fa.isSupportedFile (juce::File ("x.WAV")));
    CHECK_FALSE (fa.isSupportedFile (juce::File ("x.txt")));
    quadFile.deleteFile();
    oddFile.deleteFile();
}

TEST_CASE ("Cancelling stops the analysis")
{
    auto s = toneSequence (48000, { { 30, -20 } });
    const auto file = tempDir().getChildFile ("cancel.wav");
    juce::WavAudioFormat wav;
    writeFile (file, wav, s, 16);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    CollectingSink sink;
    int calls = 0;
    const auto final = ada::FileAnalysis::analyseReader (*reader, sink, [&calls] { return ++calls > 3; });
    CHECK_FALSE (final.has_value());
    CHECK (sink.frames.size() < 300);
    file.deleteFile();
}

// --- M4A gapless information (Media Foundation does not apply it; MediaFoundationFormat.cpp) ----
namespace
{
// An MP4 box: 32-bit size, 4-character type, payload
juce::MemoryBlock box (const char* type, const juce::MemoryBlock& payload)
{
    juce::MemoryOutputStream out;
    out.writeIntBigEndian ((int) payload.getSize() + 8);
    out.write (type, 4);
    out.write (payload.getData(), payload.getSize());
    return out.getMemoryBlock();
}

juce::MemoryBlock concat (std::initializer_list<juce::MemoryBlock> parts)
{
    juce::MemoryBlock all;
    for (const auto& p : parts)
        all.append (p.getData(), p.getSize());
    return all;
}

juce::MemoryBlock bytes (std::initializer_list<int> values) // 32-bit big-endian words
{
    juce::MemoryOutputStream out;
    for (int v : values)
        out.writeIntBigEndian (v);
    return out.getMemoryBlock();
}

juce::MemoryBlock text (const char* s) { return juce::MemoryBlock (s, std::strlen (s)); }

juce::MemoryBlock fourcc (const char* s) { return juce::MemoryBlock (s, 4); }

// version 0 mvhd / mdhd: version+flags, creation, modification, time scale, duration
juce::MemoryBlock timeHeader (const char* type, int scale, int duration)
{
    return box (type, bytes ({ 0, 0, 0, scale, duration }));
}

juce::MemoryBlock audioTrack (int mediaScale, std::optional<std::pair<int, int>> edit /* duration, media time */)
{
    const auto hdlr = box ("hdlr", concat ({ bytes ({ 0, 0 }), fourcc ("soun"), bytes ({ 0, 0, 0 }), text ("x") }));
    const auto mdia = box ("mdia", concat ({ timeHeader ("mdhd", mediaScale, 0), hdlr }));
    if (! edit)
        return box ("trak", mdia);
    const auto elst = box ("elst", bytes ({ 0, 1, edit->first, edit->second, 0x00010000 }));
    return box ("trak", concat ({ box ("edts", elst), mdia }));
}

juce::MemoryBlock iTunSmpb (const char* value, bool quickTimeMeta = false)
{
    const auto name = box ("name", concat ({ bytes ({ 0 }), text ("iTunSMPB") }));
    const auto data = box ("data", concat ({ bytes ({ 1, 0 }), text (value) }));
    const auto item = box ("----", concat ({ box ("mean", concat ({ bytes ({ 0 }), text ("com.apple.iTunes") })), name, data }));
    // ISO meta: version/flags, then children; QuickTime-style (some Apple files): children only
    const auto hdlr = box ("hdlr", concat ({ bytes ({ 0, 0 }), fourcc ("mdir"), bytes ({ 0, 0, 0 }), text ("x") }));
    const auto children = concat ({ hdlr, box ("ilst", item) });
    const auto meta = box ("meta", quickTimeMeta ? children : concat ({ bytes ({ 0 }), children }));
    return box ("udta", meta);
}

ada::Mp4Timing timingOf (const juce::MemoryBlock& file, double rate)
{
    juce::MemoryInputStream in (file, false);
    return ada::readMp4Timing (in, rate);
}
} // namespace

TEST_CASE ("M4A gapless info: edit list, iTunSMPB, none")
{
    const auto ftyp = box ("ftyp", concat ({ fourcc ("M4A "), bytes ({ 0 }) }));
    const auto mdat = box ("mdat", text ("audio"));

    // edit list (FFmpeg's AAC encoder): 1024 priming samples, 576 000 samples long
    auto t = timingOf (concat ({ ftyp, mdat, box ("moov", concat ({ timeHeader ("mvhd", 1000, 12000),
                                                                   audioTrack (48000, std::pair { 12000, 1024 }) })) }),
                       48000.0);
    CHECK (t.primingSamples == 1024);
    REQUIRE (t.lengthSamples.has_value());
    CHECK (*t.lengthSamples == 576000);

    // iTunes: no edit list, iTunSMPB with 2112 (0x840) priming and 44 100 (0xAC44) samples
    t = timingOf (concat ({ ftyp, box ("moov", concat ({ timeHeader ("mvhd", 44100, 46400), audioTrack (44100, std::nullopt),
                                                        iTunSmpb (" 00000000 00000840 000001BC 000000000000AC44 00000000") })),
                            mdat }),
                  44100.0);
    CHECK (t.primingSamples == 2112);
    REQUIRE (t.lengthSamples.has_value());
    CHECK (*t.lengthSamples == 44100);

    // the same in a QuickTime-style meta box
    t = timingOf (concat ({ ftyp, box ("moov", concat ({ timeHeader ("mvhd", 44100, 46400), audioTrack (44100, std::nullopt),
                                                        iTunSmpb (" 00000000 00000840 000001BC 000000000000AC44 00000000", true) })) }),
                  44100.0);
    CHECK (t.primingSamples == 2112);
    CHECK (t.lengthSamples == std::optional<juce::int64> (44100));

    // priming beyond FFmpeg's limit (16384) is not believed
    t = timingOf (concat ({ ftyp, box ("moov", concat ({ timeHeader ("mvhd", 44100, 46400), audioTrack (44100, std::nullopt),
                                                        iTunSmpb (" 00000000 00010000 000001BC 000000000000AC44") })) }),
                  44100.0);
    CHECK (t.primingSamples == 0);

    // an edit list whose entry is cut short is ignored, not read from the next box
    {
        const auto hdlr = box ("hdlr", concat ({ bytes ({ 0, 0 }), fourcc ("soun"), bytes ({ 0, 0, 0 }), text ("x") }));
        const auto mdia = box ("mdia", concat ({ timeHeader ("mdhd", 48000, 0), hdlr }));
        const auto elst = box ("elst", bytes ({ 0, 1, 12000 })); // count 1, entry truncated after the duration
        const auto trak = box ("trak", concat ({ box ("edts", elst), mdia }));
        t = timingOf (concat ({ ftyp, box ("moov", concat ({ timeHeader ("mvhd", 1000, 12000), trak })) }), 48000.0);
        CHECK (t.primingSamples == 0);
        CHECK (! t.lengthSamples.has_value());
    }

    // neither (e.g. many phone recorders): nothing to trim
    t = timingOf (concat ({ ftyp, box ("moov", concat ({ timeHeader ("mvhd", 44100, 44100), audioTrack (44100, std::nullopt) })) }),
                  44100.0);
    CHECK (t.primingSamples == 0);
    CHECK (! t.lengthSamples.has_value());

    // not an MP4 (raw ADTS AAC) and truncated files: nothing, no crash
    CHECK (timingOf (concat ({ bytes ({ (int) 0xFFF15080 }), text ("adts") }), 48000.0).primingSamples == 0);
    auto truncated = concat ({ ftyp, box ("moov", concat ({ timeHeader ("mvhd", 1000, 12000), audioTrack (48000, std::pair { 12000, 1024 }) })) });
    truncated.setSize (truncated.getSize() - 20);
    CHECK (! timingOf (truncated, 48000.0).lengthSamples.has_value());
}

TEST_CASE ("M4A files are offered by the file analysis on Windows")
{
    ada::FileAnalysis analysis;
#if JUCE_WINDOWS
    if (! ada::mediaFoundationAvailable())
        return; // e.g. Windows N without the Media Feature Pack: ADA runs, without M4A
    CHECK (analysis.isSupportedFile (juce::File ("song.m4a")));
    CHECK (analysis.isSupportedFile (juce::File ("song.AAC")));
    CHECK (analysis.wildcard().contains ("*.m4a"));
#endif
    CHECK (! analysis.isSupportedFile (juce::File ("notes.txt")));
}

namespace
{
// One ADTS frame header (7 bytes, no CRC) plus payload: AAC LC, given rate index, stereo
juce::MemoryBlock adtsFrame (int rateIndex, int payload, int blocks = 1)
{
    const int length = 7 + payload;
    juce::uint8 h[7] = { 0xff, 0xf1, (juce::uint8) ((1 << 6) | (rateIndex << 2)), (juce::uint8) ((2 << 6) | ((length >> 11) & 0x03)),
                         (juce::uint8) ((length >> 3) & 0xff), (juce::uint8) (((length & 0x07) << 5) | 0x1f), (juce::uint8) (0xfc | (blocks - 1)) };
    juce::MemoryBlock frame (h, 7);
    frame.setSize ((size_t) length, true);
    return frame;
}

std::optional<juce::int64> adtsSamples (const juce::MemoryBlock& file, double rate)
{
    juce::MemoryInputStream in (file, false);
    return ada::countAdtsSamples (in, rate);
}
} // namespace

TEST_CASE ("Raw AAC (ADTS) length is counted from the frame headers")
{
    // 3 frames at 44.1 kHz (rate index 4), one of them carrying 2 raw blocks
    const auto stream = concat ({ adtsFrame (4, 300), adtsFrame (4, 280, 2), adtsFrame (4, 310) });
    CHECK (adtsSamples (stream, 44100.0) == std::optional<juce::int64> (4 * 1024));

    // HE-AAC: the decoder runs at twice the header's rate, 2048 samples per frame
    CHECK (adtsSamples (concat ({ adtsFrame (6, 200), adtsFrame (6, 200) }), 48000.0) == std::optional<juce::int64> (2 * 2048));

    // ID3v2 tag in front (size 20, syncsafe) and an ID3v1 "TAG" at the end
    juce::MemoryBlock id3 ("ID3\x04\x00\x00\x00\x00\x00\x14", 10);
    id3.setSize (30, true);
    CHECK (adtsSamples (concat ({ id3, stream, text ("TAG and the rest of an ID3v1 tag") }), 44100.0)
           == std::optional<juce::int64> (4 * 1024));

    // a cut-off last frame is not counted; not ADTS at all gives nothing
    auto cut = concat ({ adtsFrame (4, 300), adtsFrame (4, 300) });
    cut.setSize (cut.getSize() - 50);
    CHECK (adtsSamples (cut, 44100.0) == std::optional<juce::int64> (1024));
    CHECK (! adtsSamples (text ("RIFF....WAVEfmt "), 44100.0).has_value());
}
