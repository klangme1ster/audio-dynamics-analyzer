#pragma once

#include "DynamicsEngine.h"
#include "Summary.h"

#include <atomic>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

namespace adatest
{

// Set by tests around code that must not allocate; counted by the global operator new/delete.
extern thread_local bool countAllocations;
extern std::atomic<long> allocationCount;

constexpr double silence = -1000.0;

struct Signal
{
    double fs = 48000.0;
    std::vector<std::vector<float>> channels; // planar

    std::size_t frames() const { return channels.empty() ? 0 : channels[0].size(); }
    std::vector<const float*> pointers (std::size_t offset = 0) const
    {
        std::vector<const float*> p;
        for (const auto& c : channels)
            p.push_back (c.data() + offset);
        return p;
    }
    Signal slice (std::size_t from) const
    {
        Signal s { fs, {} };
        for (const auto& c : channels)
            s.channels.emplace_back (c.begin() + (std::ptrdiff_t) from, c.end());
        return s;
    }
};

inline double dbToAmp (double db) { return db <= silence ? 0.0 : std::pow (10.0, db / 20.0); }

// Concatenated (seconds, peak level dBFS) tone segments with continuous phase (Tech 3341 style)
inline Signal toneSequence (double fs, const std::vector<std::pair<double, double>>& segments,
                            int numChannels = 2, double freq = 1000.0)
{
    Signal s { fs, std::vector<std::vector<float>> ((std::size_t) numChannels) };
    std::size_t n = 0;
    for (auto [seconds, level] : segments)
    {
        const auto count = (std::size_t) std::llround (seconds * fs);
        const double amp = dbToAmp (level);
        for (std::size_t i = 0; i < count; ++i, ++n)
        {
            const auto v = (float) (amp * std::sin (2.0 * std::numbers::pi * freq * (double) n / fs));
            for (auto& c : s.channels)
                c.push_back (v);
        }
    }
    return s;
}

// Sine at fs/div with linear amplitude and phase, raised-cosine fades (Tech 3341 cases 15-19)
inline Signal peakSine (double fs, double div, double amplitude, double phaseDeg, double seconds,
                        double fadeSeconds = 0.05, int numChannels = 2)
{
    const auto count = (std::size_t) std::llround (seconds * fs);
    const auto fade = (std::size_t) std::llround (fadeSeconds * fs);
    Signal s { fs, std::vector<std::vector<float>> ((std::size_t) numChannels, std::vector<float> (count)) };
    for (std::size_t i = 0; i < count; ++i)
    {
        double g = 1.0;
        if (i < fade)
            g = 0.5 - 0.5 * std::cos (std::numbers::pi * (double) i / (double) fade);
        else if (i >= count - fade)
            g = 0.5 - 0.5 * std::cos (std::numbers::pi * (double) (count - 1 - i) / (double) fade);
        const auto v = (float) (g * amplitude * std::sin (2.0 * std::numbers::pi * (double) i / div
                                                          + phaseDeg * std::numbers::pi / 180.0));
        for (auto& c : s.channels)
            c[i] = v;
    }
    return s;
}

struct CollectingSink : ada::MetricsSink
{
    std::vector<ada::DynamicsMetrics> frames;
    void push (const ada::DynamicsMetrics& f) noexcept override { frames.push_back (f); }
};

struct Run
{
    std::vector<ada::DynamicsMetrics> frames;
    ada::DynamicsEngine::Final final {};
    ada::Summary summary;
};

inline Run analyze (const Signal& s, int blockSize = 4096)
{
    ada::DynamicsEngine engine;
    engine.prepare (s.fs, (int) s.channels.size());
    CollectingSink sink;
    for (std::size_t pos = 0; pos < s.frames(); pos += (std::size_t) blockSize)
    {
        const auto n = (int) std::min<std::size_t> ((std::size_t) blockSize, s.frames() - pos);
        engine.process (s.pointers (pos).data(), n, sink);
    }
    Run r;
    r.final = engine.finish (sink);
    r.frames = std::move (sink.frames);
    r.summary = ada::summarize (r.frames, r.final);
    return r;
}

} // namespace adatest
