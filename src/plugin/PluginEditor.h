#pragma once

#include "DynamicsViews.h"
#include "HistoryGraph.h"
#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <limits>

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

// Readouts strip, scrolling history with PSR meter, zone distribution bar, controls (Phase 4).
class AdaAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AdaAudioProcessorEditor (AdaAudioProcessor&);
    ~AdaAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateFromHistory();

    enum Field { psr, psrMin, shortTerm, momentary, integrated, lra, truePeak3s, truePeakMax, plr, numFields };

    AdaAudioProcessor& processor;
    juce::TooltipWindow tooltipWindow { this, 400 };
    std::array<std::unique_ptr<Readout>, numFields> readouts;
    HistoryGraph graph;
    PsrMeter meter;
    ZoneBar zoneBar;
    juce::TextButton resetButton { "Reset" };
    juce::ToggleButton resetOnPlayToggle { "New measurement when playback jumps" };
    juce::ToggleButton freezeToggle { "Freeze while stopped" };
    juce::AudioProcessorValueTreeState::ButtonAttachment resetOnPlayAttachment, freezeAttachment;

    std::uint64_t shownVersion = 0;
    std::uint32_t shownRestartCount = 0;
    juce::String restartText; // why the current measurement started (automatic restarts only)
    juce::Rectangle<int> statusArea;
    bool frozenShown = false, activeShown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaAudioProcessorEditor)
};
