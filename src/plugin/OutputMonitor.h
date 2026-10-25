#pragma once

#include "ChannelLayout.h"
#include "HistoryStore.h"
#include "MetricsFifo.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>
#include <vector>

namespace ada
{

struct OutputDevice
{
    juce::String id;   // empty = the system's default output
    juce::String name;
};

// Measures what a Windows output device plays (WASAPI loopback capture), standalone app only.
// A capture thread converts the device's mix to float and feeds its own DynamicsEngine; frames
// reach the message thread through a lock-free FIFO and fill a 90-minute HistoryStore.
// Programs that play through ASIO bypass the Windows mixer and cannot be captured this way.
class OutputMonitor : private juce::Timer
{
public:
    OutputMonitor();
    ~OutputMonitor() override;

    static bool isSupported() noexcept; // Windows only

    // Message thread
    std::vector<OutputDevice> devices() const; // first entry: the default output
    bool start (const juce::String& deviceId);  // stops a running capture first
    void stop();
    void requestReset();                        // new measurement (engine and history)
    void setClipThreshold (float dbtp) noexcept { clipThreshold.store (dbtp); }

    bool isRunning() const noexcept { return capture != nullptr; }
    const juce::String& deviceId() const noexcept { return currentId; }
    const juce::String& deviceName() const noexcept { return currentName; }
    const juce::String& errorText() const noexcept { return error; }
    double sampleRate() const noexcept { return rate; }
    int channels() const noexcept { return deviceChannels; }
    // What is measured of the device: all of mono / stereo / 5.0 / 5.1 / 7.1 (layoutExact), else its front pair
    const ChannelLayout& layout() const noexcept { return measuredLayout; }
    bool layoutExact() const noexcept { return exactLayout; }

    const HistoryStore& store() const noexcept { return history; }
    // Changes on new frames and on state changes (start, stop, error)
    std::uint64_t version() const noexcept { return history.version() + stateChanges; }

private:
    class Capture;
    void timerCallback() override;
    static juce::String defaultEndpointId();

    MetricsFifo fifo;
    HistoryStore history;
    std::unique_ptr<Capture> capture;
    std::atomic<float> clipThreshold { 0.0f };
    juce::String currentId, currentName, error, resolvedDefaultId;
    int ticksSinceDefaultCheck = 0;
    double rate = 0.0;
    int deviceChannels = 0;
    ChannelLayout measuredLayout = ChannelLayout::stereo();
    bool exactLayout = true;
    std::uint64_t stateChanges = 0;
};

} // namespace ada
