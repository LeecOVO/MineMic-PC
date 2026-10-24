#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace minemic {
inline std::int16_t referenceSample(const unsigned char* sample, unsigned bits, bool floating) {
    double value = 0;
    if (floating && bits == 32) {
        float decoded; std::memcpy(&decoded, sample, 4);
        value = std::isfinite(decoded) ? decoded * 32768.0 : 0;
    } else if (bits == 16) {
        std::int16_t decoded; std::memcpy(&decoded, sample, 2); value = decoded;
    } else if (bits == 24) {
        const auto decoded = static_cast<std::int32_t>((std::uint32_t(sample[2]) << 24) | (std::uint32_t(sample[1]) << 16) | (std::uint32_t(sample[0]) << 8));
        value = decoded / 65536.0;
    } else if (bits == 32) {
        std::int32_t decoded; std::memcpy(&decoded, sample, 4); value = decoded / 65536.0;
    }
    return static_cast<std::int16_t>(std::lround(std::clamp(value, -32768.0, 32767.0)));
}
}
