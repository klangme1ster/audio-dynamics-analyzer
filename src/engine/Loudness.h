#pragma once

#include <cmath>
#include <limits>

namespace ada
{

inline double powerToLUFS (double power) noexcept
{
    return power > 0.0 ? -0.691 + 10.0 * std::log10 (power) : -std::numeric_limits<double>::infinity();
}

inline double linearToDB (double linear) noexcept
{
    return linear > 0.0 ? 20.0 * std::log10 (linear) : -std::numeric_limits<double>::infinity();
}

} // namespace ada
