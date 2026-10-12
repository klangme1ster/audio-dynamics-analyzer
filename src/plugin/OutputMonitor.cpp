#include "OutputMonitor.h"

#include "DynamicsEngine.h"
#include "InterleavedConvert.h"

#include <array>
#include <cstring>

#if JUCE_WINDOWS
 #include <audioclient.h>
 #include <mmdeviceapi.h>
 #include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace ada
{

#if JUCE_WINDOWS
namespace
{
    // Defined here (published values) instead of via functiondiscoverykeys_devpkey.h / ksmedia.h,
    // whose GUID definitions need extra setup next to JUCE's lean windows.h.
    constexpr PROPERTYKEY friendlyNameKey { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

    // KSDATAFORMAT_SUBTYPE_*: {0000000X-0000-0010-8000-00AA00389B71}, X = format tag
    bool isSubtype (const GUID& g, unsigned long tag)
    {
        static constexpr unsigned char tail[8] = { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
        return g.Data1 == tag && g.Data2 == 0x0000 && g.Data3 == 0x0010 && std::memcmp (g.Data4, tail, 8) == 0;
    }

    juce::String deviceFriendlyName (IMMDevice* device)
    {
        ComPtr<IPropertyStore> props;
        if (FAILED (device->OpenPropertyStore (STGM_READ, &props)))
            return "Unknown device";
        PROPVARIANT value;
        PropVariantInit (&value);
        juce::String name ("Unknown device");
        if (SUCCEEDED (props->GetValue (friendlyNameKey, &value)) && value.vt == VT_LPWSTR)
            name = juce::String (value.pwszVal);
        PropVariantClear (&value);
        return name;
    }

    juce::String endpointId (IMMDevice* device)
    {
        LPWSTR id = nullptr;
        if (FAILED (device->GetId (&id)))
            return {};
        juce::String result (id);
        CoTaskMemFree (id);
        return result;
    }

    ComPtr<IMMDevice> openDevice (const juce::String& id)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator))))
            return {};
        ComPtr<IMMDevice> device;
        if (id.isEmpty())
            enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &device);
        else
            enumerator->GetDevice (id.toWideCharPointer(), &device);
        return device;
    }

    bool sampleFormatOf (const WAVEFORMATEX& wf, SampleFormat& format)
    {
        WORD tag = wf.wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE && wf.cbSize >= 22)
        {
            const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&> (wf);
            tag = isSubtype (ext.SubFormat, WAVE_FORMAT_IEEE_FLOAT) ? (WORD) WAVE_FORMAT_IEEE_FLOAT
                  : isSubtype (ext.SubFormat, WAVE_FORMAT_PCM)        ? (WORD) WAVE_FORMAT_PCM
                                                                      : (WORD) 0;
        }
        if (tag == WAVE_FORMAT_IEEE_FLOAT && wf.wBitsPerSample == 32)
            format = SampleFormat::float32;
        else if (tag == WAVE_FORMAT_PCM && wf.wBitsPerSample == 16)
            format = SampleFormat::int16;
        else if (tag == WAVE_FORMAT_PCM && wf.wBitsPerSample == 24)
            format = SampleFormat::int24;
        else if (tag == WAVE_FORMAT_PCM && wf.wBitsPerSample == 32)
            format = SampleFormat::int32;
        else
            return false;
        return true;
    }

    // Sink on the capture thread: never drops (waits for the message thread when the FIFO is full)
    struct CaptureSink : MetricsSink
    {
        MetricsFifo& fifo;
        juce::Thread& thread;
        CaptureSink (MetricsFifo& f, juce::Thread& t) : fifo (f), thread (t) {}
        void push (const DynamicsMetrics& frame) noexcept override
        {
            while (! fifo.tryPush (frame) && ! thread.threadShouldExit())
                juce::Thread::sleep (2);
        }
    };
} // namespace
#endif

// ---------------------------------------------------------------------------------------------

class OutputMonitor::Capture : public juce::Thread
{
public:
    Capture (juce::String id, MetricsFifo& f, std::atomic<float>& clip)
        : juce::Thread ("ADA output capture"), deviceId (std::move (id)), fifo (f), clipThreshold (clip)
    {
    }

    ~Capture() override { stopThread (2000); }

    // Opens the device on the calling (message) thread to report errors and the format at once;
    // the capture itself runs on the thread.
    bool open (juce::String& errorOut, double& rateOut, int& channelsOut)
    {
       #if JUCE_WINDOWS
        auto device = openDevice (deviceId);
        if (device == nullptr)
            return fail (errorOut, "The output device is not available.");
        ComPtr<IAudioClient> client;
        if (FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, &client)))
            return fail (errorOut, "Cannot open the output device.");
        WAVEFORMATEX* mix = nullptr;
        if (FAILED (client->GetMixFormat (&mix)) || mix == nullptr)
            return fail (errorOut, "Cannot read the output device's format.");
        const WAVEFORMATEX wf = *mix;
        const bool known = sampleFormatOf (*mix, format);
        CoTaskMemFree (mix);
        if (! known)
            return fail (errorOut, "Unsupported output sample format.");
        if (! DynamicsEngine::supportsSampleRate ((double) wf.nSamplesPerSec))
            return fail (errorOut, "The output runs at " + juce::String ((int) wf.nSamplesPerSec)
                                       + " Hz; ADA needs a multiple of 100 Hz.");
        rate = (double) wf.nSamplesPerSec;
        deviceChannels = (int) wf.nChannels;
        rateOut = rate;
        channelsOut = deviceChannels;
        return true;
       #else
        juce::ignoreUnused (rateOut, channelsOut);
        return fail (errorOut, "Output capture is only available on Windows.");
       #endif
    }

    bool hasFailed() const noexcept { return failed.load (std::memory_order_acquire); }
    const juce::String& failure() const noexcept { return failureText; } // after hasFailed()

    void requestReset() noexcept { engine.requestReset(); }

    void run() override
    {
       #if JUCE_WINDOWS
        if (FAILED (CoInitializeEx (nullptr, COINIT_MULTITHREADED)))
            return reportFailure ("Cannot initialise Windows audio.");
        runCapture();
        CoUninitialize();
       #endif
    }

private:
    static bool fail (juce::String& errorOut, const juce::String& text)
    {
        errorOut = text;
        return false;
    }

    void reportFailure (const juce::String& text)
    {
        failureText = text;
        failed.store (true, std::memory_order_release);
    }

   #if JUCE_WINDOWS
    void runCapture()
    {
        auto device = openDevice (deviceId);
        ComPtr<IAudioClient> client;
        WAVEFORMATEX* mix = nullptr;
        if (device == nullptr || FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, &client))
            || FAILED (client->GetMixFormat (&mix)))
            return reportFailure ("The output device is not available.");

        constexpr REFERENCE_TIME bufferDuration = 2000000; // 200 ms
        const HRESULT init = client->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                                 bufferDuration, 0, mix, nullptr);
        const int srcChannels = (int) mix->nChannels;
        CoTaskMemFree (mix);
        ComPtr<IAudioCaptureClient> captureClient;
        if (FAILED (init) || FAILED (client->GetService (IID_PPV_ARGS (&captureClient))))
            return reportFailure ("Cannot capture the output device (loopback not available).");

        const int engineChannels = (std::min) (srcChannels, DynamicsEngine::maxChannels);
        engine.prepare (rate, engineChannels);
        CaptureSink sink (fifo, *this);

        constexpr int maxBlock = 48000;
        std::vector<float> planar ((size_t) (maxBlock * DynamicsEngine::maxChannels), 0.0f);
        std::array<float*, DynamicsEngine::maxChannels> ptrs { planar.data(), planar.data() + maxBlock };
        std::array<const float*, DynamicsEngine::maxChannels> cptrs { ptrs[0], ptrs[1] };

        if (FAILED (client->Start()))
            return reportFailure ("Cannot start capturing the output device.");

        // Windows delivers nothing while the device plays nothing; silence is filled in from the
        // clock so the time axis keeps running and pauses are measured as silence.
        double lastData = juce::Time::getMillisecondCounterHiRes();
        while (! threadShouldExit())
        {
            wait (5);
            engine.setClipThreshold (clipThreshold.load (std::memory_order_relaxed));
            bool gotData = false;
            UINT32 packet = 0;
            for (;;)
            {
                const HRESULT hr = captureClient->GetNextPacketSize (&packet);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED || FAILED (hr))
                {
                    client->Stop();
                    return reportFailure ("The output device was removed or changed.");
                }
                if (packet == 0)
                    break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED (captureClient->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                    break;
                for (UINT32 done = 0; done < frames;)
                {
                    const int n = (int) (std::min<UINT32>) ((UINT32) maxBlock, frames - done);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                        for (int ch = 0; ch < engineChannels; ++ch)
                            std::fill (ptrs[(size_t) ch], ptrs[(size_t) ch] + n, 0.0f);
                    else
                        interleavedToPlanar (data + (size_t) done * (size_t) srcChannels * (size_t) bytesPerSample(),
                                             format, srcChannels, n, ptrs.data(), engineChannels);
                    engine.process (cptrs.data(), n, sink);
                    done += (UINT32) n;
                }
                captureClient->ReleaseBuffer (frames);
                gotData = true;
            }

            const double now = juce::Time::getMillisecondCounterHiRes();
            if (gotData)
                lastData = now;
            else if (now - lastData > 1000.0)
            {
                // More than a second without packets and without our own silence fill: the thread
                // or the computer was stalled (e.g. sleep). Skip the gap instead of inventing it.
                lastData = now - 50.0;
            }
            else if (now - lastData > 50.0)
            {
                const int n = (int) (std::min) ((double) maxBlock, (now - lastData) * rate / 1000.0);
                for (int ch = 0; ch < engineChannels; ++ch)
                    std::fill (ptrs[(size_t) ch], ptrs[(size_t) ch] + n, 0.0f);
                engine.process (cptrs.data(), n, sink);
                lastData += (double) n * 1000.0 / rate;
            }
        }
        client->Stop();
    }

    int bytesPerSample() const noexcept
    {
        return format == SampleFormat::int16 ? 2 : format == SampleFormat::int24 ? 3 : 4;
    }
   #endif

    juce::String deviceId;
    MetricsFifo& fifo;
    std::atomic<float>& clipThreshold;
    DynamicsEngine engine;
    SampleFormat format = SampleFormat::float32;
    double rate = 48000.0;
    int deviceChannels = 2;
    std::atomic<bool> failed { false };
    juce::String failureText;
};

// ---------------------------------------------------------------------------------------------

OutputMonitor::OutputMonitor() = default;

OutputMonitor::~OutputMonitor()
{
    stopTimer();
    capture.reset();
}

bool OutputMonitor::isSupported() noexcept
{
   #if JUCE_WINDOWS
    return true;
   #else
    return false;
   #endif
}

std::vector<OutputDevice> OutputMonitor::devices() const
{
    std::vector<OutputDevice> result;
   #if JUCE_WINDOWS
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator))))
        return result;
    ComPtr<IMMDevice> defaultDevice;
    juce::String defaultName ("none");
    if (SUCCEEDED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &defaultDevice)))
        defaultName = deviceFriendlyName (defaultDevice.Get());
    result.push_back ({ {}, "Default output (" + defaultName + ")" });

    ComPtr<IMMDeviceCollection> collection;
    UINT count = 0;
    if (SUCCEEDED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &collection))
        && SUCCEEDED (collection->GetCount (&count)))
        for (UINT i = 0; i < count; ++i)
        {
            ComPtr<IMMDevice> device;
            if (SUCCEEDED (collection->Item (i, &device)))
                result.push_back ({ endpointId (device.Get()), deviceFriendlyName (device.Get()) });
        }
   #endif
    return result;
}

bool OutputMonitor::start (const juce::String& id)
{
    stop();
    error.clear();
    currentId = id;
    currentName = {};
    for (const auto& d : devices())
        if (d.id == id)
            currentName = d.name;
    ++stateChanges;

    auto next = std::make_unique<Capture> (id, fifo, clipThreshold);
    if (! next->open (error, rate, deviceChannels))
        return false;
    history.restart(); // a new capture engine: new measurement, epochs start at 0 again
    resolvedDefaultId = id.isEmpty() ? defaultEndpointId() : juce::String();
    ticksSinceDefaultCheck = 0;
    capture = std::move (next);
    capture->startThread (juce::Thread::Priority::high);
    startTimerHz (30);
    return true;
}

void OutputMonitor::stop()
{
    stopTimer();
    if (capture != nullptr)
    {
        capture.reset(); // joins the thread
        fifo.drain ([this] (const DynamicsMetrics& f) { history.push (f); });
        ++stateChanges;
    }
}

void OutputMonitor::requestReset()
{
    if (capture != nullptr)
        capture->requestReset();
    history.clearForReset();
}

void OutputMonitor::timerCallback()
{
    fifo.drain ([this] (const DynamicsMetrics& f) { history.push (f); });
    if (capture != nullptr && capture->hasFailed())
    {
        error = capture->failure();
        stop();
        return;
    }

    // "Default output" follows the Windows setting: once a second, check whether it moved
    if (capture != nullptr && currentId.isEmpty() && ++ticksSinceDefaultCheck >= 30)
    {
        ticksSinceDefaultCheck = 0;
        const auto now = defaultEndpointId();
        if (now.isNotEmpty() && now != resolvedDefaultId)
            start ({}); // new device: a new measurement
    }
}

juce::String OutputMonitor::defaultEndpointId()
{
   #if JUCE_WINDOWS
    if (auto device = openDevice ({}))
        return endpointId (device.Get());
   #endif
    return {};
}

} // namespace ada
