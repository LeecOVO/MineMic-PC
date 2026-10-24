#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace minemic {
// WASAPI timestamps are not reliable on every endpoint, even without
// TIMESTAMP_ERROR. Like Chromium/OBS, fall back to system-time estimates, but
// advance by sample position so a delayed callback cannot compress the stream.
// This is an estimate, not a measurement of the speaker's acoustic latency.
class ReferenceTiming {
public:
    struct Result { std::uint64_t time = 0; bool estimated = false, reset = false; };
    Result observe(std::uint64_t position, std::uint32_t frames, std::uint32_t rate,
                   std::uint64_t qpcMicros, std::uint64_t receivedAt,
                   bool timestampError, bool discontinuity) {
        if (!frames || !rate) return {};
        const bool gap = havePosition_ && position != expectedPosition_;
        const bool reset = discontinuity || gap;
        if (reset) { anchored_ = false; observations_ = 0; }
        expectedPosition_ = position + frames; havePosition_ = true;
        // Old but otherwise valid timestamps are left intact: the sender must
        // discard stale audio rather than relabel it as fresh after a stall.
        const bool future = qpcMicros > receivedAt && qpcMicros - receivedAt > 20000;
        if (!estimated_ && !timestampError && qpcMicros && !future)
            return {qpcMicros, false, reset};
        estimated_ = true; // Remain in one clock domain until capture restarts.
        const auto duration = std::uint64_t(frames) * 1000000 / rate;
        if (receivedAt <= duration) return {};
        if (!anchored_) {
            originPosition_ = position;
            originTime_ = receivedAt - duration;
            anchored_ = true; observations_ = 0;
        }
        const auto relative = static_cast<std::int64_t>((position - originPosition_) * 1000000 / rate);
        const auto observation = static_cast<std::int64_t>(receivedAt - duration) - relative;
        arrivals_[observations_++ % arrivals_.size()] = observation;
        // Earliest arrival rejects positive scheduling jitter. Follow its slow
        // movement rather than repeatedly anchoring each packet to callback time.
        const auto count = std::min<std::size_t>(observations_, arrivals_.size());
        const auto earliest = *std::min_element(arrivals_.begin(), arrivals_.begin() + count);
        const auto correction = std::clamp(earliest - static_cast<std::int64_t>(originTime_), std::int64_t(-10), std::int64_t(10));
        originTime_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(originTime_) + correction);
        const auto time = originTime_ + static_cast<std::uint64_t>(relative);
        return {observations_ >= 3 ? time : 0, true, reset};
    }
private:
    bool havePosition_ = false, estimated_ = false, anchored_ = false;
    std::uint64_t expectedPosition_ = 0, originPosition_ = 0, originTime_ = 0;
    std::array<std::int64_t, 64> arrivals_{};
    std::size_t observations_ = 0;
};
}
