// ada_file_cli: analyse an audio file the way the plugin's file analysis does (same readers,
// including the Media Foundation M4A / AAC reader on Windows) and print per-bin metrics and the
// summary as JSON. Used by python/tests/test_parity.py for decoded formats (spec ADA-PARITY).
//
//   ada_file_cli <file> [--dump <out.f32>]   (--dump also writes the decoded samples, float32 interleaved)

#include "CliJson.h"
#include "FileAnalysis.h"
#include "MediaFoundationFormat.h"

#include <iostream>
#include <stdexcept>

namespace
{
juce::String arg (const char* a) { return juce::String::fromUTF8 (a); }
[[maybe_unused]] juce::String arg (const wchar_t* a) { return juce::String (a); }
} // namespace

// wide arguments on Windows: the narrow ones use the ANSI code page and lose e.g. umlauts
#if JUCE_WINDOWS
int wmain (int argc, wchar_t** argv)
#else
int main (int argc, char** argv)
#endif
{
    if (argc < 2)
    {
        std::cerr << "usage: ada_file_cli <file>\n";
        return 2;
    }
    const juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (arg (argv[1])));
    juce::AudioFormatManager formats;
    ada::registerAudioFormats (formats);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        std::cerr << "cannot open " << arg (argv[1]).toRawUTF8() << " (Media Foundation "
                  << (ada::mediaFoundationAvailable() ? "available" : "not available") << ")\n";
        return 1;
    }
    ada::DynamicsEngine probe; // for the oversampling factor only
    try
    {
        probe.prepare (reader->sampleRate, (int) reader->numChannels);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }

    ada::cli::Collect sink;
    bool decodeFailed = false;
    const auto final = ada::FileAnalysis::analyseReader (*reader, sink, [] { return false; }, nullptr, &decodeFailed);
    if (! final)
    {
        std::cerr << "decoding failed: " << arg (argv[1]).toRawUTF8() << "\n";
        return 1;
    }
    std::cerr << "reader: " << reader->getFormatName() << "\n";
    if (argc > 3 && arg (argv[2]) == "--dump") // decoded samples, float32 interleaved
    {
        juce::FileOutputStream out (juce::File::getCurrentWorkingDirectory().getChildFile (arg (argv[3])));
        out.setPosition (0);
        out.truncate();
        const int ch = (int) reader->numChannels;
        juce::AudioBuffer<float> buffer (ch, 32768);
        for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += 32768)
        {
            const int n = (int) std::min<juce::int64> (32768, reader->lengthInSamples - pos);
            reader->read (&buffer, 0, n, pos, true, true);
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < ch; ++c)
                    out.writeFloat (buffer.getSample (c, i));
        }
    }
    ada::cli::printResult (sink.frames, *final, probe.oversamplingFactor(), reader->sampleRate, (int) reader->numChannels);
    return 0;
}
