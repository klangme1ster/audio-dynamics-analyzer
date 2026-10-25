// File analysis through JUCE's audio readers (plan 3.6): decoding, block handling and the per-file
// summary must give the same result as feeding the samples to the engine directly.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "FileAnalysis.h"
#include "MediaFoundationFormat.h"
#include "PluginLayout.h"
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

// --- Surround files (spec 3.11): the layout comes from the WAV channel mask --------------------------

namespace
{
// One tone per channel at different levels, so that a wrong role or order changes the result
Signal surroundSignal (int channels)
{
    Signal s { 48000.0, {} };
    for (int ch = 0; ch < channels; ++ch)
        s.channels.push_back (toneSequence (48000, { { 12, -20.0 - 3.0 * ch } }, 1).channels[0]);
    return s;
}

void writeSurround (const juce::File& file, const Signal& s, const juce::AudioChannelSet& set)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (s.fs)
                             .withChannelLayout (set)
                             .withBitsPerSample (24);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    REQUIRE (writer->writeFromFloatArrays (s.pointers().data(), (int) s.channels.size(), (int) s.frames()));
}

// speakers in WAV order: L R C LFE Lb Rb Ls Rs (mask 0x63F)
juce::AudioChannelSet wav71()
{
    juce::AudioChannelSet set;
    for (auto t : { juce::AudioChannelSet::left, juce::AudioChannelSet::right, juce::AudioChannelSet::centre,
                    juce::AudioChannelSet::LFE, juce::AudioChannelSet::leftSurround, juce::AudioChannelSet::rightSurround,
                    juce::AudioChannelSet::leftSurroundSide, juce::AudioChannelSet::rightSurroundSide })
        set.addChannel (t);
    return set;
}

std::unique_ptr<juce::AudioFormatReader> openReader (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    return std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file));
}
} // namespace

TEST_CASE ("Surround WAV: the layout is taken from the channel mask and analysed like the engine does")
{
    struct Case { const char* name; juce::AudioChannelSet set; ada::ChannelLayout layout; };
    for (const auto& c : { Case { "5.0", juce::AudioChannelSet::create5point0(), ada::ChannelLayout::surround50() },
                           Case { "5.1", juce::AudioChannelSet::create5point1(), ada::ChannelLayout::surround51() },
                           Case { "7.1", wav71(), ada::ChannelLayout::surround71() } })
    {
        CAPTURE (c.name);
        const auto s = surroundSignal (c.layout.count);
        const auto file = tempDir().getChildFile (juce::String ("surround-") + c.name + ".wav");
        writeSurround (file, s, c.set);

        auto reader = openReader (file);
        REQUIRE (reader != nullptr);
        const auto detected = ada::FileAnalysis::detectLayout (*reader);
        REQUIRE (detected.has_value());
        CHECK (*detected == c.layout);

        CollectingSink sink;
        const auto final = ada::FileAnalysis::analyseReader (*reader, sink, [] { return false; });
        REQUIRE (final.has_value());
        const auto direct = analyze (readBack (file), c.layout, 4096);
        CHECK (sameFrames (sink.frames, direct.frames));
        CHECK (final->truePeakMaxDBTP == direct.final.truePeakMaxDBTP);
        reader.reset();
        file.deleteFile();
    }
}

TEST_CASE ("Multichannel WAV without a layout needs one: rejected with a suggestion, accepted when supplied")
{
    const auto s = surroundSignal (6);
    const auto file = tempDir().getChildFile ("six-discrete.wav");
    writeSurround (file, s, juce::AudioChannelSet::discreteChannels (6)); // no speaker information

    auto reader = openReader (file);
    REQUIRE (reader != nullptr);
    CHECK_FALSE (ada::FileAnalysis::detectLayout (*reader).has_value());

    ada::FileAnalysis fa;
    CHECK_FALSE (fa.start (file));
    CHECK (fa.needsLayout());
    REQUIRE (fa.suggestedLayout().has_value());
    CHECK (fa.suggestedLayout()->name() == "5.1");
    CHECK (fa.errorText().contains ("5.1"));
    CHECK (fa.errorText().containsIgnoreCase ("layout"));

    // the same file with the user's choice (here the suggestion, edited: a custom order)
    const auto custom = ada::ChannelLayout::parse ("Ls,L,Rs,C,LFE,R");
    REQUIRE (custom.has_value());
    CollectingSink sink;
    const auto final = ada::FileAnalysis::analyseReader (*reader, sink, [] { return false; }, nullptr, nullptr, custom);
    REQUIRE (final.has_value());
    CHECK (sameFrames (sink.frames, analyze (readBack (file), *custom, 4096).frames));

    CHECK (fa.start (file, ada::ChannelLayout::surround51()));
    CHECK (fa.layout() == ada::ChannelLayout::surround51());
    CHECK (fa.channels() == 6);
    fa.cancel();

    // a layout that does not match the channel count is refused
    CHECK_FALSE (fa.start (file, ada::ChannelLayout::surround71()));
    CHECK (fa.errorText().contains ("8"));
    reader.reset();
    file.deleteFile();
}

TEST_CASE ("Four and seven channel files stay rejected without a suggestion")
{
    ada::FileAnalysis fa;
    for (int n : { 4, 7 })
    {
        const auto file = tempDir().getChildFile ("n" + juce::String (n) + ".wav");
        writeSurround (file, surroundSignal (n), juce::AudioChannelSet::discreteChannels (n));
        CHECK_FALSE (fa.start (file));
        CHECK (fa.errorText().contains (juce::String (n) + " channels"));
        CHECK_FALSE (fa.suggestedLayout().has_value());
        file.deleteFile();
    }
}

// --- Host bus layouts (plugin) ------------------------------------------------------------------------

TEST_CASE ("Host channel sets map to layouts: standard speaker sets only")
{
    using S = juce::AudioChannelSet;
    CHECK (*ada::layoutFromChannelSet (S::mono()) == ada::ChannelLayout::mono());
    CHECK (*ada::layoutFromChannelSet (S::stereo()) == ada::ChannelLayout::stereo());
    CHECK (*ada::layoutFromChannelSet (S::create5point0()) == ada::ChannelLayout::surround50());
    CHECK (*ada::layoutFromChannelSet (S::create5point1()) == ada::ChannelLayout::surround51());
    S sideFive; // 5.1 with side instead of back surrounds ("Music"): still Ls / Rs
    for (auto t : { S::left, S::right, S::centre, S::LFE, S::leftSurroundSide, S::rightSurroundSide })
        sideFive.addChannel (t);
    CHECK (*ada::layoutFromChannelSet (sideFive) == ada::ChannelLayout::surround51());

    // 7.1: the buffer order is JUCE's (by channel type), the roles follow it
    const auto seven = ada::layoutFromChannelSet (S::create7point1());
    REQUIRE (seven.has_value());
    CHECK (seven->count == 8);
    CHECK (seven->measuredCount() == 7);
    CHECK (ada::displayName (*seven) == "7.1");
    const auto sevenLayout = S::create7point1();
    for (int ch = 0; ch < 8; ++ch)
    {
        const auto type = sevenLayout.getTypeOfChannel (ch);
        const bool rear = type == S::leftSurroundRear || type == S::rightSurroundRear;
        CHECK ((seven->roles[(size_t) ch] == ada::Role::Lb || seven->roles[(size_t) ch] == ada::Role::Rb) == rear);
    }

    for (const auto& set : { S::createLCR(), S::quadraphonic(), S::discreteChannels (6), S::create7point1SDDS(),
                             S::create7point1point4(), S::disabled() })
        CHECK_FALSE (ada::layoutFromChannelSet (set).has_value());
}
