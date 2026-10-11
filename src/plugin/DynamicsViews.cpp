#include "DynamicsViews.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>

// --- PsrMeter --------------------------------------------------------------------------------

PsrMeter::PsrMeter()
{
    setTooltip ("PSR right now, as a bar coloured by its zone.\n"
                "Solid line: lowest PSR since the last reset (the most squashed 3 seconds).\n"
                "Dashed line: P10, the PSR the track stays below 10 % of the measured time.\n"
                "Green >= 12 open | yellow 10-12 controlled | orange 8-10 heavy | red < 8 over-limited.");
}

void PsrMeter::setValues (std::optional<float> c, std::optional<float> m, std::optional<float> p)
{
    if (c == current && m == minimum && p == p10)
        return;
    current = c;
    minimum = m;
    p10 = p;
    repaint();
}

void PsrMeter::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto r = getLocalBounds().toFloat().reduced (8.0f, 6.0f);

    g.setColour (theme::textSecondary);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("PSR now (dB)", r.removeFromTop (16.0f), juce::Justification::centredLeft);
    g.setColour (current ? theme::zoneColours[theme::zoneOf (*current)] : theme::textMuted);
    g.setFont (juce::FontOptions (24.0f));
    g.drawText (current ? theme::number (*current) : theme::dash(), r.removeFromTop (30.0f),
                juce::Justification::centredLeft);
    r.removeFromTop (8.0f);
    r.removeFromBottom (6.0f);

    const auto scale = r.removeFromLeft (20.0f);
    const auto bar = r.removeFromLeft (26.0f);
    const auto labels = r.withTrimmedLeft (6.0f);
    const float lo = theme::psrRangeMin, hi = theme::psrRangeMax;
    auto yOf = [&] (float v) { return bar.getBottom() - (std::clamp (v, lo, hi) - lo) / (hi - lo) * bar.getHeight(); };

    for (int z = 0; z < 4; ++z)
    {
        const float top = yOf (std::min (theme::zoneUpper[z], hi)), bottom = yOf (std::max (theme::zoneLower[z], lo));
        g.setColour (theme::zoneColours[z].withAlpha (0.18f));
        g.fillRect (bar.withY (top).withBottom (bottom));
    }
    g.setFont (juce::FontOptions (10.5f));
    for (float v : { 0.0f, 8.0f, 10.0f, 12.0f, 24.0f })
    {
        g.setColour (theme::textMuted);
        g.drawText (theme::number (v, 0), scale.withY (yOf (v) - 6.0f).withHeight (12.0f).withTrimmedRight (4.0f),
                    juce::Justification::centredRight);
    }

    if (current)
    {
        const float top = yOf (*current);
        g.setColour (theme::zoneColours[theme::zoneOf (*current)]);
        g.fillRoundedRectangle (bar.reduced (4.0f, 0.0f).withTop (top), 3.0f);
    }

    // Markers; keep their labels from overlapping
    float minLabelY = -100.0f;
    if (minimum)
    {
        const float y = yOf (*minimum);
        g.setColour (theme::textPrimary);
        g.fillRect (bar.getX() - 2.0f, y - 1.0f, bar.getWidth() + 8.0f, 2.0f);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("min " + theme::number (*minimum), labels.withY (y - 7.0f).withHeight (14.0f),
                    juce::Justification::centredLeft);
        minLabelY = y;
    }
    if (p10)
    {
        const float y = yOf (*p10);
        g.setColour (theme::textSecondary);
        const float dashes[] = { 4.0f, 3.0f };
        g.drawDashedLine (juce::Line<float> (bar.getX() - 2.0f, y, bar.getRight() + 6.0f, y), dashes, 2, 1.5f);
        float labelY = y;
        if (std::abs (labelY - minLabelY) < 14.0f)
            labelY = minLabelY - 14.0f;
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("P10 " + theme::number (*p10), labels.withY (labelY - 7.0f).withHeight (14.0f),
                    juce::Justification::centredLeft);
    }
}

// --- ZoneBar ---------------------------------------------------------------------------------

ZoneBar::ZoneBar()
{
    setTooltip ("How much of the measured time (since the last reset) fell into each PSR zone.\n"
                "Silence, very quiet passages and the first 3 seconds are not counted.\n"
                "A large red/orange share means most of the track is heavily limited.");
}

void ZoneBar::setStats (const ada::HistoryStore::PsrStats& s)
{
    stats = s;
    repaint();
}

void ZoneBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto r = getLocalBounds().toFloat().reduced (10.0f, 6.0f);

    auto title = r.removeFromTop (16.0f);
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (theme::textSecondary);
    g.drawText ("Time in each PSR zone since reset", title, juce::Justification::centredLeft);
    g.setColour (theme::textMuted);
    g.drawText (stats.measuredFrames > 0 ? "measured " + theme::timeLabel ((double) stats.measuredFrames * 0.1)
                                         : juce::String ("no measured audio yet"),
                title, juce::Justification::centredRight);
    r.removeFromTop (5.0f);

    auto bar = r.removeFromTop (14.0f);
    if (stats.measuredFrames == 0)
    {
        g.setColour (theme::grid);
        g.fillRoundedRectangle (bar, 3.0f);
    }
    else
    {
        // segments separated by 2 px gaps
        float x = bar.getX();
        const float usable = bar.getWidth();
        for (int z = 0; z < 4; ++z)
        {
            const float w = usable * stats.zonePercent[(size_t) z] / 100.0f;
            if (w <= 0.0f)
                continue;
            g.setColour (theme::zoneColours[z]);
            g.fillRoundedRectangle (juce::Rectangle<float> (x, bar.getY(), std::max (0.0f, w - 2.0f), bar.getHeight()), 2.0f);
            x += w;
        }
    }

    // Legend with percentages: identity never relies on colour alone
    r.removeFromTop (6.0f);
    const float cellW = r.getWidth() / 4.0f;
    g.setFont (juce::FontOptions (12.0f));
    for (int z = 0; z < 4; ++z)
    {
        auto cell = r.withX (r.getX() + cellW * (float) z).withWidth (cellW).withHeight (16.0f);
        g.setColour (theme::zoneColours[z]);
        g.fillRoundedRectangle (cell.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 10.0f), 2.0f);
        const juce::String pct = stats.measuredFrames > 0
                                     ? theme::number (stats.zonePercent[(size_t) z], 0) + " %"
                                     : theme::dash();
        g.setColour (theme::textSecondary);
        g.drawText (" " + juce::String (theme::zoneNames[z]) + " " + theme::zoneRange (z) + ":  " + pct, cell,
                    juce::Justification::centredLeft);
    }
}
