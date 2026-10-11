#include "KWeighting.h"

#include "GeneratedTables.h"

#include <cmath>
#include <numbers>

namespace ada
{

std::array<Biquad, 2> kWeightingCoefficients (double fs)
{
    using namespace tables;
    std::array<Biquad, 2> c {};

    double k = std::tan (std::numbers::pi * shelfF0 / fs);
    const double vh = std::pow (10.0, shelfGainDB / 20.0);
    const double vb = std::pow (vh, shelfVbExponent);
    double a0 = 1.0 + k / shelfQ + k * k;
    c[0] = { (vh + vb * k / shelfQ + k * k) / a0,
             2.0 * (k * k - vh) / a0,
             (vh - vb * k / shelfQ + k * k) / a0,
             2.0 * (k * k - 1.0) / a0,
             (1.0 - k / shelfQ + k * k) / a0 };

    k = std::tan (std::numbers::pi * highpassF0 / fs);
    a0 = 1.0 + k / highpassQ + k * k;
    c[1] = { 1.0, -2.0, 1.0,
             2.0 * (k * k - 1.0) / a0,
             (1.0 - k / highpassQ + k * k) / a0 };
    return c;
}

} // namespace ada
