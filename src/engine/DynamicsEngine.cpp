#include "DynamicsEngine.h"

#include "Loudness.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ada
{

namespace
{
    constexpr double negInf = -std::numeric_limits<double>::infinity();
    constexpr float nanF = std::numeric_limits<float>::quiet_NaN();

    float toFloat (const std::optional<double>& v) noexcept { return v ? (float) *v : nanF; }
}

bool DynamicsEngine::supportsSampleRate (double sampleRate) noexcept
{
    const double rounded = std::round (sampleRate);
    return rounded >= 8000.0 && rounded <= 384000.0 && std::abs (sampleRate - rounded) < 1e-6
           && std::fmod (rounded, 100.0) == 0.0;
}

void DynamicsEngine::prepare (double sampleRate, int channelCount)
{
    if (channelCount == 1)
        prepare (sampleRate, ChannelLayout::mono());
    else if (channelCount == 2)
        prepare (sampleRate, ChannelLayout::stereo());
    else
        throw std::invalid_argument ("more than two channels need a channel layout");
}

void DynamicsEngine::prepare (double sampleRate, const ChannelLayout& channelLayout)
{
    if (! supportsSampleRate (sampleRate))
        throw std::invalid_argument ("sample rate must be a multiple of 100 Hz");
    if (! channelLayout.valid())
        throw std::invalid_argument ("invalid channel layout");

    fs = std::round (sampleRate);
    layout_ = channelLayout;
    measuredCount = 0;
    for (int ch = 0; ch < layout_.count; ++ch)
        if (layout_.weight (ch) > 0.0)
        {
            measuredIndex[(size_t) measuredCount] = ch;
            measuredWeight[(size_t) measuredCount] = layout_.weight (ch);
            ++measuredCount;
        }
    binSize = (int) fs / 10;
    subSize = (int) fs / 100;

    const auto coeffs = kWeightingCoefficients (fs);
    for (auto& f : kWeighting)
        f.setCoefficients (coeffs);
    truePeak.prepare (fs, layout_.count);
    delay = truePeak.delay();

    integratedHistogram.allocate();
    rangeHistogram.allocate();
    if (prepared)
        ++epoch; // a re-prepare is a new measurement for consumers
    prepared = true;
    resetRequested.store (false);
    resetState();
}

void DynamicsEngine::resetState() noexcept
{
    for (auto& f : kWeighting)
        f.reset();
    truePeak.reset();
    integratedHistogram.clear();
    rangeHistogram.clear();
    samplePos = subCount = 0;
    subPos = 0;
    subAccumulator = 0.0;
    subRing.fill (0.0);
    binMaxM = binMaxS = maxM = maxS = negInf;
    latched = {};
    peakAccumulator = peakMax = 0.0;
    peakRing.fill (0.0);
}

void DynamicsEngine::process (const float* const* input, int numFrames, MetricsSink& sink) noexcept
{
    std::array<double, maxChannels> x {};
    for (int i = 0; i < numFrames; ++i)
    {
        for (int ch = 0; ch < layout_.count; ++ch)
        {
            // A single NaN/inf would poison the K-filter state for good (spec 3.4: treated as 0)
            const double v = (double) input[ch][i];
            x[(size_t) ch] = std::isfinite (v) ? v : 0.0;
        }
        processFrame (x.data(), sink);
    }
}

void DynamicsEngine::processFrame (const double* x, MetricsSink& sink) noexcept
{
    // A requested reset takes effect at the start of a bin: the engine then behaves exactly
    // like a freshly prepared one started at this sample (spec ADA-RT-RESET).
    if (subPos == 0 && subCount % 10 == 0 && resetRequested.load (std::memory_order_relaxed)
        && resetRequested.exchange (false, std::memory_order_acq_rel))
    {
        resetState();
        ++epoch;
    }

    double energy = 0.0;
    for (int m = 0; m < measuredCount; ++m)
    {
        const double y = kWeighting[(size_t) m].process (x[measuredIndex[(size_t) m]]);
        energy += measuredWeight[(size_t) m] * y * y; // G_i of the channel role (spec 3.11)
    }
    subAccumulator += energy;
    if (++subPos == subSize)
        onSubBinComplete();

    advanceTruePeak (truePeak.processFrame (x), sink);
}

void DynamicsEngine::advanceTruePeak (double magnitude, MetricsSink& sink) noexcept
{
    // Output computed at input index samplePos represents input time tau = samplePos - D;
    // outputs with tau < 0 are clamped into bin 0 (spec 3.3).
    const std::int64_t tau = samplePos - delay;
    if (tau > 0 && tau % binSize == 0)
        closePeakBin (sink);
    peakAccumulator = std::max (peakAccumulator, magnitude);
    ++samplePos;
}

double DynamicsEngine::sumLastSubBins (int count) const noexcept
{
    // Sub-bins before the stream start count as zero energy (spec 3.4).
    const auto available = (int) std::min<std::int64_t> (count, subCount);
    double sum = 0.0;
    for (int i = 1; i <= available; ++i)
        sum += subRing[(size_t) ((subCount - i) % subBinsPerWindowS)];
    return sum;
}

void DynamicsEngine::onSubBinComplete() noexcept
{
    subRing[(size_t) (subCount % subBinsPerWindowS)] = subAccumulator;
    subAccumulator = 0.0;
    subPos = 0;
    ++subCount;

    // Sub-grid read-outs (spec 3.4): windows ending at every 10 ms; zero energy before start
    const double powerM = sumLastSubBins (subBinsPerWindowM) / (4.0 * binSize);
    const double powerS = sumLastSubBins (subBinsPerWindowS) / (30.0 * binSize);
    const double m = powerToLUFS (powerM), s = powerToLUFS (powerS);
    if (subCount >= subBinsPerWindowM)
    {
        binMaxM = std::max (binMaxM, m);
        maxM = std::max (maxM, m);
    }
    if (subCount >= subBinsPerWindowS)
    {
        binMaxS = std::max (binMaxS, s);
        maxS = std::max (maxS, s);
    }

    if (subCount % 10 != 0)
        return;

    // Bin n is complete: latch M and S at its end; the frame is emitted when P[n] closes.
    const std::int64_t n = subCount / 10 - 1;
    latched = { n, m, s, binMaxM, binMaxS };
    binMaxM = binMaxS = negInf;

    if (n >= 3 && m > -70.0)
        integratedHistogram.add (m, powerM);
    if (n >= 29 && s >= -70.0)
        rangeHistogram.add (s, powerS);
}

void DynamicsEngine::closePeakBin (MetricsSink& sink) noexcept
{
    const std::int64_t n = (samplePos - delay) / binSize - 1;
    const double peak = peakAccumulator;
    peakAccumulator = 0.0;
    peakRing[(size_t) (n % peakBins)] = peak;
    peakMax = std::max (peakMax, peak);
    if (latched.bin != n) // can only happen right after a reset; never emit a stale frame
        return;

    const double tp3s = linearToDB (*std::max_element (peakRing.begin(), peakRing.end()));
    const auto range = rangeHistogram.loudnessRange();

    DynamicsMetrics f {};
    f.binIndex = n;
    f.epoch = epoch;
    f.momentaryLUFS = (float) latched.m;
    f.shortTermLUFS = (float) latched.s;
    f.momentaryPeakLUFS = (float) latched.peakM;
    f.shortTermPeakLUFS = (float) latched.peakS;
    f.maxMomentaryLUFS = (float) maxM;
    f.maxShortTermLUFS = (float) maxS;
    f.binPeakDBTP = (float) linearToDB (peak);
    f.truePeak3sDBTP = (float) tp3s;
    f.truePeakMaxDBTP = (float) linearToDB (peakMax);
    f.psr = (n >= 29 && std::isfinite (latched.s) && std::isfinite (tp3s)) ? (float) (tp3s - latched.s) : nanF;
    f.integratedLUFS = toFloat (integratedHistogram.integrated());
    f.lra = toFloat (range.lra);
    f.lraGateLUFS = toFloat (range.relativeGate);
    f.isClipping = tp3s > (double) clipThreshold.load (std::memory_order_relaxed);
    sink.push (f);
}

DynamicsEngine::Final DynamicsEngine::finish (MetricsSink& sink) noexcept
{
    const std::int64_t frames = samplePos;
    const std::array<double, maxChannels> zeros {};
    for (int i = 0; i < delay; ++i)
        advanceTruePeak (truePeak.processFrame (zeros.data()), sink);

    // After the flush the open peak bin ends at input time frames - 1.
    if (frames > 0 && frames % binSize == 0)
        closePeakBin (sink); // the last bin is complete
    else
        peakMax = std::max (peakMax, peakAccumulator); // trailing partial bin: TP max only

    return { frames, linearToDB (peakMax) };
}

} // namespace ada
