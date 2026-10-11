#pragma once

#include "HistoryStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

// Vertical PSR meter (plan 4.3): current PSR as a bar coloured by its zone, with markers for the
// lowest PSR and the 10th percentile since reset.
class PsrMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    PsrMeter();
    void setValues (std::optional<float> current, std::optional<float> minimum, std::optional<float> p10);
    void paint (juce::Graphics&) override;

private:
    std::optional<float> current, minimum, p10;
};

// Stacked bar (plan 4.4): share of the measured time spent in each PSR zone since reset.
class ZoneBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    ZoneBar();
    void setStats (const ada::HistoryStore::PsrStats& stats);
    void paint (juce::Graphics&) override;

private:
    ada::HistoryStore::PsrStats stats;
};
