#include "HistoryGraph.h"

#include "Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
double frameTime (const ada::DynamicsMetrics& f) { return (double) (f.binIndex + 1) * 0.1; }
constexpr float leftMargin = 46.0f, rightMargin = 8.0f;
} // namespace

HistoryGraph::HistoryGraph (const ada::HistoryStore& s) : store (&s) {}

void HistoryGraph::setStore (const ada::HistoryStore& newStore)
{
    store = &newStore;
    shownMeasurement = store->measurementId();
    followLive = true;
    span = 60.0;
    repaint();
}

void HistoryGraph::showAll()
{
    span = std::clamp (latestTime(), minSpan, std::max (minSpan, maxSpanFor (*store)));
    followLive = true;
    repaint();
}

void HistoryGraph::setTargetLines (std::optional<float> loudness, std::optional<float> ceiling)
{
    if (loudness == targetLoudness && ceiling == targetCeiling)
        return;
    targetLoudness = loudness;
    targetCeiling = ceiling;
    repaint();
}

void HistoryGraph::setStatusText (const juce::String& text)
{
    if (text == statusText)
        return;
    statusText = text;
    repaint();
}

void HistoryGraph::resized()
{
    auto r = getLocalBounds().toFloat();
    header = r.removeFromTop (20.0f);
    axisArea = r.removeFromBottom (18.0f).withTrimmedLeft (leftMargin).withTrimmedRight (rightMargin);
    r = r.withTrimmedLeft (leftMargin).withTrimmedRight (rightMargin);
    levelArea = r.removeFromTop (r.getHeight() * 0.46f);
    r.removeFromTop (12.0f);
    psrArea = r;
}

double HistoryGraph::latestTime() const { return store->empty() ? 0.0 : frameTime (store->latest()); }

HistoryGraph::View HistoryGraph::currentView() const
{
    const double end = followLive ? std::max (latestTime(), span) : scrolledEnd;
    return { end - span, end };
}

float HistoryGraph::xOf (double t, const View& v) const
{
    return levelArea.getX() + (float) ((t - v.start) / v.span()) * levelArea.getWidth();
}

double HistoryGraph::timeAt (float x, const View& v) const
{
    return v.start + (double) ((x - levelArea.getX()) / levelArea.getWidth()) * v.span();
}

void HistoryGraph::setScrolledEnd (double end)
{
    const double latest = std::max (latestTime(), span);
    scrolledEnd = std::clamp (end, span, latest);
    followLive = scrolledEnd >= latest - 1e-6;
}

// ---------------------------------------------------------------------------------------------

template <typename Get>
void HistoryGraph::drawSeries (juce::Graphics& g, juce::Rectangle<float> area, const View& v, float lo, float hi,
                               Get get, juce::Colour colour, float thickness) const
{
    const int first = std::max (0, store->lowerBound ((std::int64_t) std::floor (v.start * 10.0) - 2) - 1);
    const int last = store->lowerBound ((std::int64_t) std::ceil (v.end * 10.0) + 1); // exclusive
    if (last <= first)
        return;

    auto yOf = [&] (float value) {
        value = std::clamp (value, lo, hi);
        return area.getBottom() - (value - lo) / (hi - lo) * area.getHeight();
    };

    g.saveState();
    g.reduceClipRegion (area.expanded (0.0f, thickness).toNearestInt());
    g.setColour (colour);

    if ((float) (last - first) <= area.getWidth() * 1.5f)
    {
        juce::Path path;
        bool penDown = false;
        for (int i = first; i < last; ++i)
        {
            const auto& f = store->at (i);
            const float value = get (f);
            if (! std::isfinite (value))
            {
                penDown = false;
                continue;
            }
            const float x = xOf (frameTime (f), v), y = yOf (value);
            if (penDown)
                path.lineTo (x, y);
            else
                path.startNewSubPath (x, y);
            penDown = true;
        }
        g.strokePath (path, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        // More frames than pixels: draw each pixel column's min-max envelope
        int column = std::numeric_limits<int>::min();
        float lowest = 0.0f, highest = 0.0f;
        float lastValue = std::numeric_limits<float>::quiet_NaN();
        bool any = false;
        auto flush = [&] {
            if (! any)
                return;
            const float x = area.getX() + (float) column + 0.5f;
            const float yTop = yOf (highest), yBottom = yOf (lowest);
            g.fillRect (juce::Rectangle<float> (x - thickness * 0.5f, yTop - thickness * 0.5f, thickness,
                                                yBottom - yTop + thickness));
        };
        for (int i = first; i < last; ++i)
        {
            const auto& f = store->at (i);
            const int c = (int) std::floor (xOf (frameTime (f), v) - area.getX());
            const float value = get (f);
            if (c != column)
            {
                flush();
                // Start the new column at the previous value so neighbouring columns join up;
                // otherwise steep slopes break into dots. A real gap (non-finite) stays a gap.
                any = std::isfinite (lastValue) && std::isfinite (value) && c == column + 1;
                lowest = highest = lastValue;
                column = c;
            }
            lastValue = value;
            if (! std::isfinite (value))
                continue;
            lowest = any ? std::min (lowest, value) : value;
            highest = any ? std::max (highest, value) : value;
            any = true;
        }
        flush();
    }
    g.restoreState();
}

void HistoryGraph::drawTimeAxis (juce::Graphics& g, const View& v) const
{
    static constexpr double steps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800 };
    const double wanted = v.span() / std::max (2.0, (double) levelArea.getWidth() / 90.0);
    double step = steps[std::size (steps) - 1];
    for (double s : steps)
        if (s >= wanted)
        {
            step = s;
            break;
        }

    g.setFont (juce::FontOptions (11.0f));
    for (double t = std::ceil (v.start / step) * step; t <= v.end + 1e-9; t += step)
    {
        const float x = xOf (t, v);
        g.setColour (theme::grid);
        g.drawVerticalLine ((int) x, levelArea.getY(), levelArea.getBottom());
        g.drawVerticalLine ((int) x, psrArea.getY(), psrArea.getBottom());
        g.setColour (theme::textMuted);
        auto label = juce::Rectangle<float> (x - 30.0f, axisArea.getY() + 2.0f, 60.0f, 14.0f);
        label = label.withX (std::clamp (label.getX(), 0.0f, (float) getWidth() - label.getWidth()));
        g.drawText (theme::timeLabel (t), label, juce::Justification::centred);
    }
}

void HistoryGraph::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    if (store->measurementId() != shownMeasurement) // new measurement: back to live
    {
        shownMeasurement = store->measurementId();
        followLive = true;
    }
    const auto v = currentView();

    // Header: legend left, view state right
    g.setFont (juce::FontOptions (12.0f));
    auto h = header.withTrimmedLeft (leftMargin).withTrimmedRight (rightMargin);
    auto legend = [&] (juce::Colour c, const juce::String& text) {
        g.setColour (c);
        g.fillRect (h.getX(), h.getCentreY() - 1.0f, 14.0f, 2.0f);
        g.setColour (theme::textSecondary);
        const float w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 26.0f;
        g.drawText (text, h.withTrimmedLeft (19.0f).withWidth (w), juce::Justification::centredLeft);
        h.removeFromLeft (w + 19.0f);
    };
    legend (theme::shortTerm, "Short-term loudness (LUFS)");
    legend (theme::truePeak, "True peak, max over 3 s (dBTP)");
    g.setColour (theme::textMuted);
    const juce::String view = followLive ? "last " + theme::timeLabel (span) : "scrolled back (double-click: latest)";
    const juce::String state = statusText.isNotEmpty() ? statusText + ", " + view : "Live, " + view;
    g.drawText (state + "   |   wheel: zoom, drag: scroll", h, juce::Justification::centredRight);

    if (store->empty())
    {
        g.setColour (theme::textMuted);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("Waiting for audio", levelArea.getUnion (psrArea), juce::Justification::centred);
        return;
    }

    // Level panel: range follows the visible data in 5 dB steps
    float dataMin = std::numeric_limits<float>::max(), dataMax = std::numeric_limits<float>::lowest();
    {
        const int first = std::max (0, store->lowerBound ((std::int64_t) std::floor (v.start * 10.0)) - 1);
        const int last = store->lowerBound ((std::int64_t) std::ceil (v.end * 10.0) + 1);
        for (int i = first; i < last; ++i)
            for (float value : { store->at (i).shortTermLUFS, store->at (i).truePeak3sDBTP })
                if (std::isfinite (value) && value > -70.0f)
                {
                    dataMin = std::min (dataMin, value);
                    dataMax = std::max (dataMax, value);
                }
    }
    // keep the delivery target lines in view
    for (const auto& line : { targetLoudness, targetCeiling })
        if (line && dataMin <= dataMax)
        {
            dataMin = std::min (dataMin, *line);
            dataMax = std::max (dataMax, *line);
        }
    float lo = -40.0f, hi = 0.0f;
    if (dataMin <= dataMax)
    {
        lo = std::max (-70.0f, std::floor ((dataMin - 3.0f) / 5.0f) * 5.0f);
        hi = std::min (5.0f, std::ceil ((dataMax + 2.0f) / 5.0f) * 5.0f);
        if (hi - lo < 15.0f)
            lo = hi - 15.0f;
    }
    const float tick = hi - lo > 30.0f ? 10.0f : 5.0f;
    g.setFont (juce::FontOptions (11.0f));
    for (float y = std::ceil (lo / tick) * tick; y <= hi + 0.01f; y += tick)
    {
        const float py = levelArea.getBottom() - (y - lo) / (hi - lo) * levelArea.getHeight();
        g.setColour (theme::grid);
        g.drawHorizontalLine ((int) py, levelArea.getX(), levelArea.getRight());
        g.setColour (theme::textMuted);
        g.drawText (theme::number (y, 0), juce::Rectangle<float> (0.0f, py - 7.0f, leftMargin - 6.0f, 14.0f),
                    juce::Justification::centredRight);
    }

    // PSR panel: zone bands with labels
    const float pLo = theme::psrRangeMin, pHi = theme::psrRangeMax;
    auto psrY = [&] (float value) { return psrArea.getBottom() - (value - pLo) / (pHi - pLo) * psrArea.getHeight(); };
    for (int z = 0; z < 4; ++z)
    {
        const float top = psrY (std::min (theme::zoneUpper[z], pHi)), bottom = psrY (std::max (theme::zoneLower[z], pLo));
        const juce::Rectangle<float> band (psrArea.getX(), top, psrArea.getWidth(), bottom - top);
        g.setColour (theme::zoneColours[z].withAlpha (0.14f));
        g.fillRect (band);
        g.setColour (theme::textMuted);
        if (band.getHeight() >= 13.0f) // small windows: skip labels that would overlap
            g.drawText (juce::String (theme::zoneNames[z]) + " " + theme::zoneRange (z) + " ", band,
                        juce::Justification::centredRight);
    }
    float lastLabelY = 1.0e9f;
    for (float y : { 0.0f, 8.0f, 10.0f, 12.0f, 16.0f, 20.0f, 24.0f })
    {
        if (lastLabelY - psrY (y) < 12.0f)
            continue;
        lastLabelY = psrY (y);
        g.setColour (theme::textMuted);
        g.drawText (theme::number (y, 0), juce::Rectangle<float> (0.0f, psrY (y) - 7.0f, leftMargin - 6.0f, 14.0f),
                    juce::Justification::centredRight);
    }
    g.saveState();
    g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, 10.0f, psrArea.getCentreY()));
    g.setColour (theme::textSecondary);
    g.drawText ("PSR (dB)", juce::Rectangle<float> (10.0f - 40.0f, psrArea.getCentreY() - 7.0f, 80.0f, 14.0f),
                juce::Justification::centred);
    g.restoreState();

    drawTimeAxis (g, v);

    // delivery target lines (dashed, labelled at the right end)
    const float dashes[] = { 6.0f, 4.0f };
    auto lineY = [&] (std::optional<float> value) -> std::optional<float> {
        if (! value || ! std::isfinite (*value) || *value < lo || *value > hi)
            return std::nullopt;
        return levelArea.getBottom() - (*value - lo) / (hi - lo) * levelArea.getHeight();
    };
    struct Label
    {
        juce::String text;
        juce::Colour colour;
        juce::Rectangle<float> area;
    };
    std::array<std::optional<Label>, 2> labels; // drawn after the curves, so they stay readable
    auto targetLine = [&] (std::optional<float> y, juce::Colour colour, const juce::String& label, bool labelBelow,
                           std::optional<Label>& out) {
        if (! y)
            return;
        g.setColour (colour.withAlpha (0.8f));
        g.drawDashedLine (juce::Line<float> (levelArea.getX(), *y, levelArea.getRight(), *y), dashes, 2, 1.0f);
        // a label that would leave the plot at the top (into the header text) goes below its line
        const bool below = labelBelow || *y - 15.0f < levelArea.getY();
        const float width = juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (11.0f)), label) + 8.0f;
        out = Label { label, colour, { levelArea.getRight() - width - 2.0f, below ? *y + 1.0f : *y - 15.0f, width, 14.0f } };
    };
    const auto loudnessY = lineY (targetLoudness), ceilingY = lineY (targetCeiling);
    // labels sit above their lines; when the lines are close, the lower line's label goes below it
    const bool close = loudnessY && ceilingY && std::abs (*loudnessY - *ceilingY) < 15.0f;
    if (loudnessY)
        targetLine (loudnessY, theme::shortTerm, "target " + theme::number (*targetLoudness) + " LUFS",
                    close && *loudnessY >= *ceilingY, labels[0]);
    if (ceilingY)
        targetLine (ceilingY, theme::truePeak, "ceiling " + theme::number (*targetCeiling) + " dBTP",
                    close && *ceilingY > *loudnessY, labels[1]);

    drawSeries (g, levelArea, v, lo, hi, [] (const ada::DynamicsMetrics& f) { return f.truePeak3sDBTP; },
                theme::truePeak, 1.5f);
    drawSeries (g, levelArea, v, lo, hi, [] (const ada::DynamicsMetrics& f) { return f.shortTermLUFS; },
                theme::shortTerm, 1.5f);
    drawSeries (g, psrArea, v, pLo, pHi,
                [] (const ada::DynamicsMetrics& f) {
                    return ada::passesLivePsrGate (f) ? f.psr : std::numeric_limits<float>::quiet_NaN();
                },
                theme::psrLine, 1.5f);

    if (labels[0] && labels[1] && labels[0]->area.intersects (labels[1]->area)) // both lines close to the top
        labels[0]->area.setY (labels[1]->area.getBottom() + 1.0f);
    g.setFont (juce::FontOptions (11.0f));
    for (const auto& label : labels)
        if (label)
        {
            g.setColour (theme::panel.withAlpha (0.85f));
            g.fillRoundedRectangle (label->area, 3.0f);
            g.setColour (label->colour);
            g.drawText (label->text, label->area, juce::Justification::centred);
        }

    drawHover (g, v, lo, hi);
}

void HistoryGraph::drawHover (juce::Graphics& g, const View& v, float lo, float hi) const
{
    if (! hoverX || *hoverX < levelArea.getX() || *hoverX > levelArea.getRight())
        return;
    const double t = timeAt (*hoverX, v);
    const int i = store->lowerBound ((std::int64_t) std::llround (t * 10.0) - 1);
    if (i >= store->size())
        return;
    const auto& f = store->at (i);
    if (std::abs (frameTime (f) - t) > 0.25)
        return;

    const float x = xOf (frameTime (f), v);
    g.setColour (theme::textMuted);
    g.drawVerticalLine ((int) x, levelArea.getY(), levelArea.getBottom());
    g.drawVerticalLine ((int) x, psrArea.getY(), psrArea.getBottom());

    auto fmt = [] (float value, const char* unit) {
        if (! std::isfinite (value))
            return theme::dash();
        return theme::number (value) + " " + unit;
    };
    const bool measured = ada::passesLivePsrGate (f);
    const juce::String lines[] = { theme::timeLabel (frameTime (f), true),
                                   "Short-term  " + fmt (f.shortTermLUFS, "LUFS"),
                                   "True peak 3 s  " + fmt (f.truePeak3sDBTP, "dBTP"),
                                   "PSR  " + (measured ? fmt (f.psr, "dB") : theme::dash()) };

    const float boxW = 170.0f, boxH = 70.0f;
    float bx = x + 10.0f;
    if (bx + boxW > levelArea.getRight())
        bx = x - 10.0f - boxW;
    const juce::Rectangle<float> box (bx, levelArea.getY() + 4.0f, boxW, boxH);
    g.setColour (theme::background.withAlpha (0.92f));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (theme::grid);
    g.drawRoundedRectangle (box, 4.0f, 1.0f);
    g.setFont (juce::FontOptions (12.0f));
    for (int k = 0; k < 4; ++k)
    {
        g.setColour (k == 0 ? theme::textPrimary : theme::textSecondary);
        g.drawText (lines[k], box.reduced (8.0f, 4.0f).withHeight (15.5f).translated (0.0f, 15.5f * (float) k),
                    juce::Justification::centredLeft);
    }
    juce::ignoreUnused (lo, hi);
}

// ---------------------------------------------------------------------------------------------

void HistoryGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto v = currentView();
    const double newSpan = std::clamp (span * std::exp (-(double) wheel.deltaY * 1.5), minSpan,
                                       std::max (minSpan, maxSpanFor (*store)));
    if (followLive)
        span = newSpan; // anchored at the live edge
    else
    {
        // keep the time under the mouse in place
        const double anchor = timeAt (e.position.x, v);
        const double fraction = (anchor - v.start) / v.span();
        span = newSpan;
        setScrolledEnd (anchor + (1.0 - fraction) * span);
    }
    repaint();
}

void HistoryGraph::mouseDown (const juce::MouseEvent&) { dragStartEnd = currentView().end; }

void HistoryGraph::mouseDrag (const juce::MouseEvent& e)
{
    const double dt = -(double) e.getDistanceFromDragStartX() / levelArea.getWidth() * span;
    setScrolledEnd (dragStartEnd + dt);
    hoverX = e.position.x;
    repaint();
}

void HistoryGraph::mouseDoubleClick (const juce::MouseEvent&)
{
    followLive = true;
    repaint();
}

void HistoryGraph::mouseMove (const juce::MouseEvent& e)
{
    hoverX = e.position.x;
    repaint();
}

void HistoryGraph::mouseExit (const juce::MouseEvent&)
{
    hoverX.reset();
    repaint();
}
