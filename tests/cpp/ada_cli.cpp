// ada_cli: run the C++ engine on raw float32 interleaved audio and print per-bin metrics and the
// summary as JSON. Used by python/tests/test_parity.py (spec ADA-PARITY).
//
//   ada_cli <input.f32> <sample_rate> <channels> [block_size | random]

#include "CliJson.h"
#include "DynamicsEngine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

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

    ada::cli::Collect sink;
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

    ada::cli::printResult (sink.frames, final, engine.oversamplingFactor(), fs, channels);
    return 0;
}
