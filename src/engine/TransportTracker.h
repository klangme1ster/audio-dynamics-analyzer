#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace ada
{

// Decides whether the host's transport continues the current measurement or begins a new one
// (spec 4). Fed once per audio block; allocation-free.
//   - pause and resume at the same position: continue
//   - resume slightly *before* where measuring stopped (hosts that process ahead of playback,
//     e.g. Cubase ASIO-Guard): continue, and skip the audio that was already measured
//   - playback starts elsewhere, or the position jumps while playing (e.g. "go to start"): new
//   - a jump that lands on the loop start while cycle/loop is on (a lap): continue
//   - unknown positions: continue (losing a measurement is worse than keeping one)
class TransportTracker
{
public:
    static constexpr double jumpToleranceSeconds = 0.25;
    static constexpr double resumeBackstepSeconds = 2.0;

    struct Decision
    {
        bool startNewMeasurement = false;
        bool firstPlayback = false;      // first playback since load or a settings change
        std::int64_t from = 0, to = 0;   // expected position / actual position (samples, may be < 0)
        int skipSamples = 0;             // leading samples of this block that were already measured
    };

    // True when the play position is at the start of an active loop, i.e. a jump here is a lap.
    // Host positions in quarter notes; the tolerance is converted with the tempo.
    static bool isAtLoopStart (bool looping, std::optional<double> ppqPosition, std::optional<double> loopStartPpq,
                               std::optional<double> bpm) noexcept
    {
        if (! looping || ! ppqPosition || ! loopStartPpq || ! bpm || *bpm <= 0.0)
            return false;
        return std::abs (*ppqPosition - *loopStartPpq) * 60.0 / *bpm <= jumpToleranceSeconds;
    }

    // Forget everything, e.g. after the sample rate changed.
    void reset() noexcept
    {
        wasPlaying = false;
        havePlayEnd = false;
    }

    Decision onBlock (bool playing, std::optional<std::int64_t> time, bool atLoopStart, int numSamples,
                      double sampleRate) noexcept
    {
        Decision d;
        if (playing && time)
        {
            // Compare with where playback actually ended, not with the cursor shown while
            // stopped: some hosts move the cursor on stop.
            const auto tolerance = (std::int64_t) (jumpToleranceSeconds * sampleRate);
            const auto backstep = (std::int64_t) (resumeBackstepSeconds * sampleRate);
            const auto delta = havePlayEnd ? *time - lastPlayEnd : 0;
            const bool resumed = ! wasPlaying && havePlayEnd && delta >= -backstep && delta <= tolerance;
            const bool jumped = havePlayEnd && std::llabs (delta) > tolerance && ! resumed;

            if ((! wasPlaying && ! havePlayEnd) || (jumped && (! wasPlaying || ! atLoopStart)))
            {
                d = { true, ! havePlayEnd, lastPlayEnd, *time, 0 };
                measuredUntil = *time; // nothing of the new measurement has been measured yet
            }
            else if (jumped) // loop lap: everything played again belongs to the measurement
            {
                measuredUntil = *time;
            }

            // Audio between `time` and `measuredUntil` was already measured before a pause
            d.skipSamples = (int) std::clamp<std::int64_t> (measuredUntil - *time, 0, numSamples);
            measuredUntil = std::max (measuredUntil, *time + numSamples);
            lastPlayEnd = *time + numSamples;
            havePlayEnd = true;
        }
        wasPlaying = playing;
        return d;
    }

private:
    bool wasPlaying = false, havePlayEnd = false;
    std::int64_t lastPlayEnd = 0;   // valid only when havePlayEnd (host positions can be negative)
    std::int64_t measuredUntil = 0; // furthest position measured in the current measurement
};

} // namespace ada
