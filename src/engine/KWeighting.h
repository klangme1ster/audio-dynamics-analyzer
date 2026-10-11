#pragma once

#include <array>

namespace ada
{

struct Biquad
{
    double b0, b1, b2, a1, a2;
};

// BS.1770 K-weighting coefficients for any sample rate (spec 3.2), [0] shelf, [1] RLB high-pass
std::array<Biquad, 2> kWeightingCoefficients (double sampleRate);

// Two cascaded biquads in transposed direct form II (same structure as scipy sosfilt),
// one instance per channel. State starts at zero.
class KWeightingFilter
{
public:
    void setCoefficients (const std::array<Biquad, 2>& c) noexcept { coeffs = c; }
    void reset() noexcept { state = {}; }

    double process (double x) noexcept
    {
        for (int s = 0; s < 2; ++s)
        {
            const auto& c = coeffs[(size_t) s];
            auto& z = state[(size_t) s];
            const double y = c.b0 * x + z[0];
            z[0] = c.b1 * x - c.a1 * y + z[1];
            z[1] = c.b2 * x - c.a2 * y;
            x = y;
        }
        return x;
    }

private:
    std::array<Biquad, 2> coeffs {};
    std::array<std::array<double, 2>, 2> state {};
};

} // namespace ada
