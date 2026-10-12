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
    liveAnalysis = parameters.getRawParameterValue ("liveAnalysis");
}

juce::AudioProcessorValueTreeState::ParameterLayout AdaAudioProcessor::createParameters()
{
    return {
        std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "resetOnPlay", 1 },
                                                    "New measurement when playback jumps", true),
        std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "freezeWhenStopped", 1 },
                                                    "Freeze while transport is stopped", true),
        std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "liveAnalysis", 1 }, "Live analysis", true),
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
    analyseBlock (buffer);

    // In a DAW the plugin is an insert and passes audio through unchanged. The standalone app only
    // measures its input; it plays it to the output only when asked (feedback risk with speakers).
    if (wrapperType == wrapperType_Standalone && ! monitor.load (std::memory_order_relaxed))
        buffer.clear();
}

void AdaAudioProcessor::analyseBlock (const juce::AudioBuffer<float>& buffer) noexcept
{
    const bool liveOn = liveAnalysis->load (std::memory_order_relaxed) > 0.5f;
    liveStopped.store (! liveOn, std::memory_order_relaxed);
    if (! active.load (std::memory_order_relaxed))
        return;

    // The transport is tracked even while live analysis is stopped, so switching it back on
    // mid-song continues the measurement instead of looking like a position jump.
    bool analyse = true;
    const int numSamples = buffer.getNumSamples();
    const int skip = handleTransport (analyse, numSamples);
    frozen.store (! analyse, std::memory_order_relaxed);
    if (! liveOn || ! analyse || skip >= numSamples)
        return; // display keeps the last values

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

ada::OutputMonitor* AdaAudioProcessor::outputMonitor()
{
    if (output == nullptr && wrapperType == wrapperType_Standalone && ada::OutputMonitor::isSupported())
        output = std::make_unique<ada::OutputMonitor>();
    return output.get();
}

juce::Point<int> AdaAudioProcessor::savedEditorSize() const
{
    return { editorWidth.load(), editorHeight.load() };
}

void AdaAudioProcessor::saveEditorSize (int width, int height)
{
    // atomics: hosts may save the state from another thread while the window is being resized
    editorWidth.store (width);
    editorHeight.store (height);
}

juce::AudioProcessorEditor* AdaAudioProcessor::createEditor()
{
    return new AdaAudioProcessorEditor (*this);
}

void AdaAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("editorWidth", editorWidth.load(), nullptr);
    state.setProperty ("editorHeight", editorHeight.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void AdaAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (parameters.state.getType()))
        {
            editorWidth.store (xml->getIntAttribute ("editorWidth", 0));
            editorHeight.store (xml->getIntAttribute ("editorHeight", 0));
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AdaAudioProcessor();
}
