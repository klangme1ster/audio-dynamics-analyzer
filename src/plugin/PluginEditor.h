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

    void setLabel (const juce::String& newLabel, const juce::String& newUnit, const juce::String& help);
    void setValue (float newValue, bool alarm = false);
    void paint (juce::Graphics&) override;

private:
    juce::String label, unit;
    Kind kind;
    float value = std::numeric_limits<float>::quiet_NaN();
    bool alarm = false;
};

// Source bar (live input / analysed file), readouts strip, scrolling history with PSR meter,
// zone distribution bar, controls (Phase 4). Audio files can be dropped anywhere on the window.
class AdaAudioProcessorEditor : public juce::AudioProcessorEditor,
                                public juce::FileDragAndDropTarget,
                                private juce::Timer
{
public:
    explicit AdaAudioProcessorEditor (AdaAudioProcessor&);
    ~AdaAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    enum Field { psr, psrMin, shortTerm, momentary, integrated, lra, truePeak3s, truePeakMax, plr, numFields };

    void timerCallback() override;
    void updateFromHistory();
    void updateFromFile();
    void updateRestartStatus();
    void showView (AdaAudioProcessor::View view);
    void refreshDevices();
    void startOutputCapture();
    const ada::HistoryStore& liveLikeStore();
    void analyseFile (const juce::File& file, const std::optional<ada::ChannelLayout>& layout = std::nullopt);
    void updateLayoutBox();
    void chooseLayout (const std::optional<ada::ChannelLayout>& layout);
    void editCustomLayout (int channels);
    void openFileChooser();
    void updateWindowTitle();
    void applyTarget();
    void showTargetMeasurement (std::optional<double> integrated, std::optional<double> truePeakMax);
    void editCustomTarget();
    bool contentVisible() const noexcept { return processor.view() != AdaAudioProcessor::View::live || activeShown; }

    AdaAudioProcessor& processor;
    juce::TooltipWindow tooltipWindow { this, 400 };

    juce::TextButton liveModeButton { "Live" }, outputModeButton { "Output" }, fileModeButton { "File" }; // view switch
    juce::ComboBox deviceBox;                                  // output view: Windows output device
    juce::ToggleButton outputToggle { "Output analysis" };    // output view: capture on/off
    juce::Label sourceLabel;
    juce::ComboBox layoutBox;                                  // channel layout of 3+ channel input or files
    std::vector<ada::ChannelLayout> layoutItems;               // presets offered by layoutBox (id 10 + index)
    juce::String shownLayoutKey;
    std::unique_ptr<juce::AlertWindow> layoutDialog;
    juce::TextButton openButton { "Open file..." };
    juce::ToggleButton liveAnalysisToggle { "Live analysis" };
    juce::AudioProcessorValueTreeState::ButtonAttachment liveAnalysisAttachment;
    std::unique_ptr<juce::FileChooser> chooser;

    std::array<std::unique_ptr<Readout>, numFields> readouts;
    HistoryGraph graph;
    PsrMeter meter;
    ZoneBar zoneBar;
    TargetPanel targetPanel;
    int shownTargetRevision = -1;
    std::unique_ptr<juce::AlertWindow> customDialog; // owned, so it closes with the editor
    juce::TextButton resetButton { "Reset" };
    juce::ToggleButton resetOnPlayToggle { "New measurement when playback jumps" };
    juce::ToggleButton freezeToggle { "Freeze while stopped" };
    juce::ToggleButton monitorToggle { "Play input to output" }; // standalone only
    juce::AudioProcessorValueTreeState::ButtonAttachment resetOnPlayAttachment, freezeAttachment;
    juce::Label statusLabel; // why the current measurement started (automatic restarts only), with tooltip

    std::uint64_t shownVersion = 0, shownFileVersion = 0, shownRunId = 0;
    const ada::HistoryStore emptyStore { 1 }; // graph source when no file history exists
    std::uint32_t shownRestartCount = 0;
    bool frozenShown = false, liveStoppedShown = false, activeShown = false, dragHover = false, fileModeShown = false,
         outputModeShown = false;
    std::vector<ada::OutputDevice> deviceList;
    juce::String shownTitle;
    bool sizeRestored = false;
    ada::FileAnalysis::State shownFileState = ada::FileAnalysis::State::idle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaAudioProcessorEditor)
};
