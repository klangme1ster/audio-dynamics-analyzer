#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace ada
{

enum class SampleFormat { float32, int16, int24, int32 };

// Converts interleaved device samples (e.g. a WASAPI loopback packet) to planar float for the
// first `outChannels` channels (front left / right of a surround device). Allocation-free.
inline void interleavedToPlanar (const std::uint8_t* src, SampleFormat format, int srcChannels, int frames,
                                 float* const* dst, int outChannels) noexcept
{
    const int bytes = format == SampleFormat::int16 ? 2 : format == SampleFormat::int24 ? 3 : 4;
    const int channels = std::min (outChannels, srcChannels);
    for (int i = 0; i < frames; ++i)
    {
        const std::uint8_t* frame = src + (std::size_t) i * (std::size_t) srcChannels * (std::size_t) bytes;
        for (int ch = 0; ch < channels; ++ch)
        {
            const std::uint8_t* s = frame + (std::size_t) ch * (std::size_t) bytes;
            float v = 0.0f;
            switch (format)
            {
                case SampleFormat::float32: std::memcpy (&v, s, 4); break;
                case SampleFormat::int16:
                {
                    std::int16_t x;
                    std::memcpy (&x, s, 2);
                    v = (float) x / 32768.0f;
                    break;
                }
                case SampleFormat::int24:
                {
                    const std::int32_t x = (std::int32_t) ((std::uint32_t) s[0] << 8 | (std::uint32_t) s[1] << 16
                                                           | (std::uint32_t) s[2] << 24) >> 8;
                    v = (float) x / 8388608.0f;
                    break;
                }
                case SampleFormat::int32:
                {
                    std::int32_t x;
                    std::memcpy (&x, s, 4);
                    v = (float) ((double) x / 2147483648.0);
                    break;
                }
            }
            dst[ch][i] = v;
        }
    }
}

} // namespace ada
