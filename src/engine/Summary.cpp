#include "Summary.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ada
{

namespace
{
    std::optional<double> finite (double v) { return std::isfinite (v) ? std::optional<double> (v) : std::nullopt; }
}

Summary summarize (const std::vector<DynamicsMetrics>& frames, const DynamicsEngine::Final& final)
{
    Summary s;
    s.truePeakMax = finite (final.truePeakMaxDBTP);
    if (frames.empty())
        return s;

    const auto& last = frames.back();
    s.integrated = finite (last.integratedLUFS);
    s.lra = finite (last.lra);
    s.maxMomentary = finite (last.maxMomentaryLUFS);
    s.maxShortTerm = finite (last.maxShortTermLUFS);
    if (s.integrated && s.truePeakMax)
        s.plr = *s.truePeakMax - *s.integrated;

    // PSR statistics gate (spec 3.7) with the final LRA relative gate
    std::vector<double> psr;
    std::size_t nearCeiling = 0;
    if (std::isfinite (last.lraGateLUFS))
    {
        for (const auto& f : frames)
        {
            if (f.binIndex < 29 || ! (f.shortTermLUFS >= -70.0f) || ! (f.shortTermLUFS >= last.lraGateLUFS)
                || ! std::isfinite (f.psr))
                continue;
            psr.push_back (f.psr);
            if (s.truePeakMax && f.binPeakDBTP >= *s.truePeakMax - 0.5)
                ++nearCeiling;
        }
    }
    if (psr.empty())
        return s;

    std::sort (psr.begin(), psr.end());
    const double count = (double) psr.size();
    s.psrMin = psr.front();
    s.psrMean = std::accumulate (psr.begin(), psr.end(), 0.0) / count;
    s.psrP10 = psr[(std::size_t) std::floor ((count - 1.0) * 0.10 + 0.5)];
    s.peakNearCeilingPct = 100.0 * (double) nearCeiling / count;
    s.measuredFrames = psr.size();

    const double edges[] = { 12.0, 10.0, 8.0 };
    std::array<double, 4> zone {};
    for (double v : psr)
        ++zone[v >= edges[0] ? 0 : v >= edges[1] ? 1 : v >= edges[2] ? 2 : 3];
    for (std::size_t z = 0; z < 4; ++z)
        s.zonePct[z] = 100.0 * zone[z] / count;
    return s;
}

} // namespace ada
