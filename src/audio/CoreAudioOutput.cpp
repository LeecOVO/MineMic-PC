#include "audio/AudioOutput.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <vector>

namespace minemic {
namespace {
QString stringValue(CFStringRef text) {
    if (!text) return {};
    const auto size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::vector<char> bytes(static_cast<std::size_t>(size));
    return CFStringGetCString(text, bytes.data(), size, kCFStringEncodingUTF8) ? QString::fromUtf8(bytes.data()) : QString();
}
CFStringRef deviceString(AudioDeviceID id, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef value = nullptr; UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &value) == noErr ? value : nullptr;
}
}

class CoreAudioOutput final : public AudioOutput {
public:
    ~CoreAudioOutput() override { stop(); }
    QVector<OutputDevice> devices(QString& error) override {
        error.clear();
        AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 size = 0;
        OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size);
        if (status != noErr) { error = QStringLiteral("Core Audio enumeration failed: %1").arg(status); return {}; }
        std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
        status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, ids.data());
        if (status != noErr) { error = QStringLiteral("Core Audio device list failed: %1").arg(status); return {}; }
        QVector<OutputDevice> result;
        for (auto id : ids) {
            AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain};
            UInt32 streamSize = 0;
            if (AudioObjectGetPropertyDataSize(id, &streams, 0, nullptr, &streamSize) != noErr || streamSize == 0) continue;
            CFStringRef uid = deviceString(id, kAudioDevicePropertyDeviceUID);
            CFStringRef name = deviceString(id, kAudioObjectPropertyName);
            if (uid) { result.push_back({stringValue(uid), stringValue(name)}); CFRelease(uid); }
            if (name) CFRelease(name);
        }
        return result;
    }

    bool start(const QString& id, AudioBuffer& buffer, QString& error) override {
        stop(); buffer_ = &buffer; lastError_ = noErr;
        AudioStreamBasicDescription format{};
        format.mSampleRate = SampleRate; format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        format.mBytesPerPacket = 2; format.mFramesPerPacket = 1;
        format.mBytesPerFrame = 2; format.mChannelsPerFrame = 1; format.mBitsPerChannel = 16;
        OSStatus status = AudioQueueNewOutput(&format, &CoreAudioOutput::callback, this, nullptr, nullptr, 0, &queue_);
        if (status == noErr) {
            const auto utf8 = id.toUtf8();
            CFStringRef uid = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(utf8.constData()), utf8.size(), kCFStringEncodingUTF8, false);
            status = AudioQueueSetProperty(queue_, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
            if (uid) CFRelease(uid);
        }
        if (status == noErr) {
            for (int i = 0; i < 3 && status == noErr; ++i) {
                AudioQueueBufferRef audio = nullptr;
                status = AudioQueueAllocateBuffer(queue_, FramesPerPacket * 2, &audio);
                if (status == noErr) { fill(audio); status = AudioQueueEnqueueBuffer(queue_, audio, 0, nullptr); }
            }
        }
        if (status == noErr) status = AudioQueueStart(queue_, nullptr);
        if (status != noErr) { error = QStringLiteral("Core Audio output failed: %1").arg(status); stop(); return false; }
        error.clear(); return true;
    }
    void stop() override {
        if (queue_) { AudioQueueStop(queue_, true); AudioQueueDispose(queue_, true); queue_ = nullptr; }
        buffer_ = nullptr;
    }
    QString takeError() override {
        const auto status = lastError_.exchange(noErr);
        return status == noErr ? QString() : QStringLiteral("Core Audio output stopped: %1").arg(status);
    }
private:
    void fill(AudioQueueBufferRef audio) {
        const auto frames = audio->mAudioDataBytesCapacity / 2;
        buffer_->read(static_cast<std::int16_t*>(audio->mAudioData), frames);
        audio->mAudioDataByteSize = frames * 2;
    }
    static void callback(void* context, AudioQueueRef queue, AudioQueueBufferRef audio) {
        auto& self = *static_cast<CoreAudioOutput*>(context);
        self.fill(audio);
        const auto status = AudioQueueEnqueueBuffer(queue, audio, 0, nullptr);
        if (status != noErr) self.lastError_.store(status);
    }
    AudioQueueRef queue_ = nullptr;
    AudioBuffer* buffer_ = nullptr;
    std::atomic<OSStatus> lastError_{noErr};
};
std::unique_ptr<AudioOutput> createAudioOutput() { return std::make_unique<CoreAudioOutput>(); }
}
