#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace ada
{

// Fixed 0.01 LU histogram from -70 to +10 LUFS holding a count and the sum of linear powers
// per bin (spec 4). Bounded memory for sessions of any length; storage is allocated by
// allocate() and only zeroed afterwards.
class LoudnessHistogram
{
public:
    static constexpr double minLUFS = -70.0;
    static constexpr double maxLUFS = 10.0;
    static constexpr double step = 0.01;
    static constexpr int numBins = 8000;

    void allocate();
    void clear() noexcept;
    void add (double lufs, double power) noexcept;

    // BS.1770 integrated loudness of everything added (absolute gate applied by the caller)
    std::optional<double> integrated() const noexcept;

    struct Range
    {
        std::optional<double> lra;
        std::optional<double> relativeGate;
    };
    // EBU Tech 3342 loudness range of everything added (absolute gate applied by the caller)
    Range loudnessRange() const noexcept;

private:
    static double binCenter (int b) noexcept { return minLUFS + (b + 0.5) * step; }
    std::vector<std::uint64_t> counts;
    std::vector<double> powers;
    std::uint64_t total = 0;
    double totalPower = 0.0;
};

} // namespace ada
