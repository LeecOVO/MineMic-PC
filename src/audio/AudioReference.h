#pragma once
#include <QString>
#include <array>
#include <cstdint>
#include <memory>
namespace minemic {
struct ReferenceFrame {
    std::array<std::int16_t, 960> pcm{}; // 480 interleaved stereo frames.
    std::uint64_t position = 0, timeMicros = 0;
    std::uint32_t epoch = 0;
};
class AudioReference {
public:
    virtual ~AudioReference() = default;
    virtual bool supported() const = 0;
    // Accept an asynchronous start request; failures surface through takeError().
    // start/stop never wait for a device operation on the network thread.
    virtual bool start(const QString& device, QString& error) = 0;
    virtual void stop() = 0;
    virtual bool take(ReferenceFrame& frame) = 0;
    virtual QString takeError() = 0;
    // A bounded, non-sensitive description of capture progress, read off the audio worker.
    virtual QString status() { return {}; }
};
std::unique_ptr<AudioReference> createAudioReference();
std::uint64_t referenceClockMicros();
}
