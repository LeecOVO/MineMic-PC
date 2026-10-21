#include "core/AudioBuffer.h"
#include <algorithm>
#include <cmath>

namespace minemic {
void AudioBuffer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) slot.valid = false;
    current_.fill(0); expected_ = 0; offset_ = FramesPerPacket;
    initialized_ = playing_ = false; stats_ = {}; peak_ = 0;
}

bool AudioBuffer::push(std::uint32_t sequence, const std::array<std::int16_t, FramesPerPacket>& samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) { expected_ = sequence; initialized_ = true; }
    auto distance = sequenceDistance(sequence, expected_);
    if (distance < 0) { ++stats_.late; return false; }
    if (distance >= static_cast<std::int64_t>(Capacity)) {
        for (auto& slot : slots_) slot.valid = false;
        expected_ = sequence; offset_ = FramesPerPacket; playing_ = false;
        ++stats_.resyncs;
    }
    auto& slot = slots_[sequence % Capacity];
    if (slot.valid && slot.sequence == sequence) { ++stats_.duplicates; return false; }
    slot.sequence = sequence; slot.samples = samples; slot.valid = true;
    ++stats_.accepted;
    return true;
}

void AudioBuffer::read(std::int16_t* dest, std::size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!playing_) {
        std::size_t count = 0;
        for (const auto& slot : slots_) if (slot.valid) ++count;
        if (count < PreRoll) { std::fill_n(dest, frames, 0); return; }
        playing_ = true;
    }
    const float gain = muted_.load() ? 0.0f : gain_.load();
    int peak = 0;
    while (frames) {
        if (offset_ == FramesPerPacket) {
            auto& slot = slots_[expected_ % Capacity];
            if (slot.valid && slot.sequence == expected_) {
                current_ = slot.samples; slot.valid = false;
            } else {
                current_.fill(0); ++stats_.missing;
                bool pending = false;
                for (const auto& candidate : slots_) pending |= candidate.valid;
                if (!pending) {
                    // Re-prime after starvation instead of advancing the playback clock
                    // indefinitely and rejecting every packet from a slower sender.
                    playing_ = initialized_ = false;
                    std::fill_n(dest, frames, 0);
                    break;
                }
            }
            ++expected_; offset_ = 0;
        }
        const auto count = std::min(frames, FramesPerPacket - offset_);
        for (std::size_t i = 0; i < count; ++i) {
            const int sample = std::clamp(static_cast<int>(std::lround(current_[offset_ + i] * gain)), -32768, 32767);
            dest[i] = static_cast<std::int16_t>(sample);
            peak = std::max(peak, std::abs(sample));
        }
        offset_ += count; dest += count; frames -= count;
    }
    int previous = peak_.load();
    while (previous < peak && !peak_.compare_exchange_weak(previous, peak)) {}
}

BufferStats AudioBuffer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = stats_;
    for (const auto& slot : slots_) if (slot.valid) result.queuedFrames += FramesPerPacket;
    if (offset_ < FramesPerPacket) result.queuedFrames += FramesPerPacket - offset_;
    return result;
}
void AudioBuffer::setGain(float gain) { gain_.store(std::clamp(gain, 0.0f, 2.0f)); }
}
