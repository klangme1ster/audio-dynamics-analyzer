#pragma once

// JSON output shared by ada_cli (raw samples) and ada_file_cli (audio files): per-bin metrics and
// the summary, as read by python/tests/test_parity.py.

#include "DynamicsEngine.h"
#include "Summary.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace ada::cli
{
struct Collect : ada::MetricsSink
{
    std::vector<ada::DynamicsMetrics> frames;
    void push (const ada::DynamicsMetrics& f) noexcept override { frames.push_back (f); }
};

inline std::string num (double v)
{
    if (std::isnan (v))
        return "NaN";
    if (std::isinf (v))
        return v > 0 ? "Infinity" : "-Infinity";
    char buf[32];
    std::snprintf (buf, sizeof buf, "%.9g", v);
    return buf;
}

inline std::string num (const std::optional<double>& v) { return v ? num (*v) : "null"; }

template <typename Get>
void array (const char* name, const std::vector<ada::DynamicsMetrics>& frames, Get get, bool last = false)
{
    std::cout << "  \"" << name << "\": [";
    for (std::size_t i = 0; i < frames.size(); ++i)
        std::cout << (i ? "," : "") << num ((double) get (frames[i]));
    std::cout << "]" << (last ? "\n" : ",\n");
}

inline void printResult (const std::vector<ada::DynamicsMetrics>& f, const ada::DynamicsEngine::Final& final,
                         int oversampling, double sampleRate, int channels)
{
    const auto s = ada::summarize (f, final);
    std::cout << "{\n  \"frames\": " << final.frames << ",\n  \"oversampling\": " << oversampling
              << ",\n  \"sample_rate\": " << num (sampleRate) << ",\n  \"channels\": " << channels << ",\n";
    array ("momentary", f, [] (const auto& x) { return x.momentaryLUFS; });
    array ("short_term", f, [] (const auto& x) { return x.shortTermLUFS; });
    array ("momentary_peak", f, [] (const auto& x) { return x.momentaryPeakLUFS; });
    array ("short_term_peak", f, [] (const auto& x) { return x.shortTermPeakLUFS; });
    array ("bin_peak_dbtp", f, [] (const auto& x) { return x.binPeakDBTP; });
    array ("true_peak_3s", f, [] (const auto& x) { return x.truePeak3sDBTP; });
    array ("psr", f, [] (const auto& x) { return x.psr; });
    std::cout << "  \"summary\": {\n"
              << "    \"integrated_lufs\": " << num (s.integrated) << ",\n"
              << "    \"lra\": " << num (s.lra) << ",\n"
              << "    \"true_peak_max_dbtp\": " << num (s.truePeakMax) << ",\n"
              << "    \"plr\": " << num (s.plr) << ",\n"
              << "    \"max_momentary_lufs\": " << num (s.maxMomentary) << ",\n"
              << "    \"max_short_term_lufs\": " << num (s.maxShortTerm) << ",\n"
              << "    \"psr_min\": " << num (s.psrMin) << ",\n"
              << "    \"psr_mean\": " << num (s.psrMean) << ",\n"
              << "    \"psr_p10\": " << num (s.psrP10) << ",\n"
              << "    \"peak_near_ceiling_pct\": " << num (s.peakNearCeilingPct) << ",\n"
              << "    \"psr_green_pct\": " << num (s.zonePct[0]) << ",\n"
              << "    \"psr_yellow_pct\": " << num (s.zonePct[1]) << ",\n"
              << "    \"psr_orange_pct\": " << num (s.zonePct[2]) << ",\n"
              << "    \"psr_red_pct\": " << num (s.zonePct[3]) << "\n"
              << "  }\n}\n";
}
} // namespace ada::cli
