#pragma once

#include "DynamicsEngine.h"
#include "HistoryStore.h"
#include "MetricsFifo.h"
#include "Summary.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ada
{

// Offline analysis of an audio file at full CPU speed (plan 3.6, spec 4). Owned by the processor
// so results survive the editor being closed. A worker thread decodes the file and runs its own
// DynamicsEngine; frames reach the message thread through a lock-free FIFO and fill a
// HistoryStore for the graph plus a complete frame list for the exact per-file summary.
class FileAnalysis : private juce::Timer
{
public:
    enum class State { idle, running, finished, failed };

    FileAnalysis();
    ~FileAnalysis() override;

    // Message thread. Cancels any running analysis. Returns false (and sets errorText) if the
    // file cannot be opened or is not mono/stereo at a supported sample rate.
    bool start (const juce::File& file);
    void cancel();

    State state() const noexcept { return currentState; }
    double progress() const noexcept { return workerProgress.load (std::memory_order_relaxed); }
    const juce::String& errorText() const noexcept { return error; }
    const juce::File& file() const noexcept { return currentFile; }
    double durationSeconds() const noexcept { return duration; }
    double sampleRate() const noexcept { return rate; }
    int channels() const noexcept { return numChannels; }
    double analysisSeconds() const noexcept { return elapsedSeconds; }

    const HistoryStore* store() const noexcept { return history.get(); }
    const std::optional<Summary>& summary() const noexcept { return result; }

    // Increments on every visible change (new frames, state change)
    std::uint64_t version() const noexcept { return changeCount; }
    // Increments with every start(), so views can reset (zoom, scroll) for a new file
    std::uint64_t runId() const noexcept { return runCount; }

    bool isSupportedFile (const juce::File& f) const;

    // Decodes `reader` in blocks and feeds the engine. Blocking; used by the worker and by tests.
    // `sink` receives every frame; returns the engine's final values, or nullopt if cancelled or
    // if decoding failed (then *decodeFailed is set).
    static std::optional<DynamicsEngine::Final> analyseReader (juce::AudioFormatReader& reader,
                                                               MetricsSink& sink,
                                                               const std::function<bool()>& shouldStop,
                                                               std::atomic<double>* progress = nullptr,
                                                               bool* decodeFailed = nullptr);

private:
    class Worker;
    void timerCallback() override;
    void drain();

    juce::AudioFormatManager formats;
    MetricsFifo fifo;
    std::unique_ptr<Worker> worker;
    std::unique_ptr<HistoryStore> history;
    std::vector<DynamicsMetrics> allFrames;
    std::optional<Summary> result;
    std::atomic<double> workerProgress { 0.0 };

    State currentState = State::idle;
    juce::File currentFile;
    juce::String error;
    double duration = 0.0, rate = 0.0, elapsedSeconds = 0.0;
    int numChannels = 0;
    double startTime = 0.0;
    std::uint64_t changeCount = 0, runCount = 0;
};

} // namespace ada
