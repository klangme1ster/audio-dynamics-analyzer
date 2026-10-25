// ada_cli: run the C++ engine on raw float32 interleaved audio and print per-bin metrics and the
// summary as JSON. Used by python/tests/test_parity.py (spec ADA-PARITY).
//
//   ada_cli <input.f32> <sample_rate> <channels> [block_size | random] [layout]
// layout: 5.0, 5.1, 7.1 or a role list in file order (L,R,C,LFE,Ls,Rs); needed above 2 channels.

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
        std::cerr << "usage: ada_cli <input.f32> <sample_rate> <channels> [block_size|random] [layout]\n";
        return 2;
    }
    const double fs = std::atof (argv[2]);
    const int channels = std::atoi (argv[3]);
    const std::string blockArg = argc > 4 ? argv[4] : "4096";
    std::optional<ada::ChannelLayout> layout;
    if (channels == 1)
        layout = ada::ChannelLayout::mono();
    else if (channels == 2)
        layout = ada::ChannelLayout::stereo();
    if (argc > 5 && ! (layout = ada::ChannelLayout::parse (argv[5])))
    {
        std::cerr << "invalid layout " << argv[5] << "\n";
        return 2;
    }
    if (! layout)
    {
        std::cerr << channels << "-channel input needs a layout (5.0, 5.1, 7.1 or a role list)\n";
        return 2;
    }

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
        engine.prepare (fs, *layout);
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

    ada::cli::printResult (sink.frames, final, engine.oversamplingFactor(), fs, engine.layout());
    return 0;
}
