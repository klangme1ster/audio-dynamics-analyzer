#include "HistoryStore.h"

#include <algorithm>
#include <cmath>

namespace ada
{

namespace
{
    constexpr int numPsrBins = (int) ((HistoryStore::psrHistogramMax - HistoryStore::psrHistogramMin)
                                      / HistoryStore::psrBinWidth);
}

HistoryStore::HistoryStore (int capacity)
    : ring ((std::size_t) std::max (1, capacity)), ringCapacity (std::max (1, capacity)),
      psrHistogram ((std::size_t) numPsrBins, 0)
{
}

void HistoryStore::clear()
{
    ++measurementCount;
    head = count = 0;
    std::fill (psrHistogram.begin(), psrHistogram.end(), 0u);
    zoneCounts = {};
    measured = 0;
    psrMin = 0.0f;
    ++changeCount;
}

void HistoryStore::clearForReset()
{
    ignoredEpoch = epoch;
    clear();
}

void HistoryStore::push (const DynamicsMetrics& f)
{
    if (ignoredEpoch && f.epoch == *ignoredEpoch)
        return;
    ignoredEpoch.reset();
    if (! epoch || f.epoch != *epoch)
    {
        epoch = f.epoch;
        clear();
    }

    if (count < ringCapacity)
        ring[(std::size_t) ((head + count++) % ringCapacity)] = f;
    else
    {
        ring[(std::size_t) head] = f;
        head = (head + 1) % ringCapacity;
    }

    if (passesLivePsrGate (f))
    {
        const int bin = std::clamp ((int) std::floor ((f.psr - psrHistogramMin) / psrBinWidth), 0, numPsrBins - 1);
        ++psrHistogram[(size_t) bin];
        ++zoneCounts[f.psr >= 12.0f ? 0 : f.psr >= 10.0f ? 1 : f.psr >= 8.0f ? 2 : 3];
        psrMin = measured == 0 ? f.psr : std::min (psrMin, f.psr);
        ++measured;
    }
    ++changeCount;
}

const DynamicsMetrics& HistoryStore::at (int i) const noexcept
{
    return ring[(std::size_t) ((head + i) % ringCapacity)];
}

int HistoryStore::lowerBound (std::int64_t bin) const noexcept
{
    int lo = 0, hi = count;
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (at (mid).binIndex < bin)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

HistoryStore::PsrStats HistoryStore::psrStats() const
{
    PsrStats s;
    s.measuredFrames = measured;
    if (measured == 0)
        return s;
    s.min = psrMin;
    for (std::size_t z = 0; z < 4; ++z)
        s.zonePercent[z] = (float) (100.0 * (double) zoneCounts[z] / (double) measured);

    // Tech 3342 percentile rule: 0-based rank floor((count - 1) * p + 0.5); bin centre as value
    const auto rank = (std::uint64_t) std::floor ((double) (measured - 1) * 0.10 + 0.5);
    std::uint64_t seen = 0;
    for (int b = 0; b < numPsrBins; ++b)
    {
        seen += psrHistogram[(size_t) b];
        if (seen > rank)
        {
            // bin centre, but never below the exact minimum (both are shown side by side)
            s.p10 = std::max (psrMin, (float) (psrHistogramMin + (b + 0.5) * psrBinWidth));
            break;
        }
    }
    return s;
}

} // namespace ada
