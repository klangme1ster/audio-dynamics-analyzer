#pragma once

#include "GeneratedTables.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ada
{

// BS.1770 Annex 2 polyphase interpolator (spec 3.3). Per input frame it returns the largest
// interpolated magnitude over channels and active phases. The caller attributes that value to
// input time k - delay(). No allocation; fixed-size state for up to two channels.
class TruePeakDetector
{
public:
    static constexpr int maxChannels = 2;

    static int oversamplingFactor (double sampleRate) noexcept
    {
        return sampleRate <= 48000.0 ? 4 : (sampleRate <= 96000.0 ? 2 : 1);
    }

    void prepare (double sampleRate, int channels) noexcept
    {
        numChannels = std::clamp (channels, 1, maxChannels);
        factor = oversamplingFactor (sampleRate);
        numPhases = factor == 4 ? 4 : (factor == 2 ? 2 : 0);
        phases = factor == 4 ? std::array<int, 4> { 0, 1, 2, 3 } : std::array<int, 4> { 0, 2, 0, 0 };
        reset();
    }

    void reset() noexcept { history = {}; }

    int oversampling() const noexcept { return factor; }
    int delay() const noexcept { return numPhases > 0 ? tables::interpolatorDelay : 0; }

    double processFrame (const double* x) noexcept
    {
        double peak = 0.0;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& h = history[(size_t) ch];
            std::copy_backward (h.begin(), h.end() - 1, h.end()); // h[j] = x[k - j]
            h[0] = x[ch];

            if (numPhases == 0)
            {
                peak = std::max (peak, std::abs (h[0]));
                continue;
            }
            for (int i = 0; i < numPhases; ++i)
            {
                const int p = phases[(size_t) i];
                double y = 0.0;
                for (int j = 0; j < tables::annex2Rows; ++j)
                    y += tables::annex2[j][p] * h[(size_t) j];
                peak = std::max (peak, std::abs (y));
            }
        }
        return peak;
    }

private:
    int numChannels = 2, factor = 4, numPhases = 4;
    std::array<int, 4> phases { 0, 1, 2, 3 };
    std::array<std::array<double, tables::annex2Rows>, maxChannels> history {};
};

} // namespace ada
