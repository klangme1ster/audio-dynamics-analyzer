// Engine correctness: coefficients, compliance signals, live-meter cases, oracle (spec 2).
#include <doctest/doctest.h>

#include "GeneratedTables.h"
#include "KWeighting.h"
#include "TestSupport.h"

extern "C" {
#include <ebur128.h>
}

#include <algorithm>
#include <cmath>
#include <limits>

using namespace adatest;

namespace
{
bool tpWithin (double measured, double expected) { return measured >= expected - 0.4 && measured <= expected + 0.2; }
}

TEST_CASE ("ADA-KCOEF: K-weighting coefficients at 48 kHz match BS.1770")
{
    const auto c = ada::kWeightingCoefficients (48000.0);
    const double expected[2][5] = {
        { 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 },
        { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 },
    };
    for (int s = 0; s < 2; ++s)
    {
        const double got[5] = { c[(size_t) s].b0, c[(size_t) s].b1, c[(size_t) s].b2, c[(size_t) s].a1, c[(size_t) s].a2 };
        for (int i = 0; i < 5; ++i)
            CHECK (std::abs (got[i] - expected[s][i]) <= 1e-12);
    }
}

TEST_CASE ("ADA-FIR: Annex 2 filter is symmetric with the expected phase gains")
{
    using namespace ada::tables;
    double h[48];
    for (int j = 0; j < annex2Rows; ++j)
        for (int p = 0; p < annex2Phases; ++p)
            h[4 * j + p] = annex2[j][p];
    for (int k = 0; k < 48; ++k)
        CHECK (h[k] == h[47 - k]);
    const double gains[4] = { 1.00158691, 0.97302246, 0.97302246, 1.00158691 };
    for (int p = 0; p < 4; ++p)
    {
        double sum = 0.0;
        for (int j = 0; j < annex2Rows; ++j)
            sum += annex2[j][p];
        CHECK (std::abs (sum - gains[p]) < 1e-8);
    }
}

TEST_CASE ("ADA-RATE / ADA-ISP: oversampling factor and inter-sample peak")
{
    CHECK (ada::TruePeakDetector::oversamplingFactor (44100) == 4);
    CHECK (ada::TruePeakDetector::oversamplingFactor (48000) == 4);
    CHECK (ada::TruePeakDetector::oversamplingFactor (88200) == 2);
    CHECK (ada::TruePeakDetector::oversamplingFactor (96000) == 2);
    CHECK (ada::TruePeakDetector::oversamplingFactor (192000) == 1);
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        CAPTURE (fs);
        const auto r = analyze (peakSine (fs, 4, std::sqrt (2.0), 45.0, 10.0));
        CHECK (tpWithin (r.final.truePeakMaxDBTP, 3.01));
    }
}

TEST_CASE ("T3341 cases 15-19: true peak")
{
    struct Case { double div, amp, phase, expected; };
    for (const Case c : { Case { 4, 0.5, 0.0, -6.0 }, Case { 4, 0.5, 45.0, -6.0 }, Case { 6, 0.5, 60.0, -6.0 },
                          Case { 8, 0.5, 67.5, -6.0 }, Case { 4, 1.41, 45.0, 3.0 } })
        for (double fs : { 44100.0, 48000.0 })
        {
            CAPTURE (c.div);
            CAPTURE (c.phase);
            CAPTURE (fs);
            CHECK (tpWithin (analyze (peakSine (fs, c.div, c.amp, c.phase, 5.0)).final.truePeakMaxDBTP, c.expected));
        }
}

TEST_CASE ("T3341 cases 1-5: integrated loudness and gating")
{
    const std::vector<std::pair<std::vector<std::pair<double, double>>, double>> cases = {
        { { { 20, -23 } }, -23 },
        { { { 20, -33 } }, -33 },
        { { { 10, -36 }, { 60, -23 }, { 10, -36 } }, -23 },
        { { { 10, -72 }, { 10, -36 }, { 60, -23 }, { 10, -36 }, { 10, -72 } }, -23 },
        { { { 20, -26 }, { 20.1, -20 }, { 20, -26 } }, -23 },
    };
    for (const auto& [segments, expected] : cases)
    {
        const auto r = analyze (toneSequence (48000, segments));
        REQUIRE (r.summary.integrated.has_value());
        CHECK (std::abs (*r.summary.integrated - expected) <= 0.1);
    }
}

TEST_CASE ("T3342 cases 1-4: loudness range")
{
    const std::vector<std::pair<std::vector<std::pair<double, double>>, double>> cases = {
        { { { 20, -20 }, { 20, -30 } }, 10 },
        { { { 20, -20 }, { 20, -15 } }, 5 },
        { { { 20, -40 }, { 20, -20 } }, 20 },
        { { { 20, -50 }, { 20, -35 }, { 20, -20 }, { 20, -35 }, { 20, -50 } }, 15 },
    };
    for (const auto& [segments, expected] : cases)
    {
        const auto r = analyze (toneSequence (48000, segments));
        REQUIRE (r.summary.lra.has_value());
        CHECK (std::abs (*r.summary.lra - expected) <= 1.0);
    }
}

TEST_CASE ("T3341-11: live meter, max S per segment")
{
    std::vector<std::pair<double, double>> seq;
    for (int i = 0; i < 20; ++i)
        seq.insert (seq.end(), { { i * 0.15, silence }, { 3.0, -38.0 + i }, { 3.0 - i * 0.15, silence } });
    const auto r = analyze (toneSequence (48000, seq));
    for (int i = 0; i < 20; ++i)
    {
        float best = -std::numeric_limits<float>::infinity();
        for (const auto& f : r.frames)
            if (f.binIndex >= 60 * i && f.binIndex < 60 * (i + 1))
                best = std::max (best, f.shortTermPeakLUFS);
        CAPTURE (i);
        CHECK (std::abs (best - (-38.0 + i)) <= 0.1);
    }
}

TEST_CASE ("T3341-14: live meter, max M per segment")
{
    std::vector<std::pair<double, double>> seq;
    for (int i = 0; i < 20; ++i)
        seq.insert (seq.end(), { { i * 0.02, silence }, { 0.4, -38.0 + i }, { 0.4 - i * 0.02, silence } });
    const auto r = analyze (toneSequence (48000, seq));
    for (int i = 0; i < 20; ++i)
    {
        float best = -std::numeric_limits<float>::infinity();
        for (const auto& f : r.frames)
            if (f.binIndex >= 8 * i && f.binIndex < 8 * (i + 1))
                best = std::max (best, f.momentaryPeakLUFS);
        CAPTURE (i);
        CHECK (std::abs (best - (-38.0 + i)) <= 0.1);
    }
}

TEST_CASE ("ADA-MONO: mono is a single channel, not dual mono")
{
    const auto r = analyze (toneSequence (48000, { { 20, -23 } }, 1));
    REQUIRE (r.summary.integrated.has_value());
    CHECK (std::abs (*r.summary.integrated - (-26.0)) <= 0.1);
}

TEST_CASE ("ADA-TAIL: final partial bin counts toward max true peak only")
{
    Signal s { 48000.0, std::vector<std::vector<float>> (2, std::vector<float> ((size_t) (10.05 * 48000), 0.0f)) };
    const auto burst = peakSine (48000, 4, std::sqrt (2.0), 45.0, 0.02, 0.0);
    const auto start = (size_t) (10.02 * 48000);
    for (size_t ch = 0; ch < 2; ++ch)
        std::copy (burst.channels[ch].begin(), burst.channels[ch].end(), s.channels[ch].begin() + (std::ptrdiff_t) start);
    const auto r = analyze (s);
    CHECK (r.frames.size() == 100);
    for (const auto& f : r.frames)
        CHECK (std::isinf (f.binPeakDBTP));
    CHECK (tpWithin (r.final.truePeakMaxDBTP, 3.01));
}

TEST_CASE ("ADA-ORACLE: integrated loudness agrees with libebur128")
{
    const std::vector<std::vector<std::pair<double, double>>> cases = {
        { { 20, -23 } }, { { 20, -33 } }, { { 10, -36 }, { 60, -23 }, { 10, -36 } },
        { { 10, -72 }, { 10, -36 }, { 60, -23 }, { 10, -36 }, { 10, -72 } },
        { { 20, -26 }, { 20.1, -20 }, { 20, -26 } },
    };
    for (const auto& segments : cases)
    {
        const auto s = toneSequence (48000, segments);
        ebur128_state* st = ebur128_init (2, 48000, EBUR128_MODE_I);
        REQUIRE (st != nullptr);
        std::vector<float> interleaved (s.frames() * 2);
        for (size_t i = 0; i < s.frames(); ++i)
        {
            interleaved[2 * i] = s.channels[0][i];
            interleaved[2 * i + 1] = s.channels[1][i];
        }
        ebur128_add_frames_float (st, interleaved.data(), s.frames());
        double oracle = 0.0;
        ebur128_loudness_global (st, &oracle);
        ebur128_destroy (&st);

        const auto r = analyze (s);
        REQUIRE (r.summary.integrated.has_value());
        CHECK (std::abs (*r.summary.integrated - oracle) <= 0.1);
    }
}

#include "InterleavedConvert.h"

TEST_CASE ("Interleaved device samples convert to planar float (Windows output capture)")
{
    float l[3] {}, r[3] {};
    float* dst[2] { l, r };

    // 4-channel float (e.g. quad device): only front left/right are taken
    const float f[12] = { 0.5f, -0.25f, 9.0f, 9.0f, 1.0f, -1.0f, 9.0f, 9.0f, 0.0f, 0.125f, 9.0f, 9.0f };
    ada::interleavedToPlanar (reinterpret_cast<const std::uint8_t*> (f), ada::SampleFormat::float32, 4, 3, dst, 2);
    CHECK (l[0] == 0.5f);
    CHECK (r[0] == -0.25f);
    CHECK (l[1] == 1.0f);
    CHECK (r[2] == 0.125f);

    const std::int16_t i16[4] = { 16384, -32768, 32767, 0 };
    ada::interleavedToPlanar (reinterpret_cast<const std::uint8_t*> (i16), ada::SampleFormat::int16, 2, 2, dst, 2);
    CHECK (l[0] == 0.5f);
    CHECK (r[0] == -1.0f);
    CHECK (l[1] == doctest::Approx (32767.0 / 32768.0));

    // 24-bit little endian: 0x400000 = +0.5, 0xC00000 = -0.5
    const std::uint8_t i24[6] = { 0x00, 0x00, 0x40, 0x00, 0x00, 0xC0 };
    ada::interleavedToPlanar (i24, ada::SampleFormat::int24, 2, 1, dst, 2);
    CHECK (l[0] == 0.5f);
    CHECK (r[0] == -0.5f);

    const std::int32_t i32[2] = { 1073741824, -2147483647 - 1 };
    ada::interleavedToPlanar (reinterpret_cast<const std::uint8_t*> (i32), ada::SampleFormat::int32, 2, 1, dst, 2);
    CHECK (l[0] == 0.5f);
    CHECK (r[0] == -1.0f);

    // mono device into a mono engine
    float m[2] {};
    float* mono[1] { m };
    const float fm[2] = { 0.3f, -0.7f };
    ada::interleavedToPlanar (reinterpret_cast<const std::uint8_t*> (fm), ada::SampleFormat::float32, 1, 2, mono, 1);
    CHECK (m[1] == -0.7f);
}

#include "DeliveryTargets.h"

TEST_CASE ("Delivery targets: loudness and peak verdicts")
{
    using ada::TargetStatus;
    const auto& spotify = ada::deliveryTargets[0];
    const auto& ebu = ada::deliveryTargets[4];

    // loud master: turned down, and Spotify's stricter -2 dBTP ceiling applies
    auto v = ada::evaluateTarget (spotify, -8.6, -1.5);
    CHECK (v.loudness == TargetStatus::warning);
    CHECK (v.loudnessDelta == doctest::Approx (5.4));
    CHECK (v.ceiling == -2.0);
    CHECK (v.peak == TargetStatus::warning);
    CHECK (v.peakMargin == doctest::Approx (-0.5));

    // on target, within the -1 dBTP ceiling
    v = ada::evaluateTarget (spotify, -14.4, -1.2);
    CHECK (v.loudness == TargetStatus::ok);
    CHECK (v.ceiling == -1.0);
    CHECK (v.peak == TargetStatus::ok);

    // quieter than the target; inter-sample peaks above 0 dBTP clip
    v = ada::evaluateTarget (spotify, -18.0, 0.3);
    CHECK (v.loudness == TargetStatus::warning);
    CHECK (v.loudnessDelta == doctest::Approx (-4.0));
    CHECK (v.peak == TargetStatus::problem);

    // broadcast: outside +-0.5 LU or above the ceiling is a problem
    CHECK (ada::evaluateTarget (ebu, -23.4, -3.0).loudness == TargetStatus::ok);
    CHECK (ada::evaluateTarget (ebu, -22.3, -3.0).loudness == TargetStatus::problem);
    CHECK (ada::evaluateTarget (ebu, -23.0, -0.8).peak == TargetStatus::problem);

    // limits count as on target, also for custom values stored as float
    auto custom = ada::deliveryTargets[ada::customTargetIndex];
    custom.loudness = (double) -14.3f;
    custom.peakCeiling = (double) -1.3f;
    v = ada::evaluateTarget (custom, -13.3, -1.3);
    CHECK (v.loudness == TargetStatus::ok);
    CHECK (v.peak == TargetStatus::ok);
    CHECK (ada::evaluateTarget (custom, -13.2, -1.2).peak == TargetStatus::warning);
    CHECK (ada::evaluateTarget (spotify, -14.0, -1.5).ceiling == -1.0); // exactly on target: normal ceiling

    // nothing measured yet
    v = ada::evaluateTarget (ebu, std::nullopt, std::nullopt);
    CHECK (v.loudness == TargetStatus::unknown);
    CHECK (v.peak == TargetStatus::unknown);
}
