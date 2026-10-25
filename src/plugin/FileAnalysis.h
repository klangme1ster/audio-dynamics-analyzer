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
    // file cannot be opened, has more than two channels without a known layout (needsLayout(),
    // suggestedLayout()) or has an unsupported sample rate. `layout` is the user's choice (spec
    // 3.11); it overrides what the file says and must match the channel count.
    bool start (const juce::File& file, const std::optional<ChannelLayout>& layout = std::nullopt);
    void cancel();

    State state() const noexcept { return currentState; }
    double progress() const noexcept { return workerProgress.load (std::memory_order_relaxed); }
    const juce::String& errorText() const noexcept { return error; }
    const juce::File& file() const noexcept { return currentFile; }
    double durationSeconds() const noexcept { return duration; }
    double sampleRate() const noexcept { return rate; }
    int channels() const noexcept { return numChannels; } // of the last file opened, also if it was refused
    const ChannelLayout& layout() const noexcept { return fileLayout; } // of the last successful start()
    // After a failed start(): the file has 3+ channels and no layout information. suggestedLayout()
    // is the educated guess for the channel count (never applied without the user's confirmation).
    bool needsLayout() const noexcept { return layoutNeeded; }
    const std::optional<ChannelLayout>& suggestedLayout() const noexcept { return layoutSuggestion; }
    double analysisSeconds() const noexcept { return elapsedSeconds; }

    const HistoryStore* store() const noexcept { return history.get(); }
    const std::optional<Summary>& summary() const noexcept { return result; }

    // Increments on every visible change (new frames, state change)
    std::uint64_t version() const noexcept { return changeCount; }
    // Increments with every start(), so views can reset (zoom, scroll) for a new file
    std::uint64_t runId() const noexcept { return runCount; }

    bool isSupportedFile (const juce::File& f) const;
    // File-chooser pattern for every readable format, e.g. "*.wav;*.flac;...;*.m4a"
    juce::String wildcard() const { return formats.getWildcardForAllFormats(); }

    // The layout the file itself declares: the speaker mask a reader reports as "ChannelMask" (a
    // WAVE_FORMAT_EXTENSIBLE WAV, M4A / AAC through Media Foundation), or mono / stereo for one or
    // two channels. nullopt if there is none, or it is not 5.0 / 5.1 / 7.1. JUCE's own guess
    // (5.1 for any six channels) is not trusted.
    static std::optional<ChannelLayout> detectLayout (juce::AudioFormatReader& reader);

    // Decodes `reader` in blocks and feeds the engine. Blocking; used by the worker and by tests.
    // `sink` receives every frame; returns the engine's final values, or nullopt if cancelled or
    // if decoding failed (then *decodeFailed is set). The layout is `layout`, else the detected
    // one; std::invalid_argument if there is none or it does not fit the reader.
    static std::optional<DynamicsEngine::Final> analyseReader (juce::AudioFormatReader& reader,
                                                               MetricsSink& sink,
                                                               const std::function<bool()>& shouldStop,
                                                               std::atomic<double>* progress = nullptr,
                                                               bool* decodeFailed = nullptr,
                                                               const std::optional<ChannelLayout>& layout = std::nullopt);

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
    ChannelLayout fileLayout = ChannelLayout::stereo();
    bool layoutNeeded = false;
    std::optional<ChannelLayout> layoutSuggestion;
    double startTime = 0.0;
    std::uint64_t changeCount = 0, runCount = 0;
};

} // namespace ada
