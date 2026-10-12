#pragma once

#include "DynamicsEngine.h"

#include <array>
#include <optional>
#include <vector>

namespace ada
{

// Per-file report fields (spec 3.10), computed after the fact from all frames of one epoch.
// Not real-time safe.
struct Summary
{
    std::optional<double> integrated, lra, truePeakMax, plr, maxMomentary, maxShortTerm;
    std::optional<double> psrMin, psrMean, psrP10, peakNearCeilingPct;
    std::array<std::optional<double>, 4> zonePct; // green, yellow, orange, red
    std::size_t measuredFrames = 0;               // frames that passed the PSR statistics gate
};

Summary summarize (const std::vector<DynamicsMetrics>& frames, const DynamicsEngine::Final& final);

} // namespace ada
