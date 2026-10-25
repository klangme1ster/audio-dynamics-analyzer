#pragma once

#include "ChannelLayout.h"
#include "KWeighting.h"
#include "LoudnessHistogram.h"
#include "TruePeakDetector.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace ada
{

// One frame per completed 100 ms bin n, emitted D samples after the bin ends (spec 3.3, 4).
// Levels in LUFS / dBTP; -inf for silence, NaN where undefined.
struct DynamicsMetrics
{
    std::int64_t binIndex;     // n since the last reset; bin end time = (n + 1) * 0.1 s
    std::uint32_t epoch;       // incremented by every reset
    float momentaryLUFS;       // M[n], 400 ms window ending at the bin end
    float shortTermLUFS;       // S[n], 3 s window ending at the bin end
    float momentaryPeakLUFS;   // max M over the 10 ms read-outs ending within this bin
    float shortTermPeakLUFS;   // max S over the 10 ms read-outs ending within this bin
    float maxMomentaryLUFS;    // max M on the 10 ms grid since reset
    float maxShortTermLUFS;    // max S on the 10 ms grid since reset
    float binPeakDBTP;         // P[n]
    float truePeak3sDBTP;      // max P over bins n-29..n
    float truePeakMaxDBTP;     // max P since reset (complete bins)
    float psr;                 // TP3s - S, ungated; NaN for n < 29 (spec 3.7 gate is applied by consumers)
    float integratedLUFS;      // running I (NaN while undefined)
    float lra;                 // running LRA (NaN while undefined)
    float lraGateLUFS;         // running LRA relative gate, used as the PSR statistics gate
    bool isClipping;           // truePeak3sDBTP > clip threshold
};

class MetricsSink
{
public:
    virtual ~MetricsSink() = default;
    virtual void push (const DynamicsMetrics& frame) noexcept = 0;
};

// Real-time loudness / true-peak / PSR engine (spec 3, 4).
// prepare() allocates; process() is allocation-free, lock-free and wait-free.
class DynamicsEngine
{
public:
    static constexpr int maxChannels = ChannelLayout::maxChannels;
    // Mono / stereo need no layout; hosts and files with more channels must supply one (spec 3.11)
    static constexpr int maxPlainChannels = 2;

    static bool supportsSampleRate (double sampleRate) noexcept;

    // Throws std::invalid_argument for unsupported rates or invalid layouts.
    void prepare (double sampleRate, const ChannelLayout& channelLayout);
    // Mono or stereo by channel count; more channels throw (they need a layout).
    void prepare (double sampleRate, int numChannels);

    void process (const float* const* input, int numFrames, MetricsSink& sink) noexcept;

    // Any thread. Executed by process() at the next 100 ms bin boundary.
    void requestReset() noexcept { resetRequested.store (true, std::memory_order_release); }
    void setClipThreshold (float dbtp) noexcept { clipThreshold.store (dbtp, std::memory_order_relaxed); }

    // Offline end of stream: flushes the interpolator so the last complete bin is emitted and
    // the trailing partial bin counts toward the max true peak. Not for the audio thread.
    struct Final
    {
        std::int64_t frames;
        double truePeakMaxDBTP;
    };
    Final finish (MetricsSink& sink) noexcept;

    int oversamplingFactor() const noexcept { return truePeak.oversampling(); }
    int latencySamples() const noexcept { return truePeak.delay(); }
    double sampleRate() const noexcept { return fs; }
    int channels() const noexcept { return layout_.count; }
    const ChannelLayout& layout() const noexcept { return layout_; }

private:
    void resetState() noexcept;
    void processFrame (const double* x, MetricsSink& sink) noexcept;
    void advanceTruePeak (double magnitude, MetricsSink& sink) noexcept;
    void onSubBinComplete() noexcept;
    void closePeakBin (MetricsSink& sink) noexcept;
    double sumLastSubBins (int count) const noexcept;

    static constexpr int subBinsPerWindowS = 300, subBinsPerWindowM = 40, peakBins = 30;

    double fs = 48000.0;
    // The LFE (weight 0) is neither filtered nor measured: the engine works on the measured
    // channels only, weighted by G_i (spec 3.11). All fixed-size; set in prepare().
    ChannelLayout layout_ = ChannelLayout::stereo();
    int measuredCount = 2;
    std::array<int, maxChannels> measuredIndex {};
    std::array<double, maxChannels> measuredWeight {};
    int binSize = 4800, subSize = 480, delay = 0;
    std::array<KWeightingFilter, maxChannels> kWeighting;
    TruePeakDetector truePeak;
    LoudnessHistogram integratedHistogram, rangeHistogram;

    // energy path
    std::int64_t samplePos = 0, subCount = 0;
    int subPos = 0;
    double subAccumulator = 0.0;
    std::array<double, subBinsPerWindowS> subRing {};
    double binMaxM = 0.0, binMaxS = 0.0, maxM = 0.0, maxS = 0.0; // as LUFS, -inf initially

    struct Latched
    {
        std::int64_t bin = -1;
        double m = 0.0, s = 0.0, peakM = 0.0, peakS = 0.0;
    } latched;

    // true-peak path
    double peakAccumulator = 0.0, peakMax = 0.0;
    std::array<double, peakBins> peakRing {};

    std::uint32_t epoch = 0;
    bool prepared = false;
    std::atomic<bool> resetRequested { false };
    std::atomic<float> clipThreshold { 0.0f };
};

} // namespace ada
