#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <limits>
#include <optional>

// One numeric readout with a hover explanation.
class Readout : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Kind { decibel, loudness };

    Readout (juce::String label, juce::String unit, Kind kind, const juce::String& help);

    void setValue (float newValue, bool alarm = false);
    void paint (juce::Graphics&) override;

private:
    juce::String label, unit;
    Kind kind;
    float value = std::numeric_limits<float>::quiet_NaN();
    bool alarm = false;
};

// Phase 3 editor: numeric readouts with tooltips. The full GUI is Phase 4.
class AdaAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AdaAudioProcessorEditor (AdaAudioProcessor&);
    ~AdaAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    enum Field { psr, psrMin, shortTerm, momentary, integrated, lra, truePeak3s, truePeakMax, plr, numFields };

    AdaAudioProcessor& processor;
    juce::TooltipWindow tooltipWindow { this, 400 };
    std::array<std::unique_ptr<Readout>, numFields> readouts;
    juce::TextButton resetButton { "Reset" };
    juce::ToggleButton resetOnPlayToggle { "Reset when playback starts elsewhere" };
    juce::ToggleButton freezeToggle { "Freeze while stopped" };
    juce::AudioProcessorValueTreeState::ButtonAttachment resetOnPlayAttachment, freezeAttachment;

    std::uint32_t epoch = 0;                         // epoch of the last frame received
    std::optional<std::uint32_t> resetPendingEpoch;  // set by Reset until a newer epoch arrives
    bool frozenShown = false, activeShown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaAudioProcessorEditor)
};
