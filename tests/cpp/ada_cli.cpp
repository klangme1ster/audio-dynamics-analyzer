// ada_cli: run the C++ engine on raw float32 interleaved audio and print per-bin metrics and the
// summary as JSON. Used by python/tests/test_parity.py (spec ADA-PARITY).
//
//   ada_cli <input.f32> <sample_rate> <channels> [block_size | random]

#include "DynamicsEngine.h"
#include "Summary.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace
{
struct Collect : ada::MetricsSink
{
    std::vector<ada::DynamicsMetrics> frames;
    void push (const ada::DynamicsMetrics& f) noexcept override { frames.push_back (f); }
};

std::string num (double v)
{
    if (std::isnan (v))
        return "NaN";
    if (std::isinf (v))
        return v > 0 ? "Infinity" : "-Infinity";
    char buf[32];
    std::snprintf (buf, sizeof buf, "%.9g", v);
    return buf;
}

std::string num (const std::optional<double>& v) { return v ? num (*v) : "null"; }

template <typename Get>
void array (const char* name, const std::vector<ada::DynamicsMetrics>& frames, Get get, bool last = false)
{
    std::cout << "  \"" << name << "\": [";
    for (std::size_t i = 0; i < frames.size(); ++i)
        std::cout << (i ? "," : "") << num ((double) get (frames[i]));
    std::cout << "]" << (last ? "\n" : ",\n");
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: ada_cli <input.f32> <sample_rate> <channels> [block_size|random]\n";
        return 2;
    }
    const double fs = std::atof (argv[2]);
    const int channels = std::atoi (argv[3]);
    const std::string blockArg = argc > 4 ? argv[4] : "4096";

    std::ifstream in (argv[1], std::ios::binary | std::ios::ate);
    if (! in)
    {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
    }
    const auto bytes = (std::size_t) in.tellg();
    in.seekg (0);
    std::vector<float> interleaved (bytes / sizeof (float));
    in.read (reinterpret_cast<char*> (interleaved.data()), (std::streamsize) (interleaved.size() * sizeof (float)));

    const std::size_t frames = interleaved.size() / (std::size_t) channels;
    std::vector<std::vector<float>> planar ((std::size_t) channels, std::vector<float> (frames));
    for (std::size_t i = 0; i < frames; ++i)
        for (int ch = 0; ch < channels; ++ch)
            planar[(std::size_t) ch][i] = interleaved[i * (std::size_t) channels + (std::size_t) ch];

    ada::DynamicsEngine engine;
    try
    {
        engine.prepare (fs, channels);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << "\n";
        return 1;
    }

    Collect sink;
    std::mt19937 rng (1234);
    std::uniform_int_distribution<int> randomBlock (1, 8192);
    const int fixedBlock = blockArg == "random" ? 0 : std::max (1, std::atoi (blockArg.c_str()));
    std::vector<const float*> ptrs ((std::size_t) channels);
    for (std::size_t pos = 0; pos < frames;)
    {
        const auto want = (std::size_t) (fixedBlock > 0 ? fixedBlock : randomBlock (rng));
        const auto n = std::min (want, frames - pos);
        for (int ch = 0; ch < channels; ++ch)
            ptrs[(std::size_t) ch] = planar[(std::size_t) ch].data() + pos;
        engine.process (ptrs.data(), (int) n, sink);
        pos += n;
    }
    const auto final = engine.finish (sink);
    const auto s = ada::summarize (sink.frames, final);

    std::cout << "{\n  \"frames\": " << final.frames << ",\n  \"oversampling\": " << engine.oversamplingFactor() << ",\n";
    const auto& f = sink.frames;
    array ("momentary", f, [] (const auto& x) { return x.momentaryLUFS; });
    array ("short_term", f, [] (const auto& x) { return x.shortTermLUFS; });
    array ("momentary_peak", f, [] (const auto& x) { return x.momentaryPeakLUFS; });
    array ("short_term_peak", f, [] (const auto& x) { return x.shortTermPeakLUFS; });
    array ("bin_peak_dbtp", f, [] (const auto& x) { return x.binPeakDBTP; });
    array ("true_peak_3s", f, [] (const auto& x) { return x.truePeak3sDBTP; });
    array ("psr", f, [] (const auto& x) { return x.psr; });
    std::cout << "  \"summary\": {\n"
              << "    \"integrated_lufs\": " << num (s.integrated) << ",\n"
              << "    \"lra\": " << num (s.lra) << ",\n"
              << "    \"true_peak_max_dbtp\": " << num (s.truePeakMax) << ",\n"
              << "    \"plr\": " << num (s.plr) << ",\n"
              << "    \"max_momentary_lufs\": " << num (s.maxMomentary) << ",\n"
              << "    \"max_short_term_lufs\": " << num (s.maxShortTerm) << ",\n"
              << "    \"psr_min\": " << num (s.psrMin) << ",\n"
              << "    \"psr_mean\": " << num (s.psrMean) << ",\n"
              << "    \"psr_p10\": " << num (s.psrP10) << ",\n"
              << "    \"peak_near_ceiling_pct\": " << num (s.peakNearCeilingPct) << ",\n"
              << "    \"psr_green_pct\": " << num (s.zonePct[0]) << ",\n"
              << "    \"psr_yellow_pct\": " << num (s.zonePct[1]) << ",\n"
              << "    \"psr_orange_pct\": " << num (s.zonePct[2]) << ",\n"
              << "    \"psr_red_pct\": " << num (s.zonePct[3]) << "\n"
              << "  }\n}\n";
    return 0;
}
