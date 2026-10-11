#pragma once

#include "HistoryStore.h"
#include "MetricsFifo.h"

#include <juce_events/juce_events.h>

namespace ada
{

// Owned by the processor, lives on the message thread: drains the audio thread's FIFO into the
// history store 30 times a second, also while the editor is closed, so nothing is lost.
class MetricsHistory : private juce::Timer
{
public:
    explicit MetricsHistory (MetricsFifo& source) : fifo (source) { startTimerHz (30); }
    ~MetricsHistory() override { stopTimer(); }

    const HistoryStore& store() const noexcept { return history; }
    void clearForReset() { history.clearForReset(); }

private:
    void timerCallback() override
    {
        fifo.drain ([this] (const DynamicsMetrics& f) { history.push (f); });
    }

    MetricsFifo& fifo;
    HistoryStore history;
};

} // namespace ada
