#include "PluginEditor.h"

#include "Theme.h"

#if JucePlugin_Build_Standalone
 #include <juce_audio_utils/juce_audio_utils.h> // device classes the standalone header needs
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

#include <cmath>

namespace
{
constexpr float loudnessFloor = -70.0f; // BS.1770 absolute gate; anything below reads as "< -70"
constexpr int sourceBarHeight = 28, readoutsHeight = 54, zoneBarHeight = 72, controlsHeight = 30, meterWidth = 170,
              gap = 8;

struct ReadoutText
{
    const char* label;
    const char* unit;
    const char* help;
};

// Live metering: values right now / since the last reset
const ReadoutText liveText[] = {
    { "PSR", "dB", "Peak-to-short-term ratio: true peak minus short-term loudness over the same 3-second window.\n"
             "Shows how hard the transients are limited right now. Lower = more squashed.\n"
             "12 dB or more: open  |  10-12: controlled  |  8-10: heavy  |  below 8: over-limited.\n"
             "Blank during the first 3 s, in silence and in very quiet passages." },
    { "PSR min", "dB", "Lowest PSR since the last reset: the most compressed 3-second stretch so far.\n"
                 "Silence, very quiet passages and the first 3 s are ignored." },
    { "Short-term", "LUFS", "Short-term loudness (EBU R128 \"S\"): perceived loudness of the last 3 seconds." },
    { "Momentary", "LUFS", "Momentary loudness (EBU R128 \"M\"): perceived loudness of the last 400 ms.\n"
                   "Follows the music closely; good for spotting loud hits." },
    { "Integrated", "LUFS", "Integrated loudness: average loudness since the last reset, ignoring silence and quiet parts (BS.1770 gating).\n"
                    "This is what streaming services normalise to: Spotify and YouTube about -14 LUFS, Apple Music about -16 LUFS.\n"
                    "With normalisation on, a louder track is simply turned down on playback." },
    { "LRA", "LU", "Loudness range (EBU Tech 3342): how much the loudness varies between quiet and loud sections\n"
             "(spread from the 10th to the 95th percentile of short-term loudness).\n"
             "Measures macro-dynamics, e.g. verse vs. chorus, not transient limiting." },
    { "True peak 3 s", "dBTP", "Highest true peak in the last 3 seconds, including peaks between samples\n"
                       "(BS.1770 oversampling: 4x at 44.1/48 kHz, 2x at 88.2/96 kHz).\n"
                       "This is the peak half of the PSR. Shows CLIP when it exceeds the clip threshold (default 0 dBTP)." },
    { "Max true peak", "dBTP", "Highest true peak since the last reset.\n"
                       "Keep it at or below -1 dBTP for streaming: lossy encoding (AAC, MP3) adds up to about 1 dB of overshoot." },
    { "PLR", "dB", "Peak-to-loudness ratio: max true peak minus integrated loudness.\n"
             "The headroom of the whole track. Loud modern masters: about 8-10 dB; dynamic recordings: 14 dB or more." },
};

// File analysis: values over the whole file (same definitions as the Python scanner's report)
const ReadoutText fileText[] = {
    { "PSR mean", "dB", "Average PSR over the measured parts of the file (silence, very quiet passages and the first 3 s excluded).\n"
                  "12 dB or more: open  |  10-12: controlled  |  8-10: heavy  |  below 8: over-limited." },
    { "PSR min", "dB", "Lowest PSR in the file: its most compressed 3-second stretch." },
    { "Max S", "LUFS", "Maximum short-term loudness: the loudest 3-second window of the file (EBU R128 \"Max S\")." },
    { "Max M", "LUFS", "Maximum momentary loudness: the loudest 400 ms window of the file (EBU R128 \"Max M\")." },
    { "Integrated", "LUFS", "Integrated loudness of the whole file (BS.1770 gating).\n"
                    "Streaming targets: Spotify and YouTube about -14 LUFS, Apple Music about -16 LUFS." },
    { "LRA", "LU", "Loudness range of the whole file (EBU Tech 3342): spread between its quiet and loud sections." },
    { "PSR P10", "dB", "The PSR the file stays below for 10 % of its measured time.\n"
                 "More robust than the minimum: below 10 dB means large parts are limited hard." },
    { "Max true peak", "dBTP", "Highest true peak in the file, including the last partial 100 ms.\n"
                       "At or below -1 dBTP leaves room for the overshoot of lossy encoding." },
    { "PLR", "dB", "Peak-to-loudness ratio of the file: max true peak minus integrated loudness." },
};

std::optional<float> toFloat (const std::optional<double>& v)
{
    return v ? std::optional<float> ((float) *v) : std::nullopt;
}
} // namespace

// ---------------------------------------------------------------------------------------------

Readout::Readout (juce::String l, juce::String u, Kind k, const juce::String& help)
    : label (std::move (l)), unit (std::move (u)), kind (k)
{
    setTooltip (help);
}

void Readout::setLabel (const juce::String& newLabel, const juce::String& newUnit, const juce::String& help)
{
    setTooltip (help);
    if (newLabel != label || newUnit != unit)
    {
        label = newLabel;
        unit = newUnit;
        repaint();
    }
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
    // shrink the value until value and unit fit the cell; drop the unit only in very narrow cells
    const juce::FontOptions unitFont (11.0f);
    const int unitW = showUnit ? juce::GlyphArrangement::getStringWidthInt (juce::Font (unitFont), unit) + 4 : 0;
    auto valueWidth = [&text] (float s) {
        return juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (s)), text);
    };
    float size = 22.0f;
    while (size > 15.0f && valueWidth (size) + unitW > area.getWidth())
        size -= 1.0f;
    while (size > 13.0f && valueWidth (size) > area.getWidth())
        size -= 1.0f;
    const juce::FontOptions valueFont (size);
    const int w = valueWidth (size);
    g.setColour (alarm ? theme::zoneColours[3] : theme::textPrimary);
    g.setFont (valueFont);
    g.drawText (text, area, juce::Justification::centredLeft);
    if (showUnit && w + unitW <= area.getWidth())
    {
        g.setColour (theme::textMuted);
        g.setFont (unitFont);
        g.drawText (unit, area.withTrimmedLeft (w + 4).withTrimmedTop (6), juce::Justification::centredLeft);
    }
}

// ---------------------------------------------------------------------------------------------

AdaAudioProcessorEditor::AdaAudioProcessorEditor (AdaAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), graph (p.history().store()),
      liveAnalysisAttachment (p.parameters, "liveAnalysis", liveAnalysisToggle),
      resetOnPlayAttachment (p.parameters, "resetOnPlay", resetOnPlayToggle),
      freezeAttachment (p.parameters, "freezeWhenStopped", freezeToggle)
{
    using K = Readout::Kind;
    const K kinds[numFields] = { K::decibel,  K::decibel, K::loudness, K::loudness, K::loudness,
                                 K::decibel,  K::decibel, K::decibel,  K::decibel };
    for (int i = 0; i < numFields; ++i)
    {
        readouts[(size_t) i] = std::make_unique<Readout> (liveText[i].label, liveText[i].unit, kinds[i], liveText[i].help);
        addAndMakeVisible (*readouts[(size_t) i]);
    }
    addAndMakeVisible (graph);
    addAndMakeVisible (meter);
    addAndMakeVisible (zoneBar);
    addAndMakeVisible (targetPanel);
    targetPanel.onSelect = [this] (int index) {
        if (index == ada::customTargetIndex)
        {
            editCustomTarget();
            return;
        }
        processor.setTargetIndex (index);
        applyTarget();
    };

    // Source bar
    sourceLabel.setFont (juce::FontOptions (13.0f));
    sourceLabel.setColour (juce::Label::textColourId, theme::textSecondary);
    sourceLabel.setMinimumHorizontalScale (0.85f);
    addAndMakeVisible (sourceLabel);
    layoutBox.setTooltip ("Which speaker each channel is (spec 3.11): the surround channels weigh +1.5 dB and the LFE is not\n"
                          "measured. The host's bus layout is used when it names the speakers; choose one here if it does not,\n"
                          "or if the channels are in another order (Custom...). A suggestion is only a guess until you pick it.");
    layoutBox.onChange = [this] {
        const int id = layoutBox.getSelectedId();
        if (id == 1)
            chooseLayout (std::nullopt);
        else if (id == 2)
            editCustomLayout (fileModeShown ? processor.fileAnalysis().channels() : processor.liveChannels());
        else if (id >= 10 && id - 10 < (int) layoutItems.size())
            chooseLayout (layoutItems[(size_t) (id - 10)]);
    };
    addChildComponent (layoutBox);
    openButton.setTooltip ("Analyse an audio file (WAV, AIFF, FLAC, Ogg, MP3, WMA) at full speed.\n"
                           "You can also drop a file anywhere on this window.");
    openButton.onClick = [this] { openFileChooser(); };
    addAndMakeVisible (openButton);
    // Live | Output | File view switch; Output exists in the standalone app on Windows only,
    // the file view once a file was opened
    for (auto* b : { &liveModeButton, &outputModeButton, &fileModeButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (1);
        b->setColour (juce::TextButton::buttonOnColourId, theme::shortTerm.darker (0.3f));
        addAndMakeVisible (*b);
    }
    const bool hasOutput = processor.outputMonitor() != nullptr;
    outputModeButton.setVisible (hasOutput);
    liveModeButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    outputModeButton.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    fileModeButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    liveModeButton.setTooltip ("Show live metering of the audio input. Measuring continues in the background in every view.");
    outputModeButton.setTooltip ("Measure what a Windows output device plays (e.g. Spotify, a browser, a media player).\n"
                                 "Programs that play through ASIO bypass Windows and cannot be measured this way;\n"
                                 "use your interface's loopback input or the plugin in your DAW for those.");
    fileModeButton.setTooltip ("Show the analysis of the opened file (open or drop a file first).");
    using V = AdaAudioProcessor::View;
    liveModeButton.onClick = [this] { if (processor.view() != V::live) showView (V::live); };
    outputModeButton.onClick = [this] { if (processor.view() != V::output) showView (V::output); };
    fileModeButton.onClick = [this] { if (processor.view() != V::file) showView (V::file); };

    deviceBox.setTooltip ("The Windows output device to measure. \"Default output\" follows the system setting.");
    deviceBox.onChange = [this] {
        if (outputToggle.getToggleState())
            startOutputCapture();
    };
    addChildComponent (deviceBox);
    outputToggle.setTooltip ("Start or stop measuring the selected output device. Starting begins a new measurement.");
    outputToggle.setColour (juce::ToggleButton::textColourId, theme::textSecondary);
    outputToggle.onClick = [this] {
        if (auto* monitor = processor.outputMonitor())
        {
            if (outputToggle.getToggleState())
                startOutputCapture();
            else
                monitor->stop();
            updateFromHistory();
        }
    };
    addChildComponent (outputToggle);

    liveAnalysisToggle.setTooltip ("Start or stop live analysis of the input. When stopped, the display keeps\n"
                                   "the last values and nothing is measured.");
    liveAnalysisToggle.setColour (juce::ToggleButton::textColourId, theme::textSecondary);
    addChildComponent (liveAnalysisToggle);

    // Controls
    resetButton.setTooltip ("Start a new measurement now (clears the history, integrated loudness, LRA, peaks and PSR statistics).");
    resetOnPlayToggle.setTooltip (
        "Begin a new measurement when the playback position jumps: playback starts somewhere other than\n"
        "where it stopped, or you jump while playing (e.g. back to the song start).\n"
        "Pausing and continuing at the same spot, and cycle/loop playback, keep the measurement running.\n"
        "The reason for the last automatic restart is shown next to these switches.");
    freezeToggle.setTooltip (
        "While the transport is stopped, keep the last values on screen instead of measuring the silence.\n"
        "Turn this off to meter live input while the transport is stopped.");
    monitorToggle.setTooltip ("Play the measured input to the audio output.\n"
                              "Off by default: ADA measures the input without playing it, which avoids feedback.\n"
                              "With speakers instead of headphones, turning this on can cause feedback.");
    for (auto* t : { &resetOnPlayToggle, &freezeToggle, &monitorToggle })
        t->setColour (juce::ToggleButton::textColourId, theme::textSecondary);

    resetButton.onClick = [this] {
        if (outputModeShown)
        {
            if (auto* monitor = processor.outputMonitor())
                monitor->requestReset();
        }
        else
        {
            // The engine resets at its next bin boundary, which may be much later while the transport
            // is frozen; the history clears now and ignores frames from the old measurement.
            processor.requestReset();
            processor.history().clearForReset();
        }
        updateFromHistory();
    };
    addAndMakeVisible (resetButton);
    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setColour (juce::Label::textColourId, theme::textMuted);
    statusLabel.setMinimumHorizontalScale (0.8f);
    addAndMakeVisible (statusLabel);
    // transport options only make sense inside a host
    const bool inHost = processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone;
    addChildComponent (resetOnPlayToggle);
    addChildComponent (freezeToggle);
    resetOnPlayToggle.setVisible (inHost);
    freezeToggle.setVisible (inHost);
    monitorToggle.setToggleState (processor.isMonitoringInput(), juce::dontSendNotification);
    monitorToggle.onClick = [this] { processor.setMonitorInput (monitorToggle.getToggleState()); };
    addChildComponent (monitorToggle);
    monitorToggle.setVisible (! inHost);

   #if JucePlugin_Build_Standalone
    // The output stays silent unless "Play input to output" is on, so JUCE's own input mute (its
    // feedback protection) is not needed and would only hide the input from the analysis.
    if (! inHost)
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
            holder->getMuteInputValue().setValue (false);
   #endif

    activeShown = processor.isAnalysisActive();
    // Read the remembered size first: setResizeLimits() already resizes (and would overwrite it)
    const auto saved = processor.savedEditorSize();
    constexpr int minW = 860, minH = 560, maxW = 2400, maxH = 1600;
    setResizable (true, true);
    setResizeLimits (minW, minH, maxW, maxH);
    setSize (saved.x > 0 ? juce::jlimit (minW, maxW, saved.x) : 980, saved.y > 0 ? juce::jlimit (minH, maxH, saved.y) : 660);
    sizeRestored = true; // from now on, size changes are the user's and are remembered
    applyTarget();
    showView (processor.view());

    // Standalone: "Audio Dynamics Analyzer.exe song.flac" (also Explorer's "Open with") analyses the file
    if (! inHost && processor.takeCommandLineFile())
        for (auto arg : juce::JUCEApplicationBase::getCommandLineParameterArray())
        {
            if (arg == "--output" && hasOutput)
            {
                showView (AdaAudioProcessor::View::output);
                break;
            }
            const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (arg.unquoted());
            if (file.existsAsFile() && processor.fileAnalysis().isSupportedFile (file))
            {
                analyseFile (file);
                break;
            }
        }
    startTimerHz (30);
}

AdaAudioProcessorEditor::~AdaAudioProcessorEditor() { stopTimer(); }

// --- Modes -------------------------------------------------------------------------------------

const ada::HistoryStore& AdaAudioProcessorEditor::liveLikeStore()
{
    auto* monitor = processor.outputMonitor();
    return outputModeShown && monitor != nullptr ? monitor->store() : processor.history().store();
}

void AdaAudioProcessorEditor::refreshDevices()
{
    auto* monitor = processor.outputMonitor();
    if (monitor == nullptr)
        return;
    deviceList = monitor->devices();
    deviceBox.clear (juce::dontSendNotification);
    int selected = 1;
    for (size_t i = 0; i < deviceList.size(); ++i)
    {
        deviceBox.addItem (deviceList[i].name, (int) i + 1);
        if (deviceList[i].id == monitor->deviceId())
            selected = (int) i + 1;
    }
    deviceBox.setSelectedId (selected, juce::dontSendNotification);
}

void AdaAudioProcessorEditor::startOutputCapture()
{
    auto* monitor = processor.outputMonitor();
    const int index = deviceBox.getSelectedId() - 1;
    if (monitor == nullptr || index < 0 || index >= (int) deviceList.size())
        return;
    if (! monitor->start (deviceList[(size_t) index].id))
        outputToggle.setToggleState (false, juce::dontSendNotification);
    shownVersion = 0;
    updateFromHistory();
}

void AdaAudioProcessorEditor::showView (AdaAudioProcessor::View view)
{
    using V = AdaAudioProcessor::View;
    if (view == V::output && processor.outputMonitor() == nullptr)
        view = V::live;
    processor.setView (view);
    const bool shouldShowFile = view == V::file;
    fileModeShown = shouldShowFile;
    outputModeShown = view == V::output;
    const auto& text = shouldShowFile ? fileText : liveText;
    for (int i = 0; i < numFields; ++i)
        readouts[(size_t) i]->setLabel (text[i].label, text[i].unit, text[i].help);
    meter.setTitle (shouldShowFile ? "PSR over the file (dB)" : "PSR now (dB)");
    zoneBar.setTitle (shouldShowFile ? "Time in each PSR zone (whole file)" : "Time in each PSR zone since reset");
    if (shouldShowFile)
    {
        meter.setTooltip ("PSR over the whole file: the bar and the number show the mean.\n"
                          "Solid line: lowest PSR (the most squashed 3 seconds). Dashed line: P10, the PSR the file\n"
                          "stays below 10 % of its measured time.\n"
                          "Green >= 12 open | yellow 10-12 controlled | orange 8-10 heavy | red < 8 over-limited.");
        zoneBar.setTooltip ("How much of the file's measured time fell into each PSR zone.\n"
                            "Silence, very quiet passages and the first 3 seconds are not counted.");
    }
    else
    {
        meter.restoreDefaultTooltip();
        zoneBar.restoreDefaultTooltip();
    }
    liveModeButton.setToggleState (view == V::live, juce::dontSendNotification);
    outputModeButton.setToggleState (view == V::output, juce::dontSendNotification);
    fileModeButton.setToggleState (view == V::file, juce::dontSendNotification);
    liveAnalysisToggle.setVisible (view == V::live);
    deviceBox.setVisible (outputModeShown);
    outputToggle.setVisible (outputModeShown);
    resetButton.setEnabled (! shouldShowFile);
    updateRestartStatus();

    if (outputModeShown)
    {
        refreshDevices();
        auto* monitor = processor.outputMonitor();
        // first visit: start measuring the default output right away
        if (! monitor->isRunning() && monitor->errorText().isEmpty() && monitor->deviceName().isEmpty())
        {
            outputToggle.setToggleState (true, juce::dontSendNotification);
            startOutputCapture();
        }
        outputToggle.setToggleState (monitor->isRunning(), juce::dontSendNotification);
    }

    if (shouldShowFile)
    {
        shownFileVersion = 0;
        shownRunId = std::numeric_limits<std::uint64_t>::max(); // force a store/view refresh
        shownFileState = ada::FileAnalysis::State::idle;
        updateFromFile();
    }
    else
    {
        graph.setStore (liveLikeStore());
        if (outputModeShown)
            graph.setStatusText ("Output");
        else
            graph.setStatusText (processor.isLiveStopped() ? "Live analysis stopped" : juce::String());
        shownVersion = 0;
        updateFromHistory();
        updateRestartStatus();
    }
    shownLayoutKey = {};
    updateLayoutBox();
    resized();
    repaint();
}

void AdaAudioProcessorEditor::analyseFile (const juce::File& file, const std::optional<ada::ChannelLayout>& layout)
{
    processor.fileAnalysis().start (file, layout); // a failure is shown in the source bar
    showView (AdaAudioProcessor::View::file);
}

// The layout box: for live input with 3+ channels and for files that have them (spec 3.11)
void AdaAudioProcessorEditor::updateLayoutBox()
{
    using State = ada::FileAnalysis::State;
    const auto& fa = processor.fileAnalysis();
    int channels = 0;
    bool needs = false, hasChoice = false;
    std::optional<ada::ChannelLayout> current, suggestion, host;
    if (fileModeShown)
    {
        needs = fa.needsLayout();
        channels = fa.channels();
        suggestion = fa.suggestedLayout();
        if (fa.state() == State::running || fa.state() == State::finished)
            current = fa.layout();
        else if (! needs)
            channels = 0; // another failure: nothing to choose
    }
    else if (! outputModeShown)
    {
        channels = processor.liveChannels();
        current = processor.liveLayout();
        host = processor.hostLayout();
        hasChoice = processor.hasLayoutChoice();
    }
    const bool show = channels > 2;
    const juce::String key = juce::String (show ? 1 : 0) + "|" + (fileModeShown ? "f" : "l") + juce::String (channels) + "|"
                             + (current ? current->roleList() : "-") + "|" + (host ? host->roleList() : "-") + "|"
                             + juce::String (needs ? 1 : 0) + juce::String (hasChoice ? 1 : 0);
    if (key == shownLayoutKey)
        return;
    shownLayoutKey = key;
    if (! fileModeShown)
        updateFromHistory(); // the source line names the layout

    layoutBox.clear (juce::dontSendNotification);
    layoutItems.clear();
    if (layoutBox.isVisible() != show)
    {
        layoutBox.setVisible (show);
        resized();
    }
    if (! show)
        return;

    int selected = 0;
    if (host)
        layoutBox.addItem ("Auto: " + juce::String (ada::displayName (*host)) + " (host)", 1);
    for (const auto& preset : { ada::ChannelLayout::surround50(), ada::ChannelLayout::surround51(), ada::ChannelLayout::surround71() })
        if (preset.count == channels)
        {
            layoutItems.push_back (preset);
            const int id = 10 + (int) layoutItems.size() - 1;
            const bool isSuggestion = needs && suggestion && *suggestion == preset;
            layoutBox.addItem (juce::String (preset.name()) + (isSuggestion ? " (suggested)" : ""), id);
            if (current && *current == preset && (fileModeShown || hasChoice || ! host))
                selected = id;
        }
    layoutBox.addItem ("Custom...", 2);
    if (selected == 0 && current)
    {
        if (! fileModeShown && ! hasChoice && host)
            selected = 1;
        else
        {
            layoutBox.addItem (juce::String (current->roleList()), 3);
            selected = 3; // an earlier custom choice
        }
    }
    if (selected != 0)
        layoutBox.setSelectedId (selected, juce::dontSendNotification);
    else
        layoutBox.setTextWhenNothingSelected (juce::String (channels) + " channels: choose a layout");
}

void AdaAudioProcessorEditor::chooseLayout (const std::optional<ada::ChannelLayout>& layout)
{
    if (fileModeShown)
        analyseFile (processor.fileAnalysis().file(), layout);
    else
        processor.setLayoutChoice (layout);
    shownLayoutKey = {};
    updateLayoutBox();
}

void AdaAudioProcessorEditor::editCustomLayout (int channels)
{
    layoutDialog = std::make_unique<juce::AlertWindow> (
        "Custom channel layout",
        "The speaker of each of the " + juce::String (channels) + " channels, in channel order, separated by commas.\n"
        "Speakers: L R C LFE Ls Rs Lb Rb (each at most once). Example for 5.1: L,R,C,LFE,Ls,Rs",
        juce::MessageBoxIconType::NoIcon, this);
    auto* window = layoutDialog.get();
    const auto current = fileModeShown ? std::optional<ada::ChannelLayout> (processor.fileAnalysis().layout()) : processor.liveLayout();
    window->addTextEditor ("roles", current && current->count == channels ? juce::String (current->roleList()) : juce::String(),
                           "Roles");
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [safeThis = juce::Component::SafePointer<AdaAudioProcessorEditor> (this), window, channels] (int result) {
            if (safeThis == nullptr)
                return;
            const auto layout = result == 1 ? ada::ChannelLayout::parse (window->getTextEditorContents ("roles").trim().toStdString())
                                            : std::nullopt;
            if (layout && layout->count == channels)
                safeThis->chooseLayout (layout);
            else
            {
                if (result == 1)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Custom channel layout",
                                                            "That is not a valid layout for " + juce::String (channels)
                                                                + " channels: use each of L, R, C, LFE, Ls, Rs, Lb, Rb at most once.");
                safeThis->shownLayoutKey = {}; // restore the selector
                safeThis->updateLayoutBox();
            }
        }), false);
}

void AdaAudioProcessorEditor::openFileChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose an audio file to analyse", juce::File(),
                                                   processor.fileAnalysis().wildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safeThis = juce::Component::SafePointer<AdaAudioProcessorEditor> (this)] (const juce::FileChooser& fc) {
                              if (safeThis != nullptr && fc.getResult().existsAsFile())
                                  safeThis->analyseFile (fc.getResult());
                          });
}

// --- Drag and drop ---------------------------------------------------------------------------

bool AdaAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (processor.fileAnalysis().isSupportedFile (juce::File (f)))
            return true;
    return false;
}

void AdaAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    dragHover = true;
    repaint();
}

void AdaAudioProcessorEditor::fileDragExit (const juce::StringArray&)
{
    dragHover = false;
    repaint();
}

void AdaAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragHover = false;
    for (const auto& f : files)
        if (processor.fileAnalysis().isSupportedFile (juce::File (f)))
        {
            analyseFile (juce::File (f));
            break;
        }
    repaint();
}

// --- Updates ---------------------------------------------------------------------------------

void AdaAudioProcessorEditor::updateFromHistory()
{
    if (fileModeShown)
        return;
    const auto& store = liveLikeStore();
    auto* monitor = processor.outputMonitor();
    shownVersion = outputModeShown && monitor != nullptr ? monitor->version() : store.version();

    if (outputModeShown && monitor != nullptr)
    {
        juce::String text, details;
        if (monitor->errorText().isNotEmpty())
            text = details = monitor->errorText();
        else if (monitor->isRunning())
        {
            const auto format = juce::String (monitor->sampleRate() / 1000.0, 1) + " kHz"
                                + (monitor->channels() <= 2 ? juce::String()
                                   : monitor->layoutExact() ? ", " + juce::String (ada::displayName (monitor->layout()))
                                                            : juce::String (", front left/right"));
            text = "Measuring the output (" + format + ")";
            details = "Measuring what " + monitor->deviceName() + " plays (" + format + ").";
        }
        else
            text = details = "Output analysis stopped.";
        sourceLabel.setText (text, juce::dontSendNotification);
        sourceLabel.setTooltip (details);
        graph.setStatusText (monitor->isRunning() ? "Output" : "Output analysis stopped");
    }
    else
    {
        juce::String live = "Live input";
        if (processor.liveChannels() > 2)
        {
            const auto layout = processor.liveLayout();
            live += layout ? " (" + juce::String (ada::displayName (*layout)) + ")"
                           : ": " + juce::String (processor.liveChannels()) + " channels, choose a layout";
        }
        sourceLabel.setText (live + ". Drop an audio file here, or open one, to analyse it.", juce::dontSendNotification);
        sourceLabel.setTooltip ({});
    }
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
    auto finite = [] (float v) { return std::isfinite (v) ? std::optional<double> (v) : std::nullopt; };
    showTargetMeasurement (store.empty() ? std::nullopt : finite (store.latest().integratedLUFS),
                           store.empty() ? std::nullopt : finite (store.latest().truePeakMaxDBTP));
    zoneBar.setStats (stats);
    graph.repaint();
}

void AdaAudioProcessorEditor::updateFromFile()
{
    using State = ada::FileAnalysis::State;
    auto& fa = processor.fileAnalysis();
    shownFileVersion = fa.version();
    const auto name = fa.file().getFileName();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // new analysis (or none): new data source and a fresh view; never a dangling store
    if (fa.runId() != shownRunId || fa.store() == nullptr)
    {
        shownRunId = fa.runId();
        graph.setStore (fa.store() != nullptr ? *fa.store() : emptyStore);
    }

    juce::String info;
    if (fa.state() != State::failed && fa.sampleRate() > 0.0)
        info = theme::timeLabel (fa.durationSeconds()) + ", " + juce::String (fa.sampleRate() / 1000.0, 1) + " kHz "
               + juce::String (ada::displayName (fa.layout()));

    switch (fa.state())
    {
        case State::running:
            sourceLabel.setText ("File: " + name + " (" + info + "). Analysing "
                                     + juce::String (juce::roundToInt (fa.progress() * 100.0)) + " %",
                                 juce::dontSendNotification);
            break;
        case State::finished:
        {
            const double speed = fa.analysisSeconds() > 0.0 ? fa.durationSeconds() / fa.analysisSeconds() : 0.0;
            sourceLabel.setText ("File: " + name + " (" + info + "). Analysed in "
                                     + juce::String (fa.analysisSeconds(), 1) + " s ("
                                     + juce::String (juce::roundToInt (speed)) + "x real time)",
                                 juce::dontSendNotification);
            break;
        }
        case State::failed:
            sourceLabel.setText (fa.errorText(), juce::dontSendNotification);
            break;
        case State::idle:
            sourceLabel.setText ("No file. Drop an audio file here, or open one.", juce::dontSendNotification);
            break;
    }
    graph.setStatusText (fa.state() == State::running ? "Analysing " + name : name);

    if (fa.state() == State::finished && fa.summary())
    {
        const auto& s = *fa.summary();
        auto value = [nan] (const std::optional<double>& v) { return v ? (float) *v : nan; };
        readouts[psr]->setValue (value (s.psrMean));
        readouts[psrMin]->setValue (value (s.psrMin));
        readouts[shortTerm]->setValue (value (s.maxShortTerm));
        readouts[momentary]->setValue (value (s.maxMomentary));
        readouts[integrated]->setValue (value (s.integrated));
        readouts[lra]->setValue (value (s.lra));
        readouts[truePeak3s]->setValue (value (s.psrP10));
        readouts[truePeakMax]->setValue (value (s.truePeakMax));
        readouts[plr]->setValue (value (s.plr));
        meter.setValues (toFloat (s.psrMean), toFloat (s.psrMin), toFloat (s.psrP10));
        showTargetMeasurement (s.integrated, s.truePeakMax);

        ada::HistoryStore::PsrStats stats;
        stats.measuredFrames = s.measuredFrames;
        stats.min = toFloat (s.psrMin);
        stats.p10 = toFloat (s.psrP10);
        for (std::size_t z = 0; z < 4; ++z)
            stats.zonePercent[z] = s.zonePct[z] ? (float) *s.zonePct[z] : 0.0f;
        zoneBar.setStats (stats);
    }
    else
    {
        // while analysing: the graph fills up; numbers appear when the whole file is measured
        for (auto& r : readouts)
            r->setValue (nan);
        showTargetMeasurement (std::nullopt, std::nullopt);
        if (fa.store() != nullptr && fa.state() == State::running)
        {
            const auto stats = fa.store()->psrStats();
            meter.setValues ({}, stats.min, stats.p10);
            zoneBar.setStats (stats);
        }
        else
        {
            meter.setValues ({}, {}, {});
            zoneBar.setStats ({});
        }
    }

    if (fa.state() == State::finished && shownFileState != State::finished)
        graph.showAll(); // whole file at a glance once it is done
    shownFileState = fa.state();
    graph.repaint();
}

void AdaAudioProcessorEditor::updateRestartStatus()
{
    const auto restart = processor.lastRestart();
    shownRestartCount = restart.count;
    if (fileModeShown || outputModeShown) // restart reasons describe live metering only
    {
        statusLabel.setText ({}, juce::dontSendNotification);
        statusLabel.setTooltip ({});
        return;
    }
    const double fs = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
    using R = AdaAudioProcessor::RestartReason;
    const juce::String arrow (juce::CharPointer_UTF8 (" \xe2\x86\x92 "));
    if (restart.reason == R::playbackJump)
    {
        const auto from = theme::timeLabel ((double) restart.from / fs, true);
        const auto to = theme::timeLabel ((double) restart.to / fs, true);
        statusLabel.setText ("New measurement: jump " + from + arrow + to, juce::dontSendNotification);
        statusLabel.setTooltip ("The last automatic new measurement started because the playback position jumped:\n"
                                "measured audio ended at " + from + ", playback continued at " + to + ".\n"
                                "Resuming up to " + juce::String (ada::TransportTracker::resumeBackstepSeconds, 1)
                                + " s before where measuring stopped, or within "
                                + juce::String (ada::TransportTracker::jumpToleranceSeconds, 2)
                                + " s after it, continues instead.\n"
                                "Turn off \"New measurement when playback jumps\" to never restart automatically.");
    }
    else if (restart.reason == R::settingsChanged)
    {
        statusLabel.setText ("New measurement: settings changed", juce::dontSendNotification);
        statusLabel.setTooltip ("The host changed the sample rate or the channel layout, so a new measurement started.");
    }
    else
    {
        statusLabel.setText ({}, juce::dontSendNotification);
        statusLabel.setTooltip ({});
    }
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
    auto* monitor = processor.outputMonitor();
    if (monitor != nullptr)
        monitor->setClipThreshold (processor.parameters.getRawParameterValue ("clipThreshold")->load());
    if (fileModeShown)
    {
        if (processor.fileAnalysis().version() != shownFileVersion)
            updateFromFile();
    }
    else if (outputModeShown && monitor != nullptr)
    {
        if (monitor->version() != shownVersion)
            updateFromHistory();
        if (monitor->isRunning() != outputToggle.getToggleState()) // e.g. the device was removed
            outputToggle.setToggleState (monitor->isRunning(), juce::dontSendNotification);
    }
    else if (processor.history().store().version() != shownVersion)
        updateFromHistory();

    const bool liveView = ! fileModeShown && ! outputModeShown;
    const bool frozen = processor.isFrozen() && liveView;
    if (frozen != frozenShown)
    {
        frozenShown = frozen;
        repaint();
    }
    if (processor.lastRestart().count != shownRestartCount)
        updateRestartStatus();

    const bool liveStopped = processor.isLiveStopped() && liveView;
    if (liveStopped != liveStoppedShown)
    {
        liveStoppedShown = liveStopped;
        if (liveView)
            graph.setStatusText (liveStopped ? "Live analysis stopped" : juce::String());
        repaint();
    }
    fileModeButton.setEnabled (processor.fileAnalysis().state() != ada::FileAnalysis::State::idle);
    updateLayoutBox();
    updateWindowTitle();
    if (processor.targetRevision() != shownTargetRevision) // e.g. the host restored a preset
        applyTarget();
}

void AdaAudioProcessorEditor::applyTarget()
{
    shownTargetRevision = processor.targetRevision();
    targetPanel.setSelection (processor.targetIndex(), processor.currentTarget());
    if (fileModeShown)
        updateFromFile();
    else
        updateFromHistory();
}

void AdaAudioProcessorEditor::showTargetMeasurement (std::optional<double> integrated, std::optional<double> truePeakMax)
{
    targetPanel.setMeasurement (integrated, truePeakMax);
    const auto target = processor.currentTarget();
    const auto verdict = targetPanel.verdict();
    graph.setTargetLines (target ? std::optional<float> ((float) target->loudness) : std::nullopt,
                          verdict ? std::optional<float> ((float) verdict->ceiling) : std::nullopt);
}

void AdaAudioProcessorEditor::editCustomTarget()
{
    const auto [loudness, ceiling] = processor.customTarget();
    // owned by the editor (not deleted when dismissed), so closing the plugin window also closes it
    customDialog = std::make_unique<juce::AlertWindow> ("Custom delivery target",
                                                        "Integrated loudness target (-40 to 0 LUFS) and true-peak ceiling (-10 to 0 dBTP).",
                                                        juce::MessageBoxIconType::NoIcon, this);
    auto* window = customDialog.get();
    window->addTextEditor ("loudness", juce::String (loudness, 1), "Target loudness (LUFS)");
    window->addTextEditor ("ceiling", juce::String (ceiling, 1), "True-peak ceiling (dBTP)");
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create (
        [safeThis = juce::Component::SafePointer<AdaAudioProcessorEditor> (this), window] (int result) {
            if (safeThis == nullptr)
                return;
            // a number, with a comma accepted as the decimal separator; anything else keeps the old value
            auto parse = [window] (const juce::String& field) -> std::optional<float> {
                const auto text = window->getTextEditorContents (field).trim().replaceCharacter (',', '.');
                if (text.isEmpty() || ! text.containsOnly ("+-.0123456789") || ! text.containsAnyOf ("0123456789"))
                    return std::nullopt;
                return text.getFloatValue();
            };
            if (result == 1)
            {
                const auto [oldLoudness, oldCeiling] = safeThis->processor.customTarget();
                safeThis->processor.setCustomTarget (parse ("loudness").value_or (oldLoudness),
                                                     parse ("ceiling").value_or (oldCeiling));
                safeThis->processor.setTargetIndex (ada::customTargetIndex);
            }
            safeThis->applyTarget(); // also restores the selector after Cancel
        }), false);
}

void AdaAudioProcessorEditor::updateWindowTitle()
{
    // Standalone only: the plugin cannot change a host's window title
    if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        return;
    juce::String title = JucePlugin_Name;
    const auto& fa = processor.fileAnalysis();
    const juce::String dash (juce::CharPointer_UTF8 (" \xe2\x80\x93 "));
    if (fileModeShown && fa.state() != ada::FileAnalysis::State::idle)
        title += dash + fa.file().getFileName()
                 + (fa.durationSeconds() > 0.0 ? " (" + theme::timeLabel (fa.durationSeconds()) + ")" : juce::String());
    else if (outputModeShown && processor.outputMonitor() != nullptr)
        title += dash + "output: " + processor.outputMonitor()->deviceName();
    else
        title += juce::String (juce::CharPointer_UTF8 (" \xe2\x80\x93 live input"));
    if (title == shownTitle)
        return;
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
    {
        window->setName (title);
        shownTitle = title;
    }
}

// --- Layout and painting ---------------------------------------------------------------------

void AdaAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    if (! contentVisible())
    {
        auto area = getLocalBounds().reduced (gap);
        area.removeFromTop (sourceBarHeight + gap);
        area.removeFromBottom (controlsHeight + gap);
        g.setColour (theme::textSecondary);
        g.setFont (juce::FontOptions (15.0f));
        const bool needsLayout = processor.liveChannels() > 2 && ! processor.liveLayout();
        g.drawFittedText (needsLayout ? juce::String (processor.liveChannels())
                                            + " input channels without a known speaker layout.\n"
                                              "Choose the layout at the top right to start measuring.\n"
                                              "You can still analyse audio files: drop one here or use \"Open file...\"."
                                      : juce::String ("No live metering: unsupported sample rate or channel layout\n"
                                                      "(ADA needs mono, stereo, 5.0, 5.1 or 7.1 at a multiple of 100 Hz).\n"
                                                      "You can still analyse audio files: drop one here or use \"Open file...\"."),
                          area, juce::Justification::centred, 4);
    }
    if (liveStoppedShown || frozenShown)
    {
        auto pill = getLocalBounds().reduced (gap).removeFromBottom (controlsHeight).removeFromRight (200).reduced (0, 4);
        g.setColour (theme::grid);
        g.fillRoundedRectangle (pill.toFloat(), 11.0f);
        g.setColour (theme::textPrimary);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (liveStoppedShown ? "Live analysis stopped" : "Frozen (transport stopped)", pill,
                    juce::Justification::centred);
    }
}

void AdaAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
    if (! dragHover)
        return;
    auto area = getLocalBounds().reduced (gap).toFloat();
    g.setColour (theme::background.withAlpha (0.8f));
    g.fillRoundedRectangle (area, 8.0f);
    g.setColour (theme::shortTerm);
    const float dashes[] = { 8.0f, 6.0f };
    juce::Path outline;
    outline.addRoundedRectangle (area.reduced (2.0f), 8.0f);
    juce::Path dashed;
    juce::PathStrokeType (2.0f).createDashedStroke (dashed, outline, dashes, 2);
    g.fillPath (dashed);
    g.setColour (theme::textPrimary);
    g.setFont (juce::FontOptions (20.0f));
    g.drawText ("Drop to analyse this file", area, juce::Justification::centred);
}

void AdaAudioProcessorEditor::resized()
{
    if (sizeRestored)
        processor.saveEditorSize (getWidth(), getHeight());

    const bool visible = contentVisible();
    for (juce::Component* c : { (juce::Component*) &graph, (juce::Component*) &meter, (juce::Component*) &zoneBar,
                                (juce::Component*) &targetPanel })
        c->setVisible (visible);
    for (auto& r : readouts)
        r->setVisible (visible);

    auto area = getLocalBounds().reduced (gap);

    auto source = area.removeFromTop (sourceBarHeight);
    liveModeButton.setBounds (source.removeFromLeft (64));
    if (outputModeButton.isVisible())
        outputModeButton.setBounds (source.removeFromLeft (72));
    fileModeButton.setBounds (source.removeFromLeft (64));
    source.removeFromLeft (12);
    openButton.setBounds (source.removeFromRight (110));
    source.removeFromRight (8);
    if (liveAnalysisToggle.isVisible())
        liveAnalysisToggle.setBounds (source.removeFromRight (120));
    if (outputToggle.isVisible())
    {
        outputToggle.setBounds (source.removeFromRight (140));
        source.removeFromRight (8);
        deviceBox.setBounds (source.removeFromRight (juce::jmin (300, source.getWidth() / 2)).reduced (0, 2));
    }
    source.removeFromRight (8);
    if (layoutBox.isVisible())
    {
        layoutBox.setBounds (source.removeFromRight (juce::jmin (190, source.getWidth() / 3)).reduced (0, 2));
        source.removeFromRight (8);
    }
    sourceLabel.setBounds (source);
    area.removeFromTop (gap);

    auto controls = area.removeFromBottom (controlsHeight);
    resetButton.setBounds (controls.removeFromLeft (80));
    controls.removeFromLeft (12);
    if (monitorToggle.isVisible())
        monitorToggle.setBounds (controls.removeFromLeft (200));
    else
    {
        resetOnPlayToggle.setBounds (controls.removeFromLeft (280));
        freezeToggle.setBounds (controls.removeFromLeft (180));
    }
    controls.removeFromLeft (8);
    statusLabel.setBounds (controls.withTrimmedRight (210)); // room for the "Frozen" badge
    area.removeFromBottom (gap);

    auto strip = area.removeFromTop (readoutsHeight);
    const int cellW = (strip.getWidth() - (numFields - 1) * 4) / numFields;
    for (int i = 0; i < numFields; ++i)
        readouts[(size_t) i]->setBounds (strip.getX() + i * (cellW + 4), strip.getY(), cellW, strip.getHeight());
    area.removeFromTop (gap);

    // the target panel sits in the corner under the PSR meter; the zone bar spans the graph's width
    auto bottom = area.removeFromBottom (zoneBarHeight);
    targetPanel.setBounds (bottom.removeFromRight (meterWidth));
    bottom.removeFromRight (gap);
    zoneBar.setBounds (bottom);
    area.removeFromBottom (gap);
    meter.setBounds (area.removeFromRight (meterWidth));
    area.removeFromRight (gap);
    graph.setBounds (area);
}
