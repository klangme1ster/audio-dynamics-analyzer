#include "PluginProcessor.h"

#include "PluginEditor.h"

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
                                                    "Reset when playback starts at a new position", true),
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
    const int channels = juce::jlimit (1, ada::DynamicsEngine::maxChannels, getTotalNumInputChannels());
    const bool supported = ada::DynamicsEngine::supportsSampleRate (sampleRate);
    if (supported)
        engine.prepare (sampleRate, channels);
    active = supported;
    fifo.clear();
    wasPlaying = false;
    lastPlayEnd = -1;
    setLatencySamples (0); // pass-through: metering latency does not delay the audio
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
    handleTransport (analyse, buffer.getNumSamples());
    frozen.store (! analyse, std::memory_order_relaxed);
    if (! analyse)
        return; // display keeps the last values; the audio still passes through unchanged

    engine.setClipThreshold (clipThreshold->load (std::memory_order_relaxed));
    engine.process (buffer.getArrayOfReadPointers(), buffer.getNumSamples(), liveStats);
}

// The standalone app and hosts without a play head have no meaningful transport: always analyse.
// Pausing and resuming at the same position continues the measurement; starting playback
// anywhere else (e.g. back at the song start) begins a new one.
void AdaAudioProcessor::handleTransport (bool& analyse, int numSamples) noexcept
{
    if (wrapperType == wrapperType_Standalone)
        return; // JUCE's standalone wrapper reports a transport that never plays
    auto* head = getPlayHead();
    if (head == nullptr)
        return;
    const auto pos = head->getPosition();
    if (! pos)
        return;

    const bool playing = pos->getIsPlaying();
    const auto time = pos->getTimeInSamples();
    if (playing && ! wasPlaying)
    {
        // Compare with where playback actually ended, not with the stopped cursor: many hosts
        // jump back to the start position on stop (REAPER default, Cubase/Studio One option).
        const auto tolerance = (std::int64_t) (0.25 * getSampleRate());
        const bool resumedInPlace = time && lastPlayEnd >= 0 && std::abs (*time - lastPlayEnd) <= tolerance;
        if (! resumedInPlace && resetOnPlay->load (std::memory_order_relaxed) > 0.5f)
            engine.requestReset();
    }
    if (playing)
        lastPlayEnd = time ? *time + numSamples : -1;
    wasPlaying = playing;

    if (! playing && freezeWhenStopped->load (std::memory_order_relaxed) > 0.5f)
        analyse = false;
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
