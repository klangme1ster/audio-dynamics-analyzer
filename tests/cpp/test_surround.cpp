// Surround layouts (spec 3.11) in the C++ engine: ADA-SURR-W, ADA-LFE, ADA-SURR-TP, ADA-NOFOLD,
// ADA-LAYOUT. ADA-PARITY for 5.1 / 7.1 lives in python/tests/test_parity.py.
#include <doctest/doctest.h>

#include "ChannelLayout.h"
#include "TestSupport.h"

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace adatest;
using ada::ChannelLayout;
using ada::Role;

namespace
{
constexpr double fs = 48000.0;
const double surroundDb = 10.0 * std::log10 (ada::surroundWeight); // +1.49 dB

// 1 kHz sine of `levelDb` on one channel of the layout, silence elsewhere
Signal toneOn (const ChannelLayout& layout, int channel, double seconds = 20.0, double levelDb = -23.0)
{
    Signal s { fs, std::vector<std::vector<float>> ((std::size_t) layout.count) };
    const auto one = toneSequence (fs, { { seconds, levelDb } }, 1);
    for (int ch = 0; ch < layout.count; ++ch)
        s.channels[(std::size_t) ch] = ch == channel ? one.channels[0]
                                                     : std::vector<float> (one.channels[0].size(), 0.0f);
    return s;
}
} // namespace

TEST_CASE ("ADA-LAYOUT: standard layouts, role lists, masks and suggestions")
{
    CHECK (ChannelLayout::fromName ("5.1") == ChannelLayout::surround51());
    CHECK (ChannelLayout::surround71().count == 8);
    CHECK (ChannelLayout::surround71().roleList() == "L,R,C,LFE,Lb,Rb,Ls,Rs");
    CHECK (ChannelLayout::mono().weight (0) == 1.0);
    CHECK (ChannelLayout::surround51().weight (3) == 0.0); // LFE
    CHECK (ChannelLayout::surround51().weight (4) == ada::surroundWeight);
    CHECK (ChannelLayout::surround71().measuredCount() == 7);

    CHECK (ChannelLayout::parse ("L,R,C,LFE,Ls,Rs")->name() == "5.1");
    const auto permuted = ChannelLayout::parse ("Ls,L,Rs,C,LFE,R");
    REQUIRE (permuted.has_value());
    CHECK (permuted->name() == "custom");
    CHECK (permuted->count == 6);
    for (const char* bad : { "", "L,L", "LFE", "L,R,X", "L,R,C,LFE,Ls,Rs,Lb,Rb,C" })
        CHECK_FALSE (ChannelLayout::parse (bad).has_value());

    CHECK (ChannelLayout::fromWavMask (0x3F)->name() == "5.1");   // back pair
    CHECK (ChannelLayout::fromWavMask (0x60F)->name() == "5.1");  // side pair
    CHECK (ChannelLayout::fromWavMask (0x63F)->name() == "7.1");
    CHECK (ChannelLayout::fromWavMask (0x37)->name() == "5.0");
    CHECK (ChannelLayout::fromWavMask (0x3)->name() == "stereo");
    CHECK_FALSE (ChannelLayout::fromWavMask (0).has_value());
    CHECK_FALSE (ChannelLayout::fromWavMask (0x1FF).has_value()); // unsupported speakers

    CHECK (ChannelLayout::suggest (5)->name() == "5.0");
    CHECK (ChannelLayout::suggest (6)->name() == "5.1");
    CHECK (ChannelLayout::suggest (8)->name() == "7.1");
    for (int n : { 3, 4, 7 })
        CHECK_FALSE (ChannelLayout::suggest (n).has_value());
}

TEST_CASE ("ADA-LAYOUT: the engine rejects invalid layouts and multichannel without one")
{
    ada::DynamicsEngine engine;
    CHECK_THROWS_AS (engine.prepare (fs, 6), std::invalid_argument); // needs a layout
    CHECK_THROWS_AS (engine.prepare (fs, ChannelLayout { Role::L, Role::L }), std::invalid_argument);
    CHECK_THROWS_AS (engine.prepare (fs, ChannelLayout { Role::LFE }), std::invalid_argument);
    CHECK_THROWS_AS (engine.prepare (fs, ChannelLayout {}), std::invalid_argument);
    CHECK_NOTHROW (engine.prepare (fs, 1));
    CHECK_NOTHROW (engine.prepare (fs, 2));
    CHECK_NOTHROW (engine.prepare (fs, ChannelLayout::surround71()));
    CHECK (engine.channels() == 8);
    CHECK (engine.layout().name() == "7.1");
}

TEST_CASE ("ADA-SURR-W: channel weights in 5.1 and 7.1")
{
    for (const auto& layout : { ChannelLayout::surround51(), ChannelLayout::surround71() })
        for (int ch = 0; ch < layout.count; ++ch)
        {
            const auto r = analyze (toneOn (layout, ch), layout);
            CAPTURE (layout.name());
            CAPTURE (ChannelLayout::roleName (layout.roles[(std::size_t) ch]));
            if (layout.roles[(std::size_t) ch] == Role::LFE)
            {
                CHECK_FALSE (r.summary.integrated.has_value()); // reads as silence
                continue;
            }
            REQUIRE (r.summary.integrated.has_value());
            const double expected = layout.weight (ch) == 1.0 ? -26.0 : -26.0 + surroundDb;
            CHECK (std::abs (*r.summary.integrated - expected) <= 0.1);
        }
}

TEST_CASE ("ADA-LFE: the LFE changes nothing")
{
    const auto layout = ChannelLayout::surround51();
    const auto bed = toneOn (layout, 0, 20.0, -20.0);
    auto loud = bed;
    const auto lfe = peakSine (fs, 960.0, 1.0, 0.0, 20.0, 0.05, 1); // 50 Hz at 0 dBFS
    loud.channels[3] = lfe.channels[0];

    const auto a = analyze (bed, layout), b = analyze (loud, layout);
    REQUIRE (a.frames.size() == b.frames.size());
    for (std::size_t i = 0; i < a.frames.size(); ++i)
    {
        CHECK (a.frames[i].momentaryLUFS == b.frames[i].momentaryLUFS);
        CHECK (a.frames[i].shortTermLUFS == b.frames[i].shortTermLUFS);
        CHECK (a.frames[i].binPeakDBTP == b.frames[i].binPeakDBTP);
    }
    CHECK (a.final.truePeakMaxDBTP == b.final.truePeakMaxDBTP);
    CHECK (*a.summary.integrated == *b.summary.integrated);
}

TEST_CASE ("ADA-SURR-TP: true peak over all channels except the LFE")
{
    const auto layout = ChannelLayout::surround71();
    for (int ch = 0; ch < layout.count; ++ch)
    {
        Signal s { fs, std::vector<std::vector<float>> ((std::size_t) layout.count) };
        const auto isp = peakSine (fs, 4.0, std::sqrt (2.0), 45.0, 20.0, 0.05, 1);
        for (int c = 0; c < layout.count; ++c)
            s.channels[(std::size_t) c] = c == ch ? isp.channels[0] : std::vector<float> (isp.channels[0].size(), 0.0f);
        const auto r = analyze (s, layout);
        CAPTURE (ch);
        if (layout.roles[(std::size_t) ch] == Role::LFE)
            CHECK (r.final.truePeakMaxDBTP == -std::numeric_limits<double>::infinity());
        else
        {
            CHECK (r.final.truePeakMaxDBTP >= 3.01 - 0.4);
            CHECK (r.final.truePeakMaxDBTP <= 3.01 + 0.2);
        }
    }
}

TEST_CASE ("ADA-NOFOLD: channels are never folded down")
{
    const auto layout = ChannelLayout::surround51();
    auto inPhase = toneOn (layout, 0);
    inPhase.channels[1] = inPhase.channels[0];
    auto inverted = inPhase;
    for (auto& v : inverted.channels[1])
        v = -v;
    const auto a = analyze (inPhase, layout), b = analyze (inverted, layout);
    CHECK (*a.summary.integrated == doctest::Approx (*b.summary.integrated).epsilon (1e-12));
    const auto one = analyze (toneOn (layout, 0), layout);
    CHECK (*a.summary.integrated == doctest::Approx (*one.summary.integrated + 3.01).epsilon (0.001));
}

TEST_CASE ("ADA-LAYOUT: channel order only matters through the roles")
{
    const auto file = ChannelLayout::surround51();
    auto x = toneOn (file, 4, 20.0, -20.0); // Ls
    const auto center = toneOn (file, 2, 20.0, -27.0);
    x.channels[2] = center.channels[2];

    // the same signal with the channels permuted and the matching role list
    const int perm[6] = { 4, 0, 5, 2, 3, 1 };
    Signal y { fs, {} };
    ChannelLayout custom;
    for (int j = 0; j < 6; ++j)
    {
        y.channels.push_back (x.channels[(std::size_t) perm[j]]);
        custom.roles[(std::size_t) custom.count++] = file.roles[(std::size_t) perm[j]];
    }
    const auto a = analyze (x, file), b = analyze (y, custom);
    REQUIRE (a.frames.size() == b.frames.size());
    for (std::size_t i = 0; i < a.frames.size(); ++i)
        CHECK (a.frames[i].shortTermLUFS == doctest::Approx (b.frames[i].shortTermLUFS).epsilon (1e-9));
    CHECK (*a.summary.integrated == doctest::Approx (*b.summary.integrated).epsilon (1e-12));
}
