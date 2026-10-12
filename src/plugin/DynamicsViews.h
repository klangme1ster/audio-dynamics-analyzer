#pragma once

#include "DeliveryTargets.h"
#include "HistoryStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

// Vertical PSR meter (plan 4.3): current PSR as a bar coloured by its zone, with markers for the
// lowest PSR and the 10th percentile since reset.
class PsrMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    PsrMeter();
    void setValues (std::optional<float> current, std::optional<float> minimum, std::optional<float> p10);
    void setTitle (const juce::String& newTitle);
    void restoreDefaultTooltip();
    void paint (juce::Graphics&) override;

private:
    std::optional<float> current, minimum, p10;
    juce::String title { "PSR now (dB)" };
};

// Stacked bar (plan 4.4): share of the measured time spent in each PSR zone since reset.
class ZoneBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    ZoneBar();
    void setStats (const ada::HistoryStore::PsrStats& stats);
    void setTitle (const juce::String& newTitle);
    void restoreDefaultTooltip();
    void paint (juce::Graphics&) override;

private:
    ada::HistoryStore::PsrStats stats;
    juce::String title { "Time in each PSR zone since reset" };
};

// Delivery target check (plan 4.5): a target selector and two verdict lines (loudness, true peak),
// each with an icon and text so the status never depends on colour alone.
class TargetPanel : public juce::Component, public juce::SettableTooltipClient
{
public:
    TargetPanel();

    std::function<void (int)> onSelect; // -1 = no target, else an index into ada::deliveryTargets

    void setSelection (int index, const std::optional<ada::DeliveryTarget>& target);
    void setMeasurement (std::optional<double> integrated, std::optional<double> truePeakMax);
    std::optional<ada::TargetVerdict> verdict() const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::String loudnessText (const ada::TargetVerdict&) const;
    juce::String peakText (const ada::TargetVerdict&) const;
    juce::String shortLoudnessText (const ada::TargetVerdict&) const;
    juce::String shortPeakText (const ada::TargetVerdict&) const;
    void updateTooltip();

    juce::ComboBox box;
    static constexpr int editCustomId = 1000; // "Edit custom target..." item
    std::optional<ada::DeliveryTarget> target;
    std::optional<double> integrated, truePeakMax;
};
