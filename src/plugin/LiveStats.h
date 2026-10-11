#pragma once

#include "DynamicsEngine.h"
#include "MetricsFifo.h"

#include <atomic>
#include <cmath>
#include <limits>

namespace ada
{

// Live PSR statistics gate (spec 3.7) using the running LRA relative gate.
inline bool passesLivePsrGate (const DynamicsMetrics& f) noexcept
{
    return f.binIndex >= 29 && f.shortTermLUFS >= -70.0f && f.shortTermLUFS >= f.lraGateLUFS && std::isfinite (f.psr);
}

// Audio-thread sink: keeps statistics that must survive the editor being closed, then forwards
// every frame to the UI FIFO. Allocation- and lock-free.
class LiveStatsSink : public MetricsSink
{
public:
    explicit LiveStatsSink (MetricsFifo& f) noexcept : fifo (f) {}

    void push (const DynamicsMetrics& f) noexcept override
    {
        if (f.epoch != epoch)
        {
            epoch = f.epoch;
            psrMin.store (nan, std::memory_order_relaxed);
        }
        if (passesLivePsrGate (f))
        {
            const float current = psrMin.load (std::memory_order_relaxed);
            if (std::isnan (current) || f.psr < current)
                psrMin.store (f.psr, std::memory_order_relaxed);
        }
        fifo.push (f);
    }

    float psrMinimum() const noexcept { return psrMin.load (std::memory_order_relaxed); }

private:
    static constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    MetricsFifo& fifo;
    std::uint32_t epoch = std::numeric_limits<std::uint32_t>::max();
    std::atomic<float> psrMin { nan };
};

} // namespace ada
