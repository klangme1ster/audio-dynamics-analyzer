#pragma once

#include "DynamicsEngine.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace ada
{

// Live PSR statistics gate (spec 3.7) using the running LRA relative gate carried by each frame.
inline bool passesLivePsrGate (const DynamicsMetrics& f) noexcept
{
    return f.binIndex >= 29 && f.shortTermLUFS >= -70.0f && f.shortTermLUFS >= f.lraGateLUFS
           && std::isfinite (f.psr);
}

// UI-side history of one measurement (spec 4): a fixed ring of the last 90 minutes of frames plus
// PSR statistics since the last reset. Single-threaded (message thread); allocates only in the
// constructor.
class HistoryStore
{
public:
    static constexpr int capacity = 54000; // 90 min at 10 frames/s
    static constexpr double psrBinWidth = 0.1, psrHistogramMin = -20.0, psrHistogramMax = 60.0;

    HistoryStore();

    // Adds a frame. A frame from a new epoch (reset or re-prepare) starts a new history.
    void push (const DynamicsMetrics& frame);

    // Reset button: clears now and ignores frames of the current epoch until a newer one arrives,
    // because the engine only resets at its next bin boundary.
    void clearForReset();

    int size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }
    const DynamicsMetrics& at (int i) const noexcept; // 0 = oldest
    const DynamicsMetrics& latest() const noexcept { return at (count - 1); }

    // Index of the first frame whose binIndex >= bin (size() if none).
    int lowerBound (std::int64_t bin) const noexcept;

    // Increments on every change, so views can repaint only when needed.
    std::uint64_t version() const noexcept { return changeCount; }
    // Increments whenever a new measurement starts (reset or new epoch).
    std::uint64_t measurementId() const noexcept { return measurementCount; }

    struct PsrStats
    {
        std::uint64_t measuredFrames = 0;              // frames that passed the live gate
        std::optional<float> min, p10;                  // p10 resolution: psrBinWidth
        std::array<float, 4> zonePercent {};            // green, yellow, orange, red
    };
    PsrStats psrStats() const;

private:
    void clear();

    std::vector<DynamicsMetrics> ring;
    int head = 0, count = 0; // head = index of the oldest frame
    std::optional<std::uint32_t> epoch, ignoredEpoch;
    std::uint64_t changeCount = 0, measurementCount = 0;

    std::vector<std::uint32_t> psrHistogram;
    std::array<std::uint64_t, 4> zoneCounts {};
    std::uint64_t measured = 0;
    float psrMin = 0.0f;
};

} // namespace ada
