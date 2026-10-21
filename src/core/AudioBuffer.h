#pragma once
#include "core/Packet.h"
#include <atomic>
#include <mutex>

namespace minemic {
struct BufferStats {
    std::uint64_t accepted = 0, late = 0, duplicates = 0, missing = 0, resyncs = 0;
    std::size_t queuedFrames = 0;
};

// Fixed 160 ms capacity. Never allocates in the device callback; short bounded lock.
// Four packets of pre-roll. Sequence gaps become silence; overflow drops stale audio.
class AudioBuffer {
public:
    void reset();
    bool push(std::uint32_t sequence, const std::array<std::int16_t, FramesPerPacket>& samples);
    void read(std::int16_t* destination, std::size_t frames);
    BufferStats stats() const;
    void setGain(float gain);
    void setMuted(bool muted) { muted_.store(muted); }
    int takePeak() { return peak_.exchange(0); }
private:
    // A power of two preserves the slot mapping across uint32 sequence wrap.
    static constexpr std::size_t Capacity = 16, PreRoll = 4;
    struct Slot {
        std::array<std::int16_t, FramesPerPacket> samples{};
        std::uint32_t sequence = 0;
        bool valid = false;
    };
    mutable std::mutex mutex_;
    std::array<Slot, Capacity> slots_{};
    std::array<std::int16_t, FramesPerPacket> current_{};
    std::uint32_t expected_ = 0;
    std::size_t offset_ = FramesPerPacket;
    bool initialized_ = false, playing_ = false;
    BufferStats stats_{};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<int> peak_{0};
};
}
