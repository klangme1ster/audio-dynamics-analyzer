#include "PluginProcessor.h"

#include "PluginEditor.h"

#include <array>

AdaAudioProcessor::AdaAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "ADA", createParameters())
{
    resetOnPlay = parameters.getRawParameterValue ("resetOnPlay");
    freezeWhenStopped = parameters.getRawParameterValue ("freezeWhenStopped");
    clipThreshold = parameters.getRawParameterValue ("clipThreshold");
}

juce::AudioProcessorValueTreeState::ParameterLayout AdaAudioProcessor::createParameters()
{
    return {
        std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "resetOnPlay", 1 },
                                                    "New measurement when playback jumps", true),
        std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "freezeWhenStopped", 1 },
                                                    "Freeze while transport is stopped", true),
        std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "clipThreshold", 1 },
                                                     "Clip threshold",
                                                     juce::NormalisableRange<float> (-6.0f, 3.0f, 0.1f), 0.0f,
                                                     juce::AudioParameterFloatAttributes().withLabel ("dBTP")),
    };
}

void AdaAudioProcessor::prepareToPlay (double sampleRate, int)
{
    setLatencySamples (0); // pass-through: metering latency does not delay the audio
    const int channels = juce::jlimit (1, ada::DynamicsEngine::maxChannels, getTotalNumInputChannels());
    const bool supported = ada::DynamicsEngine::supportsSampleRate (sampleRate);

    // Hosts may deactivate and reactivate plugins around transport changes (e.g. Cubase suspending
    // processing). With unchanged settings the running measurement simply continues.
    if (supported && active.load() && sampleRate == preparedRate && channels == preparedChannels)
        return;

    const bool hadMeasurement = preparedRate > 0.0;
    if (supported)
    {
        engine.prepare (sampleRate, channels);
        preparedRate = sampleRate;
        preparedChannels = channels;
        if (hadMeasurement)
            noteRestart (RestartReason::settingsChanged, -1, -1);
    }
    active = supported;
    transport.reset();
}

void AdaAudioProcessor::releaseResources() {}

bool AdaAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainOutputChannelSet() == in;
}

void AdaAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (! active.load (std::memory_order_relaxed))
        return;

    bool analyse = true;
    const int numSamples = buffer.getNumSamples();
    const int skip = handleTransport (analyse, numSamples);
    frozen.store (! analyse, std::memory_order_relaxed);
    if (! analyse || skip >= numSamples)
        return; // display keeps the last values; the audio still passes through unchanged

    // After a resume slightly before the pause point, audio that was already measured is skipped
    std::array<const float*, ada::DynamicsEngine::maxChannels> channels {};
    for (int ch = 0; ch < engine.channels(); ++ch)
        channels[(size_t) ch] = buffer.getReadPointer (ch, skip);
    engine.setClipThreshold (clipThreshold->load (std::memory_order_relaxed));
    engine.process (channels.data(), numSamples - skip, fifo);
}

// The standalone app and hosts without a play head have no meaningful transport: always analyse.
// Pausing and resuming at the same position continues the measurement; starting playback
// anywhere else (e.g. back at the song start) begins a new one.
int AdaAudioProcessor::handleTransport (bool& analyse, int numSamples) noexcept
{
    if (wrapperType == wrapperType_Standalone)
        return 0; // JUCE's standalone wrapper reports a transport that never plays
    auto* head = getPlayHead();
    if (head == nullptr)
        return 0;
    const auto pos = head->getPosition();
    if (! pos)
        return 0;

    const bool playing = pos->getIsPlaying();
    const auto hostTime = pos->getTimeInSamples();
    const auto time = hostTime ? std::optional<std::int64_t> (*hostTime) : std::nullopt;
    const auto loop = pos->getLoopPoints();
    const auto toStd = [] (const auto& o) { return o ? std::optional<double> (*o) : std::nullopt; };
    const bool atLoopStart = ada::TransportTracker::isAtLoopStart (
        pos->getIsLooping(), toStd (pos->getPpqPosition()),
        loop ? std::optional<double> (loop->ppqStart) : std::nullopt, toStd (pos->getBpm()));
    const auto decision = transport.onBlock (playing, time, atLoopStart, numSamples, getSampleRate());
    if (decision.startNewMeasurement && resetOnPlay->load (std::memory_order_relaxed) > 0.5f)
    {
        engine.requestReset();
        if (! decision.firstPlayback) // the very first playback is not worth a message
            noteRestart (RestartReason::playbackJump, decision.from, decision.to);
    }

    if (! playing && freezeWhenStopped->load (std::memory_order_relaxed) > 0.5f)
        analyse = false;
    return decision.skipSamples;
}

void AdaAudioProcessor::noteRestart (RestartReason reason, std::int64_t from, std::int64_t to) noexcept
{
    restartFrom.store (from, std::memory_order_relaxed);
    restartTo.store (to, std::memory_order_relaxed);
    restartReason.store ((int) reason, std::memory_order_relaxed);
    restartCount.fetch_add (1, std::memory_order_release);
}

AdaAudioProcessor::RestartInfo AdaAudioProcessor::lastRestart() const noexcept
{
    RestartInfo info;
    info.count = restartCount.load (std::memory_order_acquire);
    info.reason = (RestartReason) restartReason.load (std::memory_order_relaxed);
    info.from = restartFrom.load (std::memory_order_relaxed);
    info.to = restartTo.load (std::memory_order_relaxed);
    return info;
}

juce::AudioProcessorEditor* AdaAudioProcessor::createEditor()
{
    return new AdaAudioProcessorEditor (*this);
}

void AdaAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AdaAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AdaAudioProcessor();
}
