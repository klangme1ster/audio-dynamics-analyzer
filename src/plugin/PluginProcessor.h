#pragma once

#include "DynamicsEngine.h"
#include "LiveStats.h"
#include "MetricsFifo.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

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
    void requestReset() noexcept { engine.requestReset(); }
    ada::MetricsFifo& metrics() noexcept { return fifo; }
    bool isAnalysisActive() const noexcept { return active.load(); }
    bool isFrozen() const noexcept { return frozen.load (std::memory_order_relaxed); }
    float psrMinimum() const noexcept { return liveStats.psrMinimum(); }
    double analysisSampleRate() const noexcept { return engine.sampleRate(); }

    juce::AudioProcessorValueTreeState parameters;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();

    ada::DynamicsEngine engine;
    ada::MetricsFifo fifo;
    ada::LiveStatsSink liveStats { fifo };
    std::atomic<bool> active { false };
    void handleTransport (bool& analyse, int numSamples) noexcept;

    bool wasPlaying = false;
    std::int64_t lastPlayEnd = -1; // host timeline position just after the last played block
    std::atomic<bool> frozen { false };
    std::atomic<float>* resetOnPlay = nullptr;
    std::atomic<float>* freezeWhenStopped = nullptr;
    std::atomic<float>* clipThreshold = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaAudioProcessor)
};
