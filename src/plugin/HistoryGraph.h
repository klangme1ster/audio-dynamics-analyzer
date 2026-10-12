#pragma once

#include "HistoryStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <optional>

// Scrolling history (plan 4.2): short-term loudness and 3 s true peak on top, PSR over its zone
// bands below, one shared time axis. Wheel = zoom, drag = scroll back, double-click = live.
class HistoryGraph : public juce::Component
{
public:
    explicit HistoryGraph (const ada::HistoryStore& store);

    // Switch the data source (live history or a file analysis) and reset the view.
    void setStore (const ada::HistoryStore& newStore);
    // Zoom out to show everything in the store (e.g. a finished file).
    void showAll();
    // Replaces the "Live, last ..." text in the header (empty = default).
    void setStatusText (const juce::String& text);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    static constexpr double minSpan = 10.0, maxSpan = 6.0 * 3600.0;
    // Zooming out further than the store can hold would only show empty space
    static double maxSpanFor (const ada::HistoryStore& s) { return std::min (maxSpan, s.capacity() * 0.1); }

private:
    struct View
    {
        double start, end;
        double span() const { return end - start; }
    };

    View currentView() const;
    double latestTime() const;
    float xOf (double t, const View& v) const;
    double timeAt (float x, const View& v) const;
    void setScrolledEnd (double end);

    template <typename Get>
    void drawSeries (juce::Graphics&, juce::Rectangle<float> area, const View&, float lo, float hi, Get get,
                     juce::Colour, float thickness) const;
    void drawTimeAxis (juce::Graphics&, const View&) const;
    void drawHover (juce::Graphics&, const View&, float levelLo, float levelHi) const;

    const ada::HistoryStore* store;
    juce::String statusText;
    juce::Rectangle<float> header, levelArea, psrArea, axisArea;
    double span = 60.0;
    bool followLive = true;
    std::uint64_t shownMeasurement = 0;
    double scrolledEnd = 0.0, dragStartEnd = 0.0;
    std::optional<float> hoverX;
};
