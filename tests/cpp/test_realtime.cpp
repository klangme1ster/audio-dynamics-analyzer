// Real-time guarantees: no allocation in process() (ADA-RT-ALLOC), exact reset semantics
// (ADA-RT-RESET), block-size independence.
#include <doctest/doctest.h>

#include "TestSupport.h"

#include <array>
#include <cstring>
#include <limits>
#include <random>
#include <string>

using namespace adatest;

namespace
{
// Allocation-free sink: fixed storage, overwritten ring-style.
struct FixedSink : ada::MetricsSink
{
    std::array<ada::DynamicsMetrics, 64> frames {};
    std::size_t count = 0;
    void push (const ada::DynamicsMetrics& f) noexcept override { frames[count++ % frames.size()] = f; }
};

// Name of the first differing field, or nullptr when the frames are bit-identical.
const char* frameDifference (const ada::DynamicsMetrics& a, const ada::DynamicsMetrics& b)
{
    auto eq = [] (float x, float y) { return std::memcmp (&x, &y, sizeof x) == 0; };
#define ADA_CMP(field) if (! eq (a.field, b.field)) return #field;
    if (a.binIndex != b.binIndex) return "binIndex";
    ADA_CMP (momentaryLUFS) ADA_CMP (shortTermLUFS) ADA_CMP (momentaryPeakLUFS) ADA_CMP (shortTermPeakLUFS)
    ADA_CMP (maxMomentaryLUFS) ADA_CMP (maxShortTermLUFS) ADA_CMP (binPeakDBTP) ADA_CMP (truePeak3sDBTP)
    ADA_CMP (truePeakMaxDBTP) ADA_CMP (psr) ADA_CMP (integratedLUFS) ADA_CMP (lra) ADA_CMP (lraGateLUFS)
#undef ADA_CMP
    if (a.isClipping != b.isClipping) return "isClipping";
    return nullptr;
}

bool sameFrame (const ada::DynamicsMetrics& a, const ada::DynamicsMetrics& b) { return frameDifference (a, b) == nullptr; }
} // namespace

TEST_CASE ("ADA-RT-ALLOC: process() never allocates")
{
    // 10 minutes of noise + tone, random host block sizes 1-4096, resets and threshold changes
    const double fs = 48000.0;
    const std::size_t frames = (std::size_t) (600 * fs);
    std::mt19937 rng (42);
    std::normal_distribution<float> noise (0.0f, 0.05f);
    Signal s { fs, std::vector<std::vector<float>> (2, std::vector<float> (frames)) };
    for (std::size_t i = 0; i < frames; ++i)
        s.channels[0][i] = s.channels[1][i] = 0.3f * (float) std::sin (2.0 * 3.14159265 * 220.0 * (double) i / fs) + noise (rng);

    ada::DynamicsEngine engine;
    engine.prepare (fs, 2);
    FixedSink sink;
    std::uniform_int_distribution<int> blockSize (1, 4096);
    std::uniform_int_distribution<int> action (0, 499);
    const auto ptrs = s.pointers();
    std::array<const float*, 2> p {};

    allocationCount = 0;
    std::size_t pos = 0;
    while (pos < frames)
    {
        const int n = (int) std::min<std::size_t> ((std::size_t) blockSize (rng), frames - pos);
        p = { ptrs[0] + pos, ptrs[1] + pos };
        const int a = action (rng);
        countAllocations = true;
        if (a == 0)
            engine.requestReset();
        else if (a == 1)
            engine.setClipThreshold (-1.0f);
        engine.process (p.data(), n, sink);
        countAllocations = false;
        pos += (std::size_t) n;
    }
    CHECK (allocationCount.load() == 0);
    CHECK (sink.count > 5000);
}

static void checkResetEqualsFreshEngine (double fs)
{
    CAPTURE (fs);
    const auto s = fs == 48000.0 ? toneSequence (fs, { { 10, -36 }, { 60, -23 }, { 10, -36 } })
                                 : toneSequence (fs, { { 5, -36 }, { 15, -23 }, { 5, -36 } });
    const std::size_t binSize = (std::size_t) fs / 10;
    std::mt19937 rng (7);
    std::uniform_int_distribution<std::size_t> resetAt (1, s.frames() - 20 * binSize);

    for (int trial = 0; trial < 6; ++trial)
    {
        const std::size_t r = trial == 0 ? 25 * binSize : resetAt (rng); // include an exact boundary
        const std::size_t boundary = (r + binSize - 1) / binSize * binSize;
        CAPTURE (r);

        ada::DynamicsEngine engine;
        engine.prepare (s.fs, 2);
        CollectingSink sink;
        engine.process (s.pointers().data(), (int) r, sink);
        engine.requestReset();
        std::uniform_int_distribution<int> block (1, 3000);
        for (std::size_t pos = r; pos < s.frames();)
        {
            const int n = (int) std::min<std::size_t> ((std::size_t) block (rng), s.frames() - pos);
            engine.process (s.pointers (pos).data(), n, sink);
            pos += (std::size_t) n;
        }
        const auto finalA = engine.finish (sink);

        std::vector<ada::DynamicsMetrics> afterReset;
        for (const auto& f : sink.frames)
            if (f.epoch == 1)
                afterReset.push_back (f);

        const auto fresh = analyze (s.slice (boundary), 1024);
        REQUIRE (afterReset.size() == fresh.frames.size());
        for (std::size_t i = 0; i < afterReset.size(); ++i)
            if (const char* field = frameDifference (afterReset[i], fresh.frames[i]))
            {
                CAPTURE (i);
                CAPTURE (std::string (field));
                CAPTURE (afterReset[i].binIndex);
                FAIL ("frame differs from fresh engine");
            }
        CHECK (finalA.truePeakMaxDBTP == fresh.final.truePeakMaxDBTP);
    }
}

TEST_CASE ("ADA-RT-RESET: after a reset the engine equals a fresh one started at the next bin boundary")
{
    checkResetEqualsFreshEngine (48000.0);  // interpolator delay D = 6
    checkResetEqualsFreshEngine (192000.0); // no oversampling, D = 0
}

TEST_CASE ("Re-preparing starts a new epoch")
{
    const auto s = toneSequence (48000, { { 1, -20 } });
    ada::DynamicsEngine engine;
    CollectingSink sink;
    engine.prepare (48000, 2);
    engine.process (s.pointers().data(), (int) s.frames(), sink);
    engine.prepare (48000, 2);
    engine.process (s.pointers().data(), (int) s.frames(), sink);
    REQUIRE (! sink.frames.empty());
    CHECK (sink.frames.front().epoch == 0);
    CHECK (sink.frames.back().epoch == 1);
}

TEST_CASE ("Non-finite input samples do not stop the metering")
{
    auto s = toneSequence (48000, { { 30, -20 } }, 1);
    s.channels[0][(std::size_t) (5.0026 * 48000)] = std::numeric_limits<float>::quiet_NaN();
    s.channels[0][47999] = std::numeric_limits<float>::infinity(); // last sample of a bin
    const auto r = analyze (s);
    for (const auto& f : r.frames)
        if (f.binIndex >= 3)
        {
            CAPTURE (f.binIndex);
            REQUIRE (std::isfinite (f.momentaryLUFS));
        }
    REQUIRE (r.summary.integrated.has_value());
    CHECK (std::abs (*r.summary.integrated - (-23.0)) < 0.1);
}

TEST_CASE ("Block size does not change results")
{
    const auto s = toneSequence (44100, { { 7, -30 }, { 8.3, -18 }, { 5, -26 } });
    const auto a = analyze (s, 1);
    const auto b = analyze (s, 4096);
    const auto c = analyze (s, 4410 * 3 + 17);
    REQUIRE (a.frames.size() == b.frames.size());
    REQUIRE (a.frames.size() == c.frames.size());
    for (std::size_t i = 0; i < a.frames.size(); ++i)
    {
        CAPTURE (i);
        CHECK (std::string (frameDifference (a.frames[i], b.frames[i]) ? frameDifference (a.frames[i], b.frames[i]) : "") == "");
        CHECK (std::string (frameDifference (a.frames[i], c.frames[i]) ? frameDifference (a.frames[i], c.frames[i]) : "") == "");
    }
}
