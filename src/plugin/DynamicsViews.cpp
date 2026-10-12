#include "DynamicsViews.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>

// --- PsrMeter --------------------------------------------------------------------------------

PsrMeter::PsrMeter() { restoreDefaultTooltip(); }

void PsrMeter::restoreDefaultTooltip()
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

void PsrMeter::setTitle (const juce::String& newTitle)
{
    if (newTitle != title)
    {
        title = newTitle;
        repaint();
    }
}

void PsrMeter::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto r = getLocalBounds().toFloat().reduced (8.0f, 6.0f);

    g.setColour (theme::textSecondary);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (title, r.removeFromTop (16.0f), juce::Justification::centredLeft);
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

ZoneBar::ZoneBar() { restoreDefaultTooltip(); }

void ZoneBar::restoreDefaultTooltip()
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

void ZoneBar::setTitle (const juce::String& newTitle)
{
    if (newTitle != title)
    {
        title = newTitle;
        repaint();
    }
}

void ZoneBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto r = getLocalBounds().toFloat().reduced (10.0f, 6.0f);

    auto titleRow = r.removeFromTop (16.0f);
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (theme::textSecondary);
    g.drawText (title, titleRow, juce::Justification::centredLeft);
    g.setColour (theme::textMuted);
    g.drawText (stats.measuredFrames > 0 ? "measured " + theme::timeLabel ((double) stats.measuredFrames * 0.1)
                                         : juce::String ("no measured audio yet"),
                titleRow, juce::Justification::centredRight);
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
        // narrow: leave out the ranges (they are on the PSR axis and in the tooltips)
        const auto range = cellW >= 150.0f ? " " + theme::zoneRange (z) : juce::String();
        g.drawText (" " + juce::String (theme::zoneNames[z]) + range + ":  " + pct, cell,
                    juce::Justification::centredLeft, true);
    }
}

// --- TargetPanel -----------------------------------------------------------------------------

namespace
{
void drawStatusIcon (juce::Graphics& g, juce::Rectangle<float> r, ada::TargetStatus status)
{
    using S = ada::TargetStatus;
    const auto colour = status == S::ok ? theme::zoneColours[0]
                      : status == S::warning ? theme::zoneColours[1]
                      : status == S::problem ? theme::zoneColours[3]
                                             : theme::grid;
    g.setColour (colour);
    g.fillEllipse (r);
    g.setColour (theme::background);
    const auto c = r.getCentre();
    const float s = r.getWidth() * 0.22f;
    juce::Path p;
    if (status == S::ok)
    {
        p.startNewSubPath (c.x - s * 1.2f, c.y);
        p.lineTo (c.x - s * 0.3f, c.y + s);
        p.lineTo (c.x + s * 1.3f, c.y - s);
        g.strokePath (p, juce::PathStrokeType (1.8f));
    }
    else if (status == S::problem)
    {
        g.drawLine (c.x - s, c.y - s, c.x + s, c.y + s, 1.8f);
        g.drawLine (c.x - s, c.y + s, c.x + s, c.y - s, 1.8f);
    }
    else if (status == S::warning)
    {
        g.fillRect (c.x - 0.9f, c.y - s * 1.4f, 1.8f, s * 1.8f);
        g.fillRect (c.x - 0.9f, c.y + s * 0.8f, 1.8f, 1.8f);
    }
    else
        g.fillRect (c.x - s, c.y - 0.9f, s * 2.0f, 1.8f);
}

juce::String db (double v) { return theme::number ((float) v); }
} // namespace

TargetPanel::TargetPanel()
{
    box.addItem ("No delivery target", 1);
    for (size_t i = 0; i < ada::deliveryTargets.size(); ++i)
        box.addItem (ada::deliveryTargets[i].name, (int) i + 2);
    box.addSeparator();
    box.addItem ("Edit custom target...", editCustomId); // always opens the dialog, even when Custom is selected
    box.onChange = [this] {
        if (onSelect)
            onSelect (box.getSelectedId() == editCustomId ? ada::customTargetIndex : box.getSelectedId() - 2);
    };
    box.setTooltip ("Choose where the track is going. Streaming services turn louder tracks down to their target;\n"
                    "broadcast requires the target within a tolerance. Custom lets you enter your own values.");
    addAndMakeVisible (box);
    updateTooltip();
}

void TargetPanel::setSelection (int index, const std::optional<ada::DeliveryTarget>& t)
{
    target = t;
    if (index == ada::customTargetIndex && t)
        box.changeItemText (ada::customTargetIndex + 2, "Custom (" + db (t->loudness) + " LUFS, " + db (t->peakCeiling) + " dBTP)");
    box.setSelectedId (index + 2, juce::dontSendNotification);
    updateTooltip();
    repaint();
}

void TargetPanel::setMeasurement (std::optional<double> i, std::optional<double> tp)
{
    // judge the values as displayed (0.1 dB), so the verdict never contradicts the readouts
    auto round = [] (std::optional<double> v) { return v ? std::optional<double> (std::round (*v * 10.0) / 10.0 + 0.0) : v; };
    i = round (i);
    tp = round (tp);
    if (i == integrated && tp == truePeakMax)
        return;
    integrated = i;
    truePeakMax = tp;
    updateTooltip();
    repaint();
}

std::optional<ada::TargetVerdict> TargetPanel::verdict() const
{
    if (! target)
        return std::nullopt;
    return ada::evaluateTarget (*target, integrated, truePeakMax);
}

juce::String TargetPanel::loudnessText (const ada::TargetVerdict& v) const
{
    if (v.loudness == ada::TargetStatus::unknown)
        return "Integrated loudness: not measured yet";
    const auto& t = *target;
    const auto value = db (*integrated) + " LUFS";
    const auto delta = theme::number ((float) std::abs (v.loudnessDelta));
    if (t.kind == ada::DeliveryTarget::Kind::broadcast)
        return value + (v.loudness == ada::TargetStatus::ok ? ": within " : ": outside ") + juce::String (juce::CharPointer_UTF8 ("\xc2\xb1"))
               + theme::number ((float) t.tolerance) + " LU of " + db (t.loudness);
    if (v.loudness == ada::TargetStatus::ok)
        return value + ": on target (" + db (t.loudness) + ")";
    if (v.loudnessDelta > 0.0)
        return value + ": " + delta + " LU louder than " + db (t.loudness) + ", will be turned down by " + delta + " dB";
    return value + ": " + delta + " LU quieter than " + db (t.loudness) + ", plays quieter than other tracks";
}

juce::String TargetPanel::peakText (const ada::TargetVerdict& v) const
{
    if (v.peak == ada::TargetStatus::unknown)
        return "Max true peak: not measured yet";
    const auto value = db (*truePeakMax) + " dBTP";
    const auto ceiling = db (v.ceiling) + " dBTP";
    const bool stricter = target->loudMasterCeiling && v.ceiling == *target->loudMasterCeiling;
    const auto note = stricter ? juce::String (" (stricter for masters louder than ") + db (target->loudness) + ")" : juce::String();
    if (v.peak == ada::TargetStatus::ok)
        return value + ": below the " + ceiling + " ceiling" + note;
    const auto over = theme::number ((float) -v.peakMargin);
    if (*truePeakMax > 0.0)
        return value + ": above 0 dBTP, clips after encoding (ceiling " + ceiling + ")";
    return value + ": " + over + " dB above the " + ceiling + " ceiling" + note;
}

void TargetPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto r = getLocalBounds().toFloat().reduced (8.0f, 5.0f);
    r.removeFromTop (22.0f); // selector row
    r.removeFromTop (3.0f);

    const auto v = verdict();
    g.setFont (juce::FontOptions (12.0f));
    if (! v)
    {
        g.setColour (theme::textMuted);
        g.drawFittedText ("Choose a delivery target", r.removeFromTop (17.0f).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.8f);
        return;
    }
    // compact lines for the corner; the full sentences are in the tooltip
    const std::pair<ada::TargetStatus, juce::String> lines[] = { { v->loudness, shortLoudnessText (*v) },
                                                                  { v->peak, shortPeakText (*v) } };
    for (const auto& [status, text] : lines)
    {
        auto row = r.removeFromTop (17.0f);
        drawStatusIcon (g, row.removeFromLeft (13.0f).withSizeKeepingCentre (13.0f, 13.0f), status);
        row.removeFromLeft (5.0f);
        g.setColour (theme::textSecondary);
        g.drawFittedText (text, row.toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
        r.removeFromTop (1.0f);
    }
}

void TargetPanel::resized()
{
    box.setBounds (getLocalBounds().reduced (8, 5).removeFromTop (22));
}

juce::String TargetPanel::shortLoudnessText (const ada::TargetVerdict& v) const
{
    if (v.loudness == ada::TargetStatus::unknown)
        return "Loudness: " + theme::dash();
    const auto delta = theme::number ((float) std::abs (v.loudnessDelta));
    if (target->kind == ada::DeliveryTarget::Kind::broadcast)
        return juce::String ("Loudness: ") + (v.loudness == ada::TargetStatus::ok ? "in range" : "out of range");
    if (v.loudness == ada::TargetStatus::ok)
        return "Loudness: on target";
    return "Loudness: " + delta + (v.loudnessDelta > 0.0 ? " LU too loud" : " LU too quiet");
}

juce::String TargetPanel::shortPeakText (const ada::TargetVerdict& v) const
{
    if (v.peak == ada::TargetStatus::unknown)
        return "Peak: " + theme::dash();
    if (v.peak == ada::TargetStatus::ok)
        return "Peak: below " + db (v.ceiling) + " dBTP";
    if (*truePeakMax > 0.0)
        return "Peak: clips (above 0 dBTP)";
    return "Peak: " + theme::number ((float) -v.peakMargin) + " dB too high";
}

void TargetPanel::updateTooltip()
{
    juce::String text = "Checks the integrated loudness and the max true peak against the chosen delivery target.\n"
                        "Platform figures change over time: Spotify's and the broadcast values are published,\n"
                        "the others are widely reported measurements.";
    if (const auto v = verdict())
        text = loudnessText (*v) + "\n" + peakText (*v) + "\n\n" + text;
    setTooltip (text);
}
