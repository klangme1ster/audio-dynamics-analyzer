#pragma once

#include <array>
#include <cmath>
#include <optional>

namespace ada
{

// Delivery targets (plan 4.5). Values change over time; only Spotify's figures and the broadcast
// standards are published by their owners, the other streaming figures are widely reported
// measurements (see USAGE.md, "What good values look like").
struct DeliveryTarget
{
    enum class Kind { streaming, broadcast };

    const char* name;
    Kind kind;
    double loudness;       // LUFS (LKFS)
    double tolerance;      // LU around `loudness` that counts as on target
    double peakCeiling;    // dBTP
    std::optional<double> loudMasterCeiling; // stricter ceiling for masters louder than `loudness`
};

inline constexpr std::array<DeliveryTarget, 7> deliveryTargets { {
    { "Spotify", DeliveryTarget::Kind::streaming, -14.0, 1.0, -1.0, -2.0 },
    { "Apple Music", DeliveryTarget::Kind::streaming, -16.0, 1.0, -1.0, std::nullopt },
    { "YouTube", DeliveryTarget::Kind::streaming, -14.0, 1.0, -1.0, std::nullopt },
    { "Tidal", DeliveryTarget::Kind::streaming, -14.0, 1.0, -1.0, std::nullopt },
    { "EBU R 128 broadcast", DeliveryTarget::Kind::broadcast, -23.0, 0.5, -1.0, std::nullopt },
    { "ATSC A/85 (US TV)", DeliveryTarget::Kind::broadcast, -24.0, 2.0, -2.0, std::nullopt },
    { "Custom", DeliveryTarget::Kind::streaming, -14.0, 1.0, -1.0, std::nullopt },
} };
inline constexpr int customTargetIndex = (int) deliveryTargets.size() - 1;

enum class TargetStatus { unknown, ok, warning, problem };

struct TargetVerdict
{
    TargetStatus loudness = TargetStatus::unknown, peak = TargetStatus::unknown;
    double loudnessDelta = 0.0;   // integrated - target (LU); > 0 means louder than the target
    double ceiling = 0.0;         // the ceiling that applies to this master (dBTP)
    double peakMargin = 0.0;      // ceiling - max true peak (dB); < 0 means above the ceiling
};

// Streaming: louder or quieter than the target is a warning (the service turns the track down,
// or it plays quieter than others), not an error; peaks above the ceiling are a warning, above
// 0 dBTP a problem. Broadcast: outside the tolerance or above the ceiling is a problem.
inline TargetVerdict evaluateTarget (const DeliveryTarget& t, std::optional<double> integrated,
                                     std::optional<double> truePeakMax) noexcept
{
    // values within 1e-4 dB of a limit count as on it (float custom values, 0.1 dB display rounding)
    constexpr double eps = 1e-4;
    TargetVerdict v;
    v.ceiling = t.peakCeiling;
    if (integrated)
    {
        v.loudnessDelta = *integrated - t.loudness;
        const bool within = std::abs (v.loudnessDelta) <= t.tolerance + eps;
        v.loudness = within ? TargetStatus::ok
                            : (t.kind == DeliveryTarget::Kind::broadcast ? TargetStatus::problem : TargetStatus::warning);
        if (t.loudMasterCeiling && v.loudnessDelta > eps)
            v.ceiling = *t.loudMasterCeiling;
    }
    if (truePeakMax)
    {
        v.peakMargin = v.ceiling - *truePeakMax;
        if (v.peakMargin >= -eps)
            v.peak = TargetStatus::ok;
        else if (t.kind == DeliveryTarget::Kind::broadcast || *truePeakMax > 0.0)
            v.peak = TargetStatus::problem;
        else
            v.peak = TargetStatus::warning;
    }
    return v;
}

} // namespace ada
