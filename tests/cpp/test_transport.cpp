// Pause/resume vs. new measurement decisions (spec 4, "Reset" and "Freeze while stopped").
#include <doctest/doctest.h>

#include "TransportTracker.h"

#include <optional>

namespace
{
constexpr double fs = 48000.0;
constexpr int block = 512;

// Plays `blocks` blocks starting at `start`; returns true if any block asked for a new measurement.
bool play (ada::TransportTracker& t, std::int64_t start, int blocks, bool atLoopStart = false)
{
    bool restarted = false;
    for (int i = 0; i < blocks; ++i)
        restarted |= t.onBlock (true, start + (std::int64_t) i * block, atLoopStart && i == 0, block, fs).startNewMeasurement;
    return restarted;
}

void stopAt (ada::TransportTracker& t, std::int64_t cursor, int blocks = 50)
{
    for (int i = 0; i < blocks; ++i)
        t.onBlock (false, cursor, false, block, fs);
}
} // namespace

TEST_CASE ("First playback starts a measurement")
{
    ada::TransportTracker t;
    stopAt (t, 0);
    CHECK (play (t, 0, 10));
}

TEST_CASE ("Pause and resume in place continues (Cubase space bar)")
{
    ada::TransportTracker t;
    play (t, 0, 1000);
    const std::int64_t end = 1000 * block;
    stopAt (t, end);
    CHECK_FALSE (play (t, end, 100));
}

TEST_CASE ("Resume with a small host offset still continues")
{
    ada::TransportTracker t;
    play (t, 0, 1000);
    const std::int64_t end = 1000 * block;
    stopAt (t, end);
    CHECK_FALSE (play (t, end - 4800, 10)); // 100 ms earlier, e.g. latency compensation
    stopAt (t, end);
    CHECK_FALSE (play (t, end + 10 * block + 4800, 10));
}

TEST_CASE ("Starting elsewhere begins a new measurement")
{
    ada::TransportTracker t;
    play (t, 0, 1000);
    stopAt (t, 1000 * block);
    const auto d = [&] {
        ada::TransportTracker::Decision last;
        for (int i = 0; i < 5; ++i)
        {
            const auto now = t.onBlock (true, (std::int64_t) i * block, false, block, fs);
            if (now.startNewMeasurement)
                last = now;
        }
        return last;
    }();
    CHECK (d.startNewMeasurement);
    CHECK (d.from == 1000 * block);
    CHECK (d.to == 0);
}

TEST_CASE ("Cursor moved while stopped, playback resumes where it ended: continues")
{
    ada::TransportTracker t;
    play (t, 0, 1000);
    stopAt (t, 0); // host shows the cursor at the start while stopped
    CHECK_FALSE (play (t, 1000 * block, 10));
}

TEST_CASE ("Jumping while playing begins a new measurement (e.g. Cubase numpad 1: go to start)")
{
    ada::TransportTracker t;
    play (t, 0, 1000);
    CHECK (play (t, 0, 10)); // position jumped back without stopping
    CHECK_FALSE (play (t, 10 * block, 10)); // ...and then plays on normally
}

TEST_CASE ("Cycle/loop wrap-around while playing continues")
{
    ada::TransportTracker t;
    play (t, 0, 100, true);
    CHECK_FALSE (play (t, 0, 100, true));
    CHECK_FALSE (play (t, 0, 100, true));
}

TEST_CASE ("Small irregularities while playing do not restart")
{
    ada::TransportTracker t;
    play (t, 0, 100);
    CHECK_FALSE (play (t, 100 * block + 2400, 100)); // 50 ms off
}

TEST_CASE ("Cycle on, but a jump that does not land on the loop start restarts")
{
    // Cubase reports "looping" whenever the Cycle button is on, even outside the cycle range
    ada::TransportTracker t;
    play (t, 0, 1000);
    CHECK (play (t, 0, 10, false));
    CHECK (ada::TransportTracker::isAtLoopStart (true, 16.05, 16.0, 120.0));   // 25 ms: a lap
    CHECK_FALSE (ada::TransportTracker::isAtLoopStart (true, 0.0, 16.0, 120.0)); // 8 s away
    CHECK_FALSE (ada::TransportTracker::isAtLoopStart (false, 16.0, 16.0, 120.0));
    CHECK_FALSE (ada::TransportTracker::isAtLoopStart (true, 16.0, std::nullopt, 120.0));
}

TEST_CASE ("Negative positions (pre-roll, count-in) behave like any other")
{
    ada::TransportTracker t;
    const std::int64_t start = -48000;
    const auto first = t.onBlock (true, start, false, block, fs);
    CHECK (first.startNewMeasurement);
    CHECK (first.firstPlayback);
    play (t, start + block, 99);
    const std::int64_t end = start + 100 * block;
    stopAt (t, end);
    CHECK_FALSE (play (t, end, 10));                 // pause/resume at a negative position
    CHECK (play (t, 60 * 48000, 10));                // jump from the negative region while playing
}

TEST_CASE ("Resume slightly before the stop point (Cubase ASIO-Guard) continues and skips measured audio")
{
    ada::TransportTracker t;
    play (t, 0, 1000);
    const std::int64_t measuredEnd = 1000 * block;
    const std::int64_t heard = measuredEnd - 24000; // processed 0.5 s ahead of what was audible
    stopAt (t, heard);

    int skipped = 0;
    bool restarted = false;
    for (int i = 0; i < 100; ++i)
    {
        const auto d = t.onBlock (true, heard + (std::int64_t) i * block, false, block, fs);
        restarted |= d.startNewMeasurement;
        skipped += d.skipSamples;
        CHECK (d.skipSamples >= 0);
        CHECK (d.skipSamples <= block);
    }
    CHECK_FALSE (restarted);
    CHECK (skipped == 24000); // exactly the audio measured before the pause, nothing more
}

TEST_CASE ("Resume far before the stop point is a jump")
{
    ada::TransportTracker t;
    play (t, 0, 1000);
    stopAt (t, 1000 * block);
    CHECK (play (t, 1000 * block - 3 * 48000, 10)); // 3 s back
}

TEST_CASE ("Resume after the stop point is a jump")
{
    ada::TransportTracker t;
    play (t, 0, 1000);
    stopAt (t, 1000 * block);
    CHECK (play (t, 1000 * block + 24000, 10)); // 0.5 s forward
}

TEST_CASE ("A new measurement never skips audio")
{
    ada::TransportTracker t;
    play (t, 0, 1000);
    stopAt (t, 1000 * block);
    const auto d = t.onBlock (true, 0, false, block, fs); // back to the start
    CHECK (d.startNewMeasurement);
    CHECK (d.skipSamples == 0);
}

TEST_CASE ("Hosts without position information continue")
{
    ada::TransportTracker t;
    t.onBlock (true, std::nullopt, false, block, fs);
    t.onBlock (false, std::nullopt, false, block, fs);
    CHECK_FALSE (t.onBlock (true, std::nullopt, false, block, fs).startNewMeasurement);
}
