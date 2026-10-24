#include "audio/AudioReference.h"
#include "audio/ReferenceTiming.h"
#include "audio/ReferencePcm.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>

namespace minemic {
using Microsoft::WRL::ComPtr;
namespace {
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct Handle { HANDLE value = nullptr; ~Handle() { if (value) CloseHandle(value); } };
QString failure(const char* action, HRESULT hr) {
    return QStringLiteral("%1 (0x%2)").arg(QString::fromLatin1(action)).arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}
}
class WasapiReference final : public AudioReference {
public:
    ~WasapiReference() override {
        { std::lock_guard<std::mutex> lock(mutex_); closing_ = true; ++generation_; }
        if (wake_) SetEvent(wake_);
        if (worker_.joinable()) worker_.join();
        if (wake_) CloseHandle(wake_);
    }
    bool supported() const override { return true; }
    bool start(const QString& device, QString& error) override {
        if (!wake_) wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake_) { error = QStringLiteral("Cannot create reference control event"); return false; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            desiredDevice_ = device; requested_ = true; ++generation_; read_ = write_ = count_ = 0; error_.clear();
            status_ = QStringLiteral("正在打开外放设备");
        }
        if (!worker_.joinable()) {
            try { worker_ = std::thread([this] { workerLoop(); }); }
            catch (const std::exception&) { stop(); error = QStringLiteral("Cannot start reference capture worker"); return false; }
        }
        SetEvent(wake_); return true;
    }
    void stop() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            desiredDevice_.clear(); requested_ = false; ++generation_; read_ = write_ = count_ = 0; error_.clear(); status_.clear();
        }
        if (wake_) SetEvent(wake_);
        // No COM initialization/stop or thread join on the audio network thread.
    }
    bool take(ReferenceFrame& frame) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!count_) return false;
        frame = queue_[read_]; read_ = (read_ + 1) % queue_.size(); --count_; return true;
    }
    QString takeError() override {
        std::lock_guard<std::mutex> lock(mutex_); auto error = error_; error_.clear(); return error;
    }
    QString status() override { std::lock_guard<std::mutex> lock(mutex_); return status_; }
private:
    void push(const ReferenceFrame& frame, std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || !requested_ || closing_) return;
        if (count_ == queue_.size()) { read_ = (read_ + 1) % queue_.size(); --count_; }
        queue_[write_] = frame; write_ = (write_ + 1) % queue_.size(); ++count_;
    }
    bool current(std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_); return !closing_ && generation_ == generation;
    }
    void report(std::uint64_t generation, const QString& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closing_ && generation_ == generation) error_ = error;
    }
    void reportStatus(std::uint64_t generation, const QString& status) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closing_ && requested_ && generation_ == generation) status_ = status;
    }
    void workerLoop() {
        for (;;) {
            QString device; std::uint64_t generation; bool requested;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closing_) return;
                device = desiredDevice_; generation = generation_; requested = requested_;
            }
            WaitForSingleObject(wake_, 0);
            if (!current(generation)) continue;
            if (requested && run(device, generation)) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation_ == generation) read_ = write_ = count_ = 0;
                continue;
            }
            if (current(generation)) WaitForSingleObject(wake_, INFINITE);
        }
    }
    bool run(const QString& id, std::uint64_t generation) {
        ComScope com;
        auto fail = [&](const char* action, HRESULT hr) { report(generation, failure(action, hr)); };
        if (FAILED(com.result)) { fail("Reference COM", com.result); return false; }
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { fail("Reference endpoints", hr); return false; }
        ComPtr<IMMDevice> device;
        // An empty selection follows Windows' current default playback endpoint.
        hr = id.isEmpty() ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
                          : enumerator->GetDevice(reinterpret_cast<LPCWSTR>(id.utf16()), &device);
        if (FAILED(hr)) { fail("Reference speaker", hr); return false; }
        ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) { fail("Reference audio client", hr); return false; }
        LPWSTR selectedId = nullptr;
        hr = device->GetId(&selectedId);
        if (FAILED(hr)) { fail("Reference endpoint identity", hr); return false; }
        const QString actualId = QString::fromWCharArray(selectedId); CoTaskMemFree(selectedId);
        QString deviceName = QStringLiteral("当前外放设备");
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT value; PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
                deviceName = QString::fromWCharArray(value.pwszVal);
            PropVariantClear(&value);
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2; format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16; format.nBlockAlign = 4; format.nAvgBytesPerSec = 192000;
        // Follow the endpoint mix format when it already runs at our wire rate.
        // This avoids inserting Windows format conversion into reference timing.
        WAVEFORMATEX* mix = nullptr;
        bool native = false, floating = false;
        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            const bool extensible = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22;
            const auto* extended = extensible ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix) : nullptr;
            floating = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (extended && IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
            const bool integer = mix->wFormatTag == WAVE_FORMAT_PCM || (extended && IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM));
            native = mix->nSamplesPerSec == 48000 && (mix->nChannels == 1 || mix->nChannels == 2) &&
                ((floating && mix->wBitsPerSample == 32) || (integer && (mix->wBitsPerSample == 16 || mix->wBitsPerSample == 24 || mix->wBitsPerSample == 32))) &&
                mix->nBlockAlign == mix->nChannels * mix->wBitsPerSample / 8;
        }
        const auto inputChannels = native ? mix->nChannels : format.nChannels;
        const auto inputBits = native ? mix->wBitsPerSample : format.wBitsPerSample;
        const auto inputStride = native ? mix->nBlockAlign : format.nBlockAlign;
        floating = native && floating;
        const DWORD conversion = native ? 0 : AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        // Other rates/layouts retain Windows' channel matrix and resampling.
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | conversion,
            0, 0, native ? mix : &format, nullptr);
        if (mix) CoTaskMemFree(mix);
        if (FAILED(hr)) { fail("Reference loopback format", hr); return false; }
        REFERENCE_TIME latency = 0;
        const auto latencyMs = SUCCEEDED(client->GetStreamLatency(&latency)) ? latency / 10000 : -1;
        Handle audio; audio.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!audio.value) { fail("Reference event", HRESULT_FROM_WIN32(GetLastError())); return false; }
        hr = client->SetEventHandle(audio.value);
        if (FAILED(hr)) { fail("Reference event handle", hr); return false; }
        ComPtr<IAudioCaptureClient> capture;
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) { fail("Reference capture", hr); return false; }
        hr = client->Start();
        if (FAILED(hr)) { fail("Reference start", hr); return false; }
        if (!current(generation)) { client->Stop(); return false; }
        ReferenceFrame pending; std::size_t filled = 0; std::uint32_t epoch = ++nextEpoch_;
        ReferenceTiming timing;
        std::uint64_t expectedTime = 0, outputPosition = 0; bool havePosition = false;
        HANDLE events[] = {wake_, audio.value};
        bool failed = false, changedDefault = false;
        auto nextDefaultCheck = referenceClockMicros() + 1000000;
        auto nextStatus = referenceClockMicros();
        std::uint64_t packets = 0, invalidTiming = 0, oversized = 0;
        std::uint64_t lastPacketAt = 0;
        bool estimated = false;
        std::int64_t rawLead = 0;
        reportStatus(generation, QStringLiteral("%1 · 已打开，等待播放数据").arg(deviceName));
        while (!failed && current(generation)) {
            const auto event = WaitForMultipleObjects(2, events, FALSE, 50);
            if (event == WAIT_OBJECT_0) { if (!current(generation)) break; else continue; }
            if (event == WAIT_FAILED) { hr = HRESULT_FROM_WIN32(GetLastError()); failed = true; break; }
            if (referenceClockMicros() >= nextStatus) {
                const auto state = !packets ? QStringLiteral("尚未采集到播放数据") :
                    referenceClockMicros() - lastPacketAt > 1000000 ? QStringLiteral("当前没有新的播放数据（已采集 %1 包）").arg(packets) :
                    !estimated && invalidTiming + oversized == packets ? QStringLiteral("采集数据或参考时间无效（时间异常 %1 / 数据过大 %2）").arg(invalidTiming).arg(oversized) :
                    QStringLiteral("已采集 %1 包，原始时间异常 %2 包").arg(packets).arg(invalidTiming);
                const auto clock = estimated ? QStringLiteral("时间估计，原始领先 %1 ms").arg(rawLead / 1000) : QStringLiteral("设备时间");
                reportStatus(generation, QStringLiteral("%1 · %2；%3；%4 / 流延迟 %5 ms")
                    .arg(deviceName, state, clock, native ? QStringLiteral("原生格式") : QStringLiteral("系统格式转换")).arg(latencyMs));
                nextStatus = referenceClockMicros() + 1000000;
            }
            if (id.isEmpty() && referenceClockMicros() >= nextDefaultCheck) {
                nextDefaultCheck = referenceClockMicros() + 1000000;
                ComPtr<IMMDevice> currentDefault;
                hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &currentDefault);
                if (FAILED(hr)) { failed = true; break; }
                LPWSTR currentId = nullptr;
                hr = currentDefault->GetId(&currentId);
                if (FAILED(hr)) { failed = true; break; }
                changedDefault = actualId != QString::fromWCharArray(currentId); CoTaskMemFree(currentId);
                if (changedDefault) break;
            }
            for (int batch = 0; batch < 32; ++batch) {
                UINT32 available = 0; hr = capture->GetNextPacketSize(&available);
                if (FAILED(hr)) { failed = true; break; }
                if (!available) break;
                BYTE* bytes = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 position = 0, qpc = 0;
                hr = capture->GetBuffer(&bytes, &frames, &flags, &position, &qpc);
                if (FAILED(hr)) { failed = true; break; }
                if (!frames) break;
                ++packets;
                lastPacketAt = referenceClockMicros();
                if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || !qpc) ++invalidTiming;
                else if (frames > 8192) ++oversized;
                rawLead = static_cast<std::int64_t>(qpc / 10) - static_cast<std::int64_t>(lastPacketAt);
                const auto stamp = timing.observe(position, frames, 48000, qpc / 10, lastPacketAt,
                    (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0, (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0);
                const auto time = stamp.time; estimated = stamp.estimated;
                const auto difference = time > expectedTime ? time - expectedTime : expectedTime - time;
                if (stamp.reset || (havePosition && difference > 3000)) { filled = 0; epoch = ++nextEpoch_; }
                expectedTime = time + std::uint64_t(frames) * 1000000 / 48000; havePosition = true;
                if (time && frames <= 8192) {
                    for (UINT32 i = 0; i < frames; ++i) {
                        if (!filled) { pending.position = outputPosition + i; pending.timeMicros = time + std::uint64_t(i) * 1000000 / 48000; pending.epoch = epoch; }
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) { pending.pcm[2*filled] = pending.pcm[2*filled+1] = 0; }
                        else if (bytes) {
                            for (int channel = 0; channel < 2; ++channel) {
                                const auto* sample = bytes + inputStride*i + (inputChannels == 1 ? 0 : channel) * inputBits/8;
                                pending.pcm[2*filled + channel] = referenceSample(sample, inputBits, floating);
                            }
                        }
                        else { filled = 0; epoch = ++nextEpoch_; break; }
                        if (++filled == 480) { push(pending, generation); filled = 0; }
                    }
                } else { filled = 0; epoch = ++nextEpoch_; }
                outputPosition += frames;
                hr = capture->ReleaseBuffer(frames);
                if (FAILED(hr)) { failed = true; break; }
            }
        }
        client->Stop();
        if (failed) report(generation, failure("Reference capture stopped", hr));
        return changedDefault && current(generation);
    }
    HANDLE wake_ = nullptr;
    QString desiredDevice_;
    std::uint64_t generation_ = 0;
    bool closing_ = false, requested_ = false;
    std::uint32_t nextEpoch_ = 0;
    std::thread worker_;
    std::mutex mutex_;
    // Covers the receiver's 200 ms scheduling horizon plus bounded delivery jitter.
    std::array<ReferenceFrame, 32> queue_{};
    std::size_t read_ = 0, write_ = 0, count_ = 0;
    QString error_, status_;
};
std::unique_ptr<AudioReference> createAudioReference() { return std::make_unique<WasapiReference>(); }
}
