#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace ada
{

// Decides whether the host's transport continues the current measurement or begins a new one
// (spec 4). Fed once per audio block; allocation-free.
//   - pause and resume at the same position: continue
//   - playback starts elsewhere, or the position jumps while playing (e.g. "go to start"): new
//   - a jump that lands on the loop start while cycle/loop is on (a lap): continue
//   - unknown positions: continue (losing a measurement is worse than keeping one)
class TransportTracker
{
public:
    static constexpr double jumpToleranceSeconds = 0.25;

    struct Decision
    {
        bool startNewMeasurement = false;
        bool firstPlayback = false;      // first playback since load or a settings change
        std::int64_t from = 0, to = 0;   // expected position / actual position (samples, may be < 0)
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
            const bool jumped = havePlayEnd && std::llabs (*time - lastPlayEnd) > tolerance;
            if ((! wasPlaying && (! havePlayEnd || jumped)) || (wasPlaying && jumped && ! atLoopStart))
                d = { true, ! havePlayEnd, lastPlayEnd, *time };
            lastPlayEnd = *time + numSamples;
            havePlayEnd = true;
        }
        wasPlaying = playing;
        return d;
    }

private:
    bool wasPlaying = false, havePlayEnd = false;
    std::int64_t lastPlayEnd = 0; // valid only when havePlayEnd (host positions can be negative)
};

} // namespace ada
