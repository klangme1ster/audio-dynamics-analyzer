#include "FileAnalysis.h"

#include "MediaFoundationFormat.h"

#include <algorithm>
#include <cmath>

namespace ada
{

namespace
{
    constexpr int maxGraphFrames = 6 * 3600 * 10; // the graph keeps up to 6 hours of a file

    // Hands frames to the message thread; waits (worker thread only) while the FIFO is full.
    struct BlockingFifoSink : MetricsSink
    {
        MetricsFifo& fifo;
        std::function<bool()> shouldStop;
        BlockingFifoSink (MetricsFifo& f, std::function<bool()> stop) : fifo (f), shouldStop (std::move (stop)) {}
        void push (const DynamicsMetrics& frame) noexcept override
        {
            while (! fifo.tryPush (frame))
            {
                if (shouldStop())
                    return;
                juce::Thread::sleep (2);
            }
        }
    };
} // namespace

// ---------------------------------------------------------------------------------------------

class FileAnalysis::Worker : public juce::Thread
{
public:
    Worker (std::unique_ptr<juce::AudioFormatReader> r, MetricsFifo& f, std::atomic<double>& p)
        : juce::Thread ("ADA file analysis"), reader (std::move (r)), fifo (f), progress (p)
    {
    }

    ~Worker() override { stopThread (4000); }

    void run() override
    {
        auto stop = [this] { return threadShouldExit(); };
        BlockingFifoSink sink (fifo, stop);
        final = analyseReader (*reader, sink, stop, &progress, &failedToDecode);
        reader.reset(); // on this thread: decoders such as Media Foundation belong to the reading thread
        done.store (true, std::memory_order_release);
    }

    bool isDone() const noexcept { return done.load (std::memory_order_acquire); }
    const std::optional<DynamicsEngine::Final>& result() const noexcept { return final; } // after isDone()
    bool decodeFailed() const noexcept { return failedToDecode; }                            // after isDone()

private:
    std::unique_ptr<juce::AudioFormatReader> reader;
    MetricsFifo& fifo;
    std::atomic<double>& progress;
    std::optional<DynamicsEngine::Final> final;
    bool failedToDecode = false;
    std::atomic<bool> done { false };
};

// ---------------------------------------------------------------------------------------------

std::optional<DynamicsEngine::Final> FileAnalysis::analyseReader (juce::AudioFormatReader& reader, MetricsSink& sink,
                                                                   const std::function<bool()>& shouldStop,
                                                                   std::atomic<double>* progress, bool* decodeFailed)
{
    if (decodeFailed != nullptr)
        *decodeFailed = false;
    const int channels = (int) reader.numChannels;
    DynamicsEngine engine;
    engine.prepare (reader.sampleRate, channels);

    constexpr int blockSize = 32768;
    juce::AudioBuffer<float> buffer (std::max (2, channels), blockSize);
    const auto length = reader.lengthInSamples;
    for (juce::int64 pos = 0; pos < length; pos += blockSize)
    {
        if (shouldStop())
            return std::nullopt;
        const int n = (int) std::min<juce::int64> (blockSize, length - pos);
        if (! reader.read (&buffer, 0, n, pos, true, true))
        {
            // a partial result would look complete; report the failure instead
            if (decodeFailed != nullptr)
                *decodeFailed = true;
            return std::nullopt;
        }
        engine.process (buffer.getArrayOfReadPointers(), n, sink);
        if (progress != nullptr)
            progress->store ((double) (pos + n) / (double) std::max<juce::int64> (1, length), std::memory_order_relaxed);
    }
    if (shouldStop())
        return std::nullopt;
    return engine.finish (sink);
}

// ---------------------------------------------------------------------------------------------

FileAnalysis::FileAnalysis() { registerAudioFormats (formats); }

FileAnalysis::~FileAnalysis()
{
    stopTimer();
    worker.reset();
}

bool FileAnalysis::isSupportedFile (const juce::File& f) const
{
    return formats.findFormatForFileExtension (f.getFileExtension()) != nullptr;
}

void FileAnalysis::cancel()
{
    stopTimer();
    worker.reset(); // joins the thread
    fifo.clear();
    if (currentState == State::running)
        currentState = State::idle;
    ++changeCount;
}

bool FileAnalysis::start (const juce::File& f)
{
    cancel();
    currentFile = f;
    ++runCount;
    result.reset();
    allFrames.clear();
    history = std::make_unique<HistoryStore> (1); // never leave views without a store
    error.clear();
    workerProgress = 0.0;

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    auto fail = [this] (juce::String message) {
        error = std::move (message);
        currentState = State::failed;
        ++changeCount;
        return false;
    };
    if (reader == nullptr)
        return fail ("Cannot open " + f.getFileName() + " as audio.");
    if (reader->numChannels < 1 || reader->numChannels > (unsigned) DynamicsEngine::maxChannels)
        return fail (f.getFileName() + " has " + juce::String (reader->numChannels)
                     + " channels; ADA analyses mono and stereo files.");
    if (! DynamicsEngine::supportsSampleRate (reader->sampleRate))
        return fail (f.getFileName() + ": sample rate " + juce::String (reader->sampleRate, 0)
                     + " Hz is not supported (needs a multiple of 100 Hz).");

    rate = reader->sampleRate;
    numChannels = (int) reader->numChannels;
    duration = (double) reader->lengthInSamples / rate;
    // lengthInSamples is an estimate for some compressed formats, hence the margin
    // (clamped in double: a broken header must not overflow the int or reserve gigabytes)
    const int estimatedFrames = (int) std::min (std::ceil (duration * 10.0 * 1.05) + 100.0, (double) maxGraphFrames);
    history = std::make_unique<HistoryStore> (std::clamp (estimatedFrames, 100, maxGraphFrames));
    allFrames.reserve ((size_t) estimatedFrames);

    worker = std::make_unique<Worker> (std::move (reader), fifo, workerProgress);
    currentState = State::running;
    startTime = juce::Time::getMillisecondCounterHiRes();
    worker->startThread();
    startTimerHz (30);
    ++changeCount;
    return true;
}

void FileAnalysis::drain()
{
    bool any = false;
    fifo.drain ([this, &any] (const DynamicsMetrics& f) {
        history->push (f);
        allFrames.push_back (f);
        any = true;
    });
    if (any)
        ++changeCount;
}

void FileAnalysis::timerCallback()
{
    if (worker == nullptr || history == nullptr)
    {
        stopTimer();
        return;
    }
    drain();
    if (! worker->isDone())
        return;

    drain(); // frames pushed after the last check
    stopTimer();
    const auto final = worker->result();
    const bool decodeFailed = worker->decodeFailed();
    worker.reset();
    elapsedSeconds = (juce::Time::getMillisecondCounterHiRes() - startTime) / 1000.0;
    if (final)
    {
        result = summarize (allFrames, *final);
        duration = (double) final->frames / rate;
        currentState = State::finished;
    }
    else
    {
        error = decodeFailed ? currentFile.getFileName() + " could not be decoded completely."
                             : "Analysis of " + currentFile.getFileName() + " was cancelled.";
        currentState = State::failed;
    }
    workerProgress = 1.0;
    ++changeCount;
}

} // namespace ada
