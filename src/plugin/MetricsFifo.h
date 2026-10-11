#pragma once

#include "DynamicsEngine.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>

namespace ada
{

// Single-producer / single-consumer queue from the audio thread to the UI (spec 4).
// push() never blocks or allocates; when full the frame is dropped and counted.
class MetricsFifo : public MetricsSink
{
public:
    static constexpr int capacity = 1024; // about 100 s of frames at 10 Hz

    void push (const DynamicsMetrics& frame) noexcept override
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite (1, start1, size1, start2, size2);
        if (size1 + size2 == 0)
        {
            dropped.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        buffer[(size_t) (size1 > 0 ? start1 : start2)] = frame;
        fifo.finishedWrite (1);
    }

    // UI thread: calls fn(frame) for every queued frame, oldest first.
    template <typename Fn>
    void drain (Fn&& fn)
    {
        int start1, size1, start2, size2;
        fifo.prepareToRead (fifo.getNumReady(), start1, size1, start2, size2);
        for (int i = 0; i < size1; ++i)
            fn (buffer[(size_t) (start1 + i)]);
        for (int i = 0; i < size2; ++i)
            fn (buffer[(size_t) (start2 + i)]);
        fifo.finishedRead (size1 + size2);
    }

    void clear() noexcept { fifo.reset(); }
    std::uint32_t droppedFrames() const noexcept { return dropped.load (std::memory_order_relaxed); }

private:
    juce::AbstractFifo fifo { capacity };
    std::array<DynamicsMetrics, capacity> buffer {};
    std::atomic<std::uint32_t> dropped { 0 };
};

} // namespace ada
