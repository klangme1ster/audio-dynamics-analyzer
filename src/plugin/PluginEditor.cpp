#include "PluginEditor.h"

#include "LiveStats.h"

#include <cmath>

namespace
{
const juce::Colour background { 0xff1a1a19 }, textPrimary { 0xfff0efec }, textSecondary { 0xffc3c2b7 },
    critical { 0xffd03b3b }, badge { 0xff383835 };

constexpr float loudnessFloor = -70.0f; // BS.1770 absolute gate; anything below reads as "< -70"

const juce::String minus = juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"));
const juce::String dash = juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"));

juce::String formatNumber (float v)
{
    return juce::String (v, 1).replace ("-", minus);
}
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
    auto area = getLocalBounds().reduced (6);
    g.setColour (textSecondary);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (label + " (" + unit + ")", area.removeFromTop (18), juce::Justification::left);

    juce::String text = dash;
    if (kind == Kind::loudness && (value < loudnessFloor || (std::isinf (value) && value < 0)))
        text = "< " + minus + "70";
    else if (std::isfinite (value))
        text = formatNumber (value);
    if (alarm)
        text += "  CLIP";

    g.setColour (alarm ? critical : textPrimary);
    g.setFont (juce::FontOptions (28.0f));
    g.drawText (text, area, juce::Justification::topLeft);
}

// ---------------------------------------------------------------------------------------------

AdaAudioProcessorEditor::AdaAudioProcessorEditor (AdaAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
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

    resetButton.setTooltip ("Start a new measurement now (clears integrated loudness, LRA, peaks and PSR min).");
    resetOnPlayToggle.setTooltip (
        "When playback starts somewhere other than where it stopped (e.g. back at the song start),\n"
        "begin a new measurement. Pausing and continuing at the same spot keeps the measurement running.");
    freezeToggle.setTooltip (
        "While the transport is stopped, keep the last values on screen instead of measuring the silence.\n"
        "Turn this off to meter live input while the transport is stopped.");
    for (auto* t : { &resetOnPlayToggle, &freezeToggle })
        t->setColour (juce::ToggleButton::textColourId, textSecondary);

    resetButton.onClick = [this] {
        // The engine resets at its next bin boundary, which may be much later while the transport
        // is frozen; blank the display now and ignore frames from the old measurement.
        processor.requestReset();
        resetPendingEpoch = epoch;
        for (auto& r : readouts)
            r->setValue (std::numeric_limits<float>::quiet_NaN());
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
    setResizeLimits (480, 320, 1600, 1200);
    setSize (600, 380);
    startTimerHz (30);
}

AdaAudioProcessorEditor::~AdaAudioProcessorEditor() { stopTimer(); }

void AdaAudioProcessorEditor::timerCallback()
{
    // React to the processor becoming (in)active, e.g. a device or sample-rate change
    if (processor.isAnalysisActive() != activeShown)
    {
        activeShown = processor.isAnalysisActive();
        resized();
        repaint();
    }

    processor.metrics().drain ([&] (const ada::DynamicsMetrics& f) {
        epoch = f.epoch;
        if (resetPendingEpoch && f.epoch == *resetPendingEpoch)
            return; // frame from before the requested reset
        resetPendingEpoch.reset();

        const float nan = std::numeric_limits<float>::quiet_NaN();
        readouts[psr]->setValue (ada::passesLivePsrGate (f) ? f.psr : nan);
        // PSR min is tracked on the audio thread so it survives the editor being closed
        readouts[psrMin]->setValue (processor.psrMinimum());
        readouts[shortTerm]->setValue (f.shortTermLUFS);
        readouts[momentary]->setValue (f.momentaryLUFS);
        readouts[integrated]->setValue (f.integratedLUFS);
        readouts[lra]->setValue (f.lra);
        readouts[truePeak3s]->setValue (f.truePeak3sDBTP, f.isClipping);
        readouts[truePeakMax]->setValue (f.truePeakMaxDBTP);
        readouts[plr]->setValue (f.truePeakMaxDBTP - f.integratedLUFS);
    });

    if (processor.isFrozen() != frozenShown)
    {
        frozenShown = processor.isFrozen();
        repaint();
    }
}

void AdaAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
    if (! activeShown)
    {
        g.setColour (textSecondary);
        g.setFont (juce::FontOptions (15.0f));
        g.drawFittedText ("Unsupported sample rate or channel layout.\nADA needs mono or stereo at a multiple of 100 Hz.",
                          getLocalBounds().reduced (16), juce::Justification::centred, 3);
        return;
    }
    if (frozenShown)
    {
        auto pill = getLocalBounds().reduced (16).removeFromTop (22).removeFromRight (190);
        g.setColour (badge);
        g.fillRoundedRectangle (pill.toFloat(), 11.0f);
        g.setColour (textPrimary);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Frozen (transport stopped)", pill, juce::Justification::centred);
    }
}

void AdaAudioProcessorEditor::resized()
{
    for (auto& r : readouts)
        r->setVisible (activeShown);

    auto area = getLocalBounds().reduced (16);
    auto bottom = area.removeFromBottom (32);
    area.removeFromTop (26); // room for the frozen badge
    area.removeFromBottom (8);

    resetButton.setBounds (bottom.removeFromLeft (80));
    bottom.removeFromLeft (12);
    resetOnPlayToggle.setBounds (bottom.removeFromLeft (bottom.getWidth() * 3 / 5));
    freezeToggle.setBounds (bottom);

    constexpr int columns = 3, rows = (numFields + columns - 1) / columns;
    const int cellW = area.getWidth() / columns, cellH = area.getHeight() / rows;
    for (int i = 0; i < numFields; ++i)
        readouts[(size_t) i]->setBounds (area.getX() + (i % columns) * cellW, area.getY() + (i / columns) * cellH,
                                         cellW, cellH);
}
