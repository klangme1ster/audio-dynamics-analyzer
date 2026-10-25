#pragma once

#include "DeliveryTargets.h"
#include "DynamicsEngine.h"
#include "FileAnalysis.h"
#include "MetricsFifo.h"
#include "MetricsHistory.h"
#include "OutputMonitor.h"
#include "PluginLayout.h"
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
    // What the editor shows (message thread): live input, a Windows output device, or a file
    enum class View { live, output, file };
    View view() const noexcept { return currentView; }
    void setView (View v) noexcept { currentView = v; }
    // Windows output capture; standalone app on Windows only, otherwise nullptr (message thread)
    ada::OutputMonitor* outputMonitor();
    // Standalone: a file given on the command line is analysed once, when the first editor opens
    bool takeCommandLineFile() noexcept { return ! std::exchange (commandLineHandled, true); }
    bool isAnalysisActive() const noexcept { return active.load(); }
    bool isFrozen() const noexcept { return frozen.load (std::memory_order_relaxed); }
    bool isLiveStopped() const noexcept { return liveStopped.load (std::memory_order_relaxed); }
    // Standalone only: play the measured input to the output (off at every start: feedback risk)
    void setMonitorInput (bool on) noexcept { monitor.store (on); }
    bool isMonitoringInput() const noexcept { return monitor.load(); }

    // Delivery target (plan 4.5): -1 = none, else an index into ada::deliveryTargets; saved with the state
    int targetIndex() const noexcept { return deliveryTarget.load(); }
    // Out-of-range indices (e.g. a state saved by a newer version) select no target
    void setTargetIndex (int index) noexcept;
    // The selected target with custom values applied, or nullopt for none
    std::optional<ada::DeliveryTarget> currentTarget() const noexcept;
    std::pair<float, float> customTarget() const noexcept { return { customLoudness.load(), customCeiling.load() }; }
    // Clamped to -40..0 LUFS and -10..0 dBTP; non-finite values fall back to the defaults
    void setCustomTarget (float loudness, float ceiling) noexcept;
    // Bumped whenever the selection or the custom values change (also by a state restore), so an
    // open editor can follow
    int targetRevision() const noexcept { return targetChanges.load(); }

    // Editor size, stored with the plugin state so it is restored next time (message thread)
    juce::Point<int> savedEditorSize() const;
    void saveEditorSize (int width, int height);
    double analysisSampleRate() const noexcept { return engine.sampleRate(); }

    // Channel layout of the live input (spec 3.11). The host's bus layout decides; a choice made in
    // the editor overrides it and is saved with the state. Without either (e.g. a bus that is not a
    // standard speaker set) nothing is measured until the user chooses.
    int liveChannels() const noexcept { return inputChannels.load(); }
    std::optional<ada::ChannelLayout> liveLayout() const;   // the layout being measured
    std::optional<ada::ChannelLayout> hostLayout() const;   // what the host's bus says
    bool hasLayoutChoice() const;
    // Message thread. nullopt follows the host; a choice for another channel count is ignored
    // until the bus has that many channels.
    void setLayoutChoice (const std::optional<ada::ChannelLayout>& layout);
    int layoutRevision() const noexcept { return layoutChanges.load(); }

    juce::AudioProcessorValueTreeState parameters;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();

    ada::DynamicsEngine engine;
    ada::MetricsFifo fifo;
    ada::MetricsHistory metricsHistory { fifo }; // message thread: drains the FIFO, keeps 90 min
    ada::FileAnalysis files;                     // offline analysis of dropped/opened files
    View currentView = View::live;
    bool commandLineHandled = false;
    std::unique_ptr<ada::OutputMonitor> output;
    std::atomic<bool> active { false };
    void configureEngine (double sampleRate); // not for the audio thread
    int handleTransport (bool& analyse, int numSamples) noexcept; // returns samples to skip
    void analyseBlock (const juce::AudioBuffer<float>& buffer) noexcept;
    void noteRestart (RestartReason reason, std::int64_t from, std::int64_t to) noexcept;

    ada::TransportTracker transport;
    double preparedRate = 0.0;
    ada::ChannelLayout preparedLayout;
    mutable juce::CriticalSection layoutLock; // message / host threads only, never the audio thread
    std::optional<ada::ChannelLayout> layoutChoice, hostBusLayout, measuredLayout;
    std::atomic<int> inputChannels { 2 }, layoutChanges { 0 };
    std::atomic<int> restartReason { 0 };
    std::atomic<std::int64_t> restartFrom { -1 }, restartTo { -1 };
    std::atomic<std::uint32_t> restartCount { 0 };
    std::atomic<bool> frozen { false }, liveStopped { false };
    std::atomic<int> editorWidth { 0 }, editorHeight { 0 }, deliveryTarget { -1 }, targetChanges { 0 };
    std::atomic<float> customLoudness { -14.0f }, customCeiling { -1.0f };
    std::atomic<bool> monitor { false }; // standalone: play input to output; never saved, off at start
    std::atomic<float>* resetOnPlay = nullptr;
    std::atomic<float>* freezeWhenStopped = nullptr;
    std::atomic<float>* clipThreshold = nullptr;
    std::atomic<float>* liveAnalysis = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaAudioProcessor)
};
