// UI history store: ring capacity, epochs, Reset handling and PSR statistics (plan 4.2-4.4).
#include <doctest/doctest.h>

#include "HistoryStore.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace
{
ada::DynamicsMetrics frame (std::int64_t bin, float psr, std::uint32_t epoch = 0)
{
    ada::DynamicsMetrics f {};
    f.binIndex = bin;
    f.epoch = epoch;
    f.shortTermLUFS = -14.0f;
    f.lraGateLUFS = -34.0f;
    f.psr = psr;
    return f;
}
} // namespace

TEST_CASE ("HistoryStore keeps the last 90 minutes")
{
    ada::HistoryStore h;
    const int total = ada::HistoryStore::liveCapacity + 10;
    for (int i = 0; i < total; ++i)
        h.push (frame (i, 10.0f));
    CHECK (h.size() == ada::HistoryStore::liveCapacity);
    CHECK (h.at (0).binIndex == 10);
    CHECK (h.latest().binIndex == total - 1);
    CHECK (h.lowerBound (10) == 0);
    CHECK (h.lowerBound (500) == 490);
    CHECK (h.lowerBound (total) == h.size());
    // statistics cover the whole measurement, not just what is still in the ring
    CHECK (h.psrStats().measuredFrames == (std::uint64_t) (total - 29));
}

TEST_CASE ("A file history can hold more than 90 minutes")
{
    ada::HistoryStore h (100000);
    for (int i = 0; i < 80000; ++i)
        h.push (frame (i, 10.0f));
    CHECK (h.capacity() == 100000);
    CHECK (h.size() == 80000);
    CHECK (h.at (0).binIndex == 0);
}

TEST_CASE ("A new epoch starts a new history")
{
    ada::HistoryStore h;
    for (int i = 0; i < 100; ++i)
        h.push (frame (i, 6.0f));
    const auto v = h.version();
    h.push (frame (0, 15.0f, 1));
    CHECK (h.version() != v);
    CHECK (h.size() == 1);
    CHECK (h.psrStats().measuredFrames == 0); // bin 0 is in the 3 s warm-up
}

TEST_CASE ("Reset clears at once and ignores late frames of the old epoch")
{
    ada::HistoryStore h;
    for (int i = 0; i < 100; ++i)
        h.push (frame (i, 6.0f));
    h.clearForReset();
    CHECK (h.empty());
    CHECK (! h.psrStats().min.has_value());
    h.push (frame (100, 6.0f)); // engine has not reached its bin boundary yet
    CHECK (h.empty());
    h.push (frame (0, 12.0f, 1));
    CHECK (h.size() == 1);
}

TEST_CASE ("Every new measurement gets a new id (the graph returns to live on it)")
{
    ada::HistoryStore h;
    h.push (frame (0, 10.0f));
    const auto first = h.measurementId();
    h.push (frame (1, 10.0f));
    CHECK (h.measurementId() == first);
    h.clearForReset();
    CHECK (h.measurementId() != first);
    const auto afterReset = h.measurementId();
    h.push (frame (0, 10.0f, 1));
    CHECK (h.measurementId() != afterReset);
}

TEST_CASE ("PSR statistics use the live gate and the Tech 3342 percentile rule")
{
    ada::HistoryStore h;
    std::mt19937 rng (3);
    std::uniform_real_distribution<float> psr (4.0f, 20.0f);
    std::vector<float> measured;
    for (int i = 0; i < 2000; ++i)
    {
        auto f = frame (i, psr (rng));
        if (i % 7 == 0)
            f.shortTermLUFS = -80.0f; // below the absolute gate
        if (i % 11 == 0)
            f.psr = std::numeric_limits<float>::quiet_NaN();
        h.push (f);
        if (ada::passesLivePsrGate (f))
            measured.push_back (f.psr);
    }
    std::sort (measured.begin(), measured.end());
    const auto s = h.psrStats();
    REQUIRE (s.measuredFrames == measured.size());
    CHECK (*s.min == measured.front());
    const auto rank = (std::size_t) std::floor ((double) (measured.size() - 1) * 0.10 + 0.5);
    CHECK (std::abs (*s.p10 - measured[rank]) <= 0.051f);
    CHECK (*s.p10 >= *s.min);

    // P10 never shows below the minimum even when they share a histogram bin
    ada::HistoryStore one;
    for (int i = 0; i < 40; ++i)
        one.push (frame (i, 4.97f));
    CHECK (*one.psrStats().p10 == *one.psrStats().min);

    std::array<double, 4> zones {};
    for (float v : measured)
        ++zones[v >= 12 ? 0 : v >= 10 ? 1 : v >= 8 ? 2 : 3];
    float total = 0.0f;
    for (std::size_t z = 0; z < 4; ++z)
    {
        CHECK (std::abs (s.zonePercent[z] - (float) (100.0 * zones[z] / (double) measured.size())) < 1e-3f);
        total += s.zonePercent[z];
    }
    CHECK (std::abs (total - 100.0f) < 1e-3f);
}
