#include "LoudnessHistogram.h"

#include "Loudness.h"

#include <algorithm>
#include <cmath>

namespace ada
{

void LoudnessHistogram::allocate()
{
    counts.assign (numBins, 0);
    powers.assign (numBins, 0.0);
    total = 0;
    totalPower = 0.0;
}

void LoudnessHistogram::clear() noexcept
{
    std::fill (counts.begin(), counts.end(), 0);
    std::fill (powers.begin(), powers.end(), 0.0);
    total = 0;
    totalPower = 0.0;
}

void LoudnessHistogram::add (double lufs, double power) noexcept
{
    if (! std::isfinite (lufs) || ! std::isfinite (power))
        return;
    const int b = std::clamp ((int) std::floor ((lufs - minLUFS) / step), 0, numBins - 1);
    ++counts[(size_t) b];
    powers[(size_t) b] += power;
    ++total;
    totalPower += power;
}

std::optional<double> LoudnessHistogram::integrated() const noexcept
{
    if (total == 0)
        return std::nullopt;
    const double gate = powerToLUFS (totalPower / (double) total) - 10.0;
    std::uint64_t n = 0;
    double sum = 0.0;
    for (int b = 0; b < numBins; ++b)
        if (binCenter (b) > gate)
        {
            n += counts[(size_t) b];
            sum += powers[(size_t) b];
        }
    if (n == 0)
        return std::nullopt;
    return powerToLUFS (sum / (double) n);
}

LoudnessHistogram::Range LoudnessHistogram::loudnessRange() const noexcept
{
    if (total == 0)
        return {};
    const double gate = powerToLUFS (totalPower / (double) total) - 20.0;
    int first = 0;
    while (first < numBins && binCenter (first) < gate)
        ++first;

    std::uint64_t kept = 0;
    for (int b = first; b < numBins; ++b)
        kept += counts[(size_t) b];
    if (kept < 2)
        return { std::nullopt, gate };

    // EBU Tech 3342 reference percentile index: floor((count - 1) * p + 0.5), 0-based
    auto valueAtRank = [&] (std::uint64_t rank) {
        std::uint64_t seen = 0;
        for (int b = first; b < numBins; ++b)
        {
            seen += counts[(size_t) b];
            if (seen > rank)
                return binCenter (b);
        }
        return binCenter (numBins - 1);
    };
    const auto rank10 = (std::uint64_t) std::floor ((double) (kept - 1) * 0.10 + 0.5);
    const auto rank95 = (std::uint64_t) std::floor ((double) (kept - 1) * 0.95 + 0.5);
    return { valueAtRank (rank95) - valueAtRank (rank10), gate };
}

} // namespace ada
