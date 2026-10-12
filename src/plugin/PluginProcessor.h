#pragma once

#include "DynamicsEngine.h"
#include "FileAnalysis.h"
#include "MetricsFifo.h"
#include "MetricsHistory.h"
#include "TransportTracker.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <utility>

// Pass-through analyzer: audio is unchanged, metrics go to the UI through a lock-free FIFO.
class AdaAudioProcessor : public juce::AudioProcessor
{
public:
    AdaAudioProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // UI thread interface
    void requestReset() noexcept
    {
        engine.requestReset();
        noteRestart (RestartReason::button, -1, -1);
    }

    // Why the current measurement started (shown in the editor's status line)
    enum class RestartReason : int { none, button, playbackJump, settingsChanged };
    struct RestartInfo
    {
        RestartReason reason = RestartReason::none;
        std::int64_t from = -1, to = -1; // host positions in samples (playbackJump)
        std::uint32_t count = 0;         // increments on every restart
    };
    RestartInfo lastRestart() const noexcept;
    ada::MetricsHistory& history() noexcept { return metricsHistory; }
    ada::FileAnalysis& fileAnalysis() noexcept { return files; }
    // Whether the editor shows the analysed file instead of live metering (message thread)
    bool isShowingFile() const noexcept { return showingFile; }
    void setShowingFile (bool shouldShow) noexcept { showingFile = shouldShow; }
    // Standalone: a file given on the command line is analysed once, when the first editor opens
    bool takeCommandLineFile() noexcept { return ! std::exchange (commandLineHandled, true); }
    bool isAnalysisActive() const noexcept { return active.load(); }
    bool isFrozen() const noexcept { return frozen.load (std::memory_order_relaxed); }
    double analysisSampleRate() const noexcept { return engine.sampleRate(); }

    juce::AudioProcessorValueTreeState parameters;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();

    ada::DynamicsEngine engine;
    ada::MetricsFifo fifo;
    ada::MetricsHistory metricsHistory { fifo }; // message thread: drains the FIFO, keeps 90 min
    ada::FileAnalysis files;                     // offline analysis of dropped/opened files
    bool showingFile = false, commandLineHandled = false;
    std::atomic<bool> active { false };
    int handleTransport (bool& analyse, int numSamples) noexcept; // returns samples to skip
    void noteRestart (RestartReason reason, std::int64_t from, std::int64_t to) noexcept;

    ada::TransportTracker transport;
    double preparedRate = 0.0;
    int preparedChannels = 0;
    std::atomic<int> restartReason { 0 };
    std::atomic<std::int64_t> restartFrom { -1 }, restartTo { -1 };
    std::atomic<std::uint32_t> restartCount { 0 };
    std::atomic<bool> frozen { false };
    std::atomic<float>* resetOnPlay = nullptr;
    std::atomic<float>* freezeWhenStopped = nullptr;
    std::atomic<float>* clipThreshold = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaAudioProcessor)
};
