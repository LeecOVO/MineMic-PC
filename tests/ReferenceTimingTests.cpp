#include "audio/ReferenceTiming.h"
#include "audio/ReferencePcm.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void check(bool pass, const char* message) {
    if (!pass) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}
int main() {
    using namespace minemic;
    ReferenceTiming hardware;
    check(hardware.observe(0, 480, 48000, 990000, 1000000, false, false).time == 990000, "valid device timestamp retained");
    const auto stale = hardware.observe(480, 480, 48000, 1000000, 1400000, false, false);
    check(stale.time == 1000000 && !stale.estimated, "late capture is not relabelled as fresh");

    ReferenceTiming future;
    std::uint64_t previous = 0;
    for (unsigned i = 0; i < 30000; ++i) {
        // Five minutes, 100 ppm fast source, occasional 15 ms callback jitter.
        const auto now = 1000000 + std::uint64_t(i) * 9999 + (i % 43 == 20 ? 15000 : 0);
        const auto stamp = future.observe(std::uint64_t(i)*480, 480, 48000, now+250000, now, false, false);
        check(stamp.estimated && !stamp.reset, "unflagged future timestamps switch to estimated clock without recurring resets");
        if (i < 2) { check(stamp.time == 0, "estimated position clock requires contiguous warmup"); continue; }
        check(stamp.time <= now && now-stamp.time < 30000, "estimated clock remains bounded under drift and callback jitter");
        if (previous) check(stamp.time-previous >= 9990 && stamp.time-previous <= 10010, "sample position preserves smooth frame spacing");
        previous = stamp.time;
    }
    auto gap = future.observe(0, 480, 48000, 0, 400000000, true, true);
    check(gap.reset && gap.time == 0, "device reset discards old timing history");
    check(future.observe(480, 480, 48000, 0, 400010000, true, false).time == 0, "invalid QPC warmup remains bounded");
    check(future.observe(960, 480, 48000, 0, 400020000, true, false).time == 400010000, "flagged bad QPC can recover through contiguous position estimates");

    const unsigned char negative24[]{0, 0, 128}, positive24[]{255, 255, 127};
    check(referenceSample(negative24, 24, false) == -32768 && referenceSample(positive24, 24, false) == 32767, "native signed PCM24 conversion");
    const float floatSamples[]{-2.f, -.5f, .5f, 2.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
    const int expected[]{-32768, -16384, 16384, 32767, 0, 0};
    for (unsigned i = 0; i < 6; ++i)
        check(referenceSample(reinterpret_cast<const unsigned char*>(&floatSamples[i]), 32, true) == expected[i], "native float conversion clips and rejects nonfinite samples");
    std::cout << "Reference timing and PCM tests passed\n";
}
