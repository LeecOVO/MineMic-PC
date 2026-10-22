#include "audio/AudioReference.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
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
        if (device.isEmpty()) { error = QStringLiteral("Select the actual playback speaker first"); return false; }
        if (!wake_) wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake_) { error = QStringLiteral("Cannot create reference control event"); return false; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            desiredDevice_ = device; ++generation_; read_ = write_ = count_ = 0; error_.clear();
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
            desiredDevice_.clear(); ++generation_; read_ = write_ = count_ = 0; error_.clear();
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
private:
    void push(const ReferenceFrame& frame, std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || desiredDevice_.isEmpty() || closing_) return;
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
    void workerLoop() {
        for (;;) {
            QString device; std::uint64_t generation;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closing_) return;
                device = desiredDevice_; generation = generation_;
            }
            WaitForSingleObject(wake_, 0);
            if (!current(generation)) continue;
            if (!device.isEmpty()) run(device, generation);
            if (current(generation)) WaitForSingleObject(wake_, INFINITE);
        }
    }
    void run(const QString& id, std::uint64_t generation) {
        ComScope com;
        auto fail = [&](const char* action, HRESULT hr) { report(generation, failure(action, hr)); };
        if (FAILED(com.result)) { fail("Reference COM", com.result); return; }
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { fail("Reference endpoints", hr); return; }
        ComPtr<IMMDevice> device;
        // No implicit virtual cable or endpoint substitution: use the user's physical speaker selection.
        if (id.isEmpty()) { fail("Reference speaker not selected", E_INVALIDARG); return; }
        hr = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(id.utf16()), &device);
        if (FAILED(hr)) { fail("Reference speaker", hr); return; }
        ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) { fail("Reference audio client", hr); return; }
        WAVEFORMATEX* mix = nullptr;
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr)) { fail("Reference mix format", hr); return; }
        const auto channels = mix->nChannels; CoTaskMemFree(mix);
        if (channels > 2) { fail("Reference supports mono/stereo speakers", AUDCLNT_E_UNSUPPORTED_FORMAT); return; }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2; format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16; format.nBlockAlign = 4; format.nAvgBytesPerSec = 192000;
        // Windows performs high-quality shared-mode format conversion, rather than a custom resampler.
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            0, 0, &format, nullptr);
        if (FAILED(hr)) { fail("Reference loopback format", hr); return; }
        Handle audio; audio.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!audio.value) { fail("Reference event", HRESULT_FROM_WIN32(GetLastError())); return; }
        hr = client->SetEventHandle(audio.value);
        if (FAILED(hr)) { fail("Reference event handle", hr); return; }
        ComPtr<IAudioCaptureClient> capture;
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) { fail("Reference capture", hr); return; }
        hr = client->Start();
        if (FAILED(hr)) { fail("Reference start", hr); return; }
        if (!current(generation)) { client->Stop(); return; }
        ReferenceFrame pending; std::size_t filled = 0; std::uint32_t epoch = 1;
        std::uint64_t expectedTime = 0, outputPosition = 0; bool havePosition = false;
        HANDLE events[] = {wake_, audio.value};
        bool failed = false;
        while (!failed && current(generation)) {
            const auto event = WaitForMultipleObjects(2, events, FALSE, 50);
            if (event == WAIT_OBJECT_0) { if (!current(generation)) break; else continue; }
            if (event == WAIT_FAILED) { hr = HRESULT_FROM_WIN32(GetLastError()); failed = true; break; }
            for (int batch = 0; batch < 32; ++batch) {
                UINT32 available = 0; hr = capture->GetNextPacketSize(&available);
                if (FAILED(hr)) { failed = true; break; }
                if (!available) break;
                BYTE* bytes = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 position = 0, qpc = 0;
                hr = capture->GetBuffer(&bytes, &frames, &flags, &position, &qpc);
                if (FAILED(hr)) { failed = true; break; }
                if (!frames) break;
                const auto time = qpc / 10;
                const auto difference = time > expectedTime ? time - expectedTime : expectedTime - time;
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) || (havePosition && difference > 3000)) { filled = 0; ++epoch; }
                expectedTime = time + std::uint64_t(frames) * 1000000 / 48000; havePosition = true;
                // Uncertain hardware timing is not a valid AEC reference.
                if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && frames <= 8192) {
                    for (UINT32 i = 0; i < frames; ++i) {
                        if (!filled) { pending.position = outputPosition + i; pending.timeMicros = time + std::uint64_t(i) * 1000000 / 48000; pending.epoch = epoch; }
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) { pending.pcm[2*filled] = pending.pcm[2*filled+1] = 0; }
                        else if (bytes) std::memcpy(pending.pcm.data() + 2*filled, bytes + 4*i, 4);
                        else { filled = 0; ++epoch; break; }
                        if (++filled == 480) { push(pending, generation); filled = 0; }
                    }
                } else { filled = 0; ++epoch; }
                outputPosition += frames;
                hr = capture->ReleaseBuffer(frames);
                if (FAILED(hr)) { failed = true; break; }
            }
        }
        client->Stop();
        if (failed) report(generation, failure("Reference capture stopped", hr));
    }
    HANDLE wake_ = nullptr;
    QString desiredDevice_;
    std::uint64_t generation_ = 0;
    bool closing_ = false;
    std::thread worker_;
    std::mutex mutex_;
    std::array<ReferenceFrame, 16> queue_{};
    std::size_t read_ = 0, write_ = 0, count_ = 0;
    QString error_;
};
std::unique_ptr<AudioReference> createAudioReference() { return std::make_unique<WasapiReference>(); }
}
