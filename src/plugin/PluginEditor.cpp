#include "PluginEditor.h"

#include "Theme.h"

#include <cmath>

namespace
{
constexpr float loudnessFloor = -70.0f; // BS.1770 absolute gate; anything below reads as "< -70"
constexpr int readoutsHeight = 54, zoneBarHeight = 62, controlsHeight = 30, meterWidth = 130, gap = 8;
} // namespace

// ---------------------------------------------------------------------------------------------

Readout::Readout (juce::String l, juce::String u, Kind k, const juce::String& help)
    : label (std::move (l)), unit (std::move (u)), kind (k)
{
    setTooltip (help);
}

void Readout::setValue (float newValue, bool newAlarm)
{
    const bool same = (newValue == value || (std::isnan (newValue) && std::isnan (value))) && newAlarm == alarm;
    value = newValue;
    alarm = newAlarm;
    if (! same)
        repaint();
}

void Readout::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    auto area = getLocalBounds().reduced (8, 5);

    auto labelRow = area.removeFromTop (16);
    g.setFont (juce::FontOptions (12.0f));
    if (alarm)
    {
        g.setColour (theme::zoneColours[3]);
        g.drawText ("CLIP", labelRow.removeFromRight (34), juce::Justification::centredRight);
    }
    g.setColour (theme::textSecondary);
    g.drawText (label, labelRow, juce::Justification::centredLeft, true);

    juce::String text = theme::dash();
    bool showUnit = false;
    if (kind == Kind::loudness && (value < loudnessFloor || (std::isinf (value) && value < 0)))
        text = "< " + theme::minusSign() + "70";
    else if (std::isfinite (value))
    {
        text = theme::number (value);
        showUnit = true;
    }
    // shrink the value to fit narrow cells (unit only when there is room)
    float size = 22.0f;
    while (size > 13.0f && juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (size)), text)
                               > area.getWidth())
        size -= 1.0f;
    const juce::FontOptions valueFont (size);
    const int w = juce::GlyphArrangement::getStringWidthInt (juce::Font (valueFont), text);
    g.setColour (alarm ? theme::zoneColours[3] : theme::textPrimary);
    g.setFont (valueFont);
    g.drawText (text, area, juce::Justification::centredLeft);
    if (showUnit && w + 4 + 28 <= area.getWidth())
    {
        g.setColour (theme::textMuted);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (unit, area.withTrimmedLeft (w + 4).withTrimmedTop (6), juce::Justification::centredLeft);
    }
}

// ---------------------------------------------------------------------------------------------

AdaAudioProcessorEditor::AdaAudioProcessorEditor (AdaAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), graph (p.history().store()),
      resetOnPlayAttachment (p.parameters, "resetOnPlay", resetOnPlayToggle),
      freezeAttachment (p.parameters, "freezeWhenStopped", freezeToggle)
{
    using K = Readout::Kind;
    readouts[psr] = std::make_unique<Readout> ("PSR", "dB", K::decibel,
        "Peak-to-short-term ratio: true peak minus short-term loudness over the same 3-second window.\n"
        "Shows how hard the transients are limited right now. Lower = more squashed.\n"
        "12 dB or more: open  |  10-12: controlled  |  8-10: heavy  |  below 8: over-limited.\n"
        "Blank during the first 3 s, in silence and in very quiet passages.");
    readouts[psrMin] = std::make_unique<Readout> ("PSR min", "dB", K::decibel,
        "Lowest PSR since the last reset: the most compressed 3-second stretch so far.\n"
        "Silence, very quiet passages and the first 3 s are ignored.");
    readouts[shortTerm] = std::make_unique<Readout> ("Short-term", "LUFS", K::loudness,
        "Short-term loudness (EBU R128 \"S\"): perceived loudness of the last 3 seconds.");
    readouts[momentary] = std::make_unique<Readout> ("Momentary", "LUFS", K::loudness,
        "Momentary loudness (EBU R128 \"M\"): perceived loudness of the last 400 ms.\n"
        "Follows the music closely; good for spotting loud hits.");
    readouts[integrated] = std::make_unique<Readout> ("Integrated", "LUFS", K::loudness,
        "Integrated loudness: average loudness since the last reset, ignoring silence and quiet parts (BS.1770 gating).\n"
        "This is what streaming services normalise to: Spotify and YouTube about -14 LUFS, Apple Music about -16 LUFS.\n"
        "A track louder than that is simply turned down on playback.");
    readouts[lra] = std::make_unique<Readout> ("LRA", "LU", K::decibel,
        "Loudness range (EBU Tech 3342): how much the loudness varies between quiet and loud sections\n"
        "(spread from the 10th to the 95th percentile of short-term loudness).\n"
        "Measures macro-dynamics, e.g. verse vs. chorus, not transient limiting.");
    readouts[truePeak3s] = std::make_unique<Readout> ("True peak 3 s", "dBTP", K::decibel,
        "Highest true peak in the last 3 seconds, including peaks between samples\n"
        "(BS.1770 oversampling: 4x at 44.1/48 kHz, 2x at 88.2/96 kHz).\n"
        "This is the peak half of the PSR. Shows CLIP when it exceeds the clip threshold (default 0 dBTP).");
    readouts[truePeakMax] = std::make_unique<Readout> ("Max true peak", "dBTP", K::decibel,
        "Highest true peak since the last reset.\n"
        "Keep it at or below -1 dBTP for streaming: lossy encoding (AAC, MP3) adds up to about 1 dB of overshoot.");
    readouts[plr] = std::make_unique<Readout> ("PLR", "dB", K::decibel,
        "Peak-to-loudness ratio: max true peak minus integrated loudness.\n"
        "The headroom of the whole track. Loud modern masters: about 8-11 dB; dynamic recordings: 14 dB or more.");
    for (auto& r : readouts)
        addAndMakeVisible (*r);
    addAndMakeVisible (graph);
    addAndMakeVisible (meter);
    addAndMakeVisible (zoneBar);

    resetButton.setTooltip ("Start a new measurement now (clears the history, integrated loudness, LRA, peaks and PSR statistics).");
    resetOnPlayToggle.setTooltip (
        "Begin a new measurement when the playback position jumps: playback starts somewhere other than\n"
        "where it stopped, or you jump while playing (e.g. back to the song start).\n"
        "Pausing and continuing at the same spot, and cycle/loop playback, keep the measurement running.\n"
        "The reason for the last automatic restart is shown next to these switches.");
    freezeToggle.setTooltip (
        "While the transport is stopped, keep the last values on screen instead of measuring the silence.\n"
        "Turn this off to meter live input while the transport is stopped.");
    for (auto* t : { &resetOnPlayToggle, &freezeToggle })
        t->setColour (juce::ToggleButton::textColourId, theme::textSecondary);

    resetButton.onClick = [this] {
        // The engine resets at its next bin boundary, which may be much later while the transport
        // is frozen; the history clears now and ignores frames from the old measurement.
        processor.requestReset();
        processor.history().clearForReset();
        updateFromHistory();
    };
    addAndMakeVisible (resetButton);
    // transport options only make sense inside a host
    const bool inHost = processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone;
    addChildComponent (resetOnPlayToggle);
    addChildComponent (freezeToggle);
    resetOnPlayToggle.setVisible (inHost);
    freezeToggle.setVisible (inHost);

    activeShown = processor.isAnalysisActive();
    setResizable (true, true);
    setResizeLimits (860, 520, 2400, 1600);
    setSize (980, 620);
    updateFromHistory();
    startTimerHz (30);
}

AdaAudioProcessorEditor::~AdaAudioProcessorEditor() { stopTimer(); }

void AdaAudioProcessorEditor::updateFromHistory()
{
    const auto& store = processor.history().store();
    shownVersion = store.version();
    const auto stats = store.psrStats();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    if (store.empty())
    {
        for (auto& r : readouts)
            r->setValue (nan);
        meter.setValues ({}, {}, {});
    }
    else
    {
        const auto& f = store.latest();
        const bool measured = ada::passesLivePsrGate (f);
        readouts[psr]->setValue (measured ? f.psr : nan);
        readouts[psrMin]->setValue (stats.min ? *stats.min : nan);
        readouts[shortTerm]->setValue (f.shortTermLUFS);
        readouts[momentary]->setValue (f.momentaryLUFS);
        readouts[integrated]->setValue (f.integratedLUFS);
        readouts[lra]->setValue (f.lra);
        readouts[truePeak3s]->setValue (f.truePeak3sDBTP, f.isClipping);
        readouts[truePeakMax]->setValue (f.truePeakMaxDBTP);
        readouts[plr]->setValue (f.truePeakMaxDBTP - f.integratedLUFS);
        meter.setValues (measured ? std::optional<float> (f.psr) : std::nullopt, stats.min, stats.p10);
    }
    zoneBar.setStats (stats);
    graph.repaint();
}

void AdaAudioProcessorEditor::timerCallback()
{
    // React to the processor becoming (in)active, e.g. a device or sample-rate change
    if (processor.isAnalysisActive() != activeShown)
    {
        activeShown = processor.isAnalysisActive();
        resized();
        repaint();
    }
    if (processor.history().store().version() != shownVersion)
        updateFromHistory();
    if (processor.isFrozen() != frozenShown)
    {
        frozenShown = processor.isFrozen();
        repaint();
    }

    const auto restart = processor.lastRestart();
    if (restart.count != shownRestartCount)
    {
        shownRestartCount = restart.count;
        const double fs = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
        using R = AdaAudioProcessor::RestartReason;
        if (restart.reason == R::playbackJump)
            restartText = "New measurement: playback moved from " + theme::timeLabel ((double) restart.from / fs, true)
                          + " to " + theme::timeLabel ((double) restart.to / fs, true);
        else if (restart.reason == R::settingsChanged)
            restartText = "New measurement: sample rate or channel layout changed";
        else
            restartText = {};
        repaint (statusArea);
    }
}

void AdaAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    if (! activeShown)
    {
        g.setColour (theme::textSecondary);
        g.setFont (juce::FontOptions (15.0f));
        g.drawFittedText ("Unsupported sample rate or channel layout.\nADA needs mono or stereo at a multiple of 100 Hz.",
                          getLocalBounds().reduced (16), juce::Justification::centred, 3);
        return;
    }
    if (restartText.isNotEmpty())
    {
        g.setColour (theme::textMuted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (restartText, statusArea, juce::Justification::centredLeft, true);
    }
    if (frozenShown)
    {
        auto pill = getLocalBounds().reduced (gap).removeFromBottom (controlsHeight).removeFromRight (200).reduced (0, 4);
        g.setColour (theme::grid);
        g.fillRoundedRectangle (pill.toFloat(), 11.0f);
        g.setColour (theme::textPrimary);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Frozen (transport stopped)", pill, juce::Justification::centred);
    }
}

void AdaAudioProcessorEditor::resized()
{
    for (juce::Component* c : { (juce::Component*) &graph, (juce::Component*) &meter, (juce::Component*) &zoneBar })
        c->setVisible (activeShown);
    for (auto& r : readouts)
        r->setVisible (activeShown);

    auto area = getLocalBounds().reduced (gap);

    auto controls = area.removeFromBottom (controlsHeight);
    resetButton.setBounds (controls.removeFromLeft (80));
    controls.removeFromLeft (12);
    resetOnPlayToggle.setBounds (controls.removeFromLeft (280));
    freezeToggle.setBounds (controls.removeFromLeft (180));
    controls.removeFromLeft (8);
    statusArea = controls.withTrimmedRight (210); // room for the "Frozen" badge
    area.removeFromBottom (gap);

    auto strip = area.removeFromTop (readoutsHeight);
    const int cellW = (strip.getWidth() - (numFields - 1) * 4) / numFields;
    for (int i = 0; i < numFields; ++i)
        readouts[(size_t) i]->setBounds (strip.getX() + i * (cellW + 4), strip.getY(), cellW, strip.getHeight());
    area.removeFromTop (gap);

    zoneBar.setBounds (area.removeFromBottom (zoneBarHeight));
    area.removeFromBottom (gap);
    meter.setBounds (area.removeFromRight (meterWidth));
    area.removeFromRight (gap);
    graph.setBounds (area);
}
