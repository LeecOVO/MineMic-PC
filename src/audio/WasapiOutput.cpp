#include "audio/AudioOutput.h"
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace minemic {
using Microsoft::WRL::ComPtr;
namespace {
QString hresultMessage(const char* action, HRESULT hr) {
    return QStringLiteral("%1 (0x%2)").arg(QString::fromLatin1(action)).arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
}

class WasapiOutput final : public AudioOutput {
public:
    ~WasapiOutput() override { stop(); }
    QVector<OutputDevice> devices(QString& error) override {
        error.clear();
        ComScope com;
        if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) { error = hresultMessage("COM", com.result); return {}; }
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { error = hresultMessage("Audio device enumeration", hr); return {}; }
        ComPtr<IMMDeviceCollection> collection;
        hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr)) { error = hresultMessage("Audio endpoints", hr); return {}; }
        UINT count = 0; collection->GetCount(&count);
        QVector<OutputDevice> result;
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, &device))) continue;
            LPWSTR id = nullptr;
            if (FAILED(device->GetId(&id))) continue;
            const QString deviceId = QString::fromWCharArray(id); CoTaskMemFree(id);
            QString name = deviceId;
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT value; PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
                    name = QString::fromWCharArray(value.pwszVal);
                PropVariantClear(&value);
            }
            result.push_back({deviceId, name});
        }
        return result;
    }

    bool start(const QString& id, AudioBuffer& buffer, QString& error) override {
        stop();
        { std::lock_guard<std::mutex> lock(errorMutex_); error_.clear(); }
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent_) { error = QStringLiteral("Cannot create audio stop event"); return false; }
        std::promise<QString> ready;
        auto result = ready.get_future();
        worker_ = std::thread([this, id, &buffer, ready = std::move(ready)]() mutable {
            run(id, buffer, ready);
        });
        error = result.get();
        if (!error.isEmpty()) { stop(); return false; }
        return true;
    }

    void stop() override {
        if (stopEvent_) SetEvent(stopEvent_);
        if (worker_.joinable()) worker_.join();
        if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    }

    QString takeError() override {
        std::lock_guard<std::mutex> lock(errorMutex_);
        auto value = error_; error_.clear(); return value;
    }
private:
    void run(const QString& id, AudioBuffer& buffer, std::promise<QString>& ready) {
        ComScope com;
        auto fail = [&](const char* action, HRESULT hr) { ready.set_value(hresultMessage(action, hr)); };
        if (FAILED(com.result)) { fail("COM initialization", com.result); return; }
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { fail("Audio enumeration", hr); return; }
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(id.utf16()), &device);
        if (FAILED(hr)) { fail("Selected output device", hr); return; }
        ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) { fail("Audio client", hr); return; }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 1; format.nSamplesPerSec = SampleRate;
        format.wBitsPerSample = 16; format.nBlockAlign = 2; format.nAvgBytesPerSec = SampleRate * 2;
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &format, nullptr);
        if (FAILED(hr)) { fail("48 kHz PCM output initialization", hr); return; }
        Handle event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        if (!event.value) { fail("Audio event creation", HRESULT_FROM_WIN32(GetLastError())); return; }
        hr = client->SetEventHandle(event.value);
        if (FAILED(hr)) { fail("Audio event registration", hr); return; }
        UINT32 capacity = 0;
        hr = client->GetBufferSize(&capacity);
        if (FAILED(hr) || !capacity) { fail("Audio buffer capacity", FAILED(hr) ? hr : E_FAIL); return; }
        ComPtr<IAudioRenderClient> renderer;
        hr = client->GetService(IID_PPV_ARGS(&renderer));
        if (FAILED(hr)) { fail("Audio render client", hr); return; }
        std::vector<std::int16_t> scratch(capacity);
        BYTE* initial = nullptr;
        hr = renderer->GetBuffer(capacity, &initial);
        if (FAILED(hr)) { fail("Initial audio buffer", hr); return; }
        hr = renderer->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(hr)) { fail("Initial audio release", hr); return; }
        hr = client->Start();
        if (FAILED(hr)) { fail("Audio start", hr); return; }
        DWORD taskIndex = 0;
        HANDLE priority = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
        ready.set_value(QString());
        HANDLE handles[] = {stopEvent_, event.value};
        while (true) {
            const auto wait = WaitForMultipleObjects(2, handles, FALSE, 2000);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_OBJECT_0 + 1) { hr = wait == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : AUDCLNT_E_DEVICE_INVALIDATED; break; }
            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;
            if (padding >= capacity) continue;
            const UINT32 available = capacity - padding;
            BYTE* data = nullptr;
            hr = renderer->GetBuffer(available, &data);
            if (FAILED(hr)) break;
            buffer.read(scratch.data(), available);
            std::memcpy(data, scratch.data(), available * sizeof(std::int16_t));
            hr = renderer->ReleaseBuffer(available, 0);
            if (FAILED(hr)) break;
        }
        client->Stop();
        if (priority) AvRevertMmThreadCharacteristics(priority);
        if (FAILED(hr)) {
            std::lock_guard<std::mutex> lock(errorMutex_);
            error_ = hresultMessage("Audio device stopped; refresh and reconnect", hr);
        }
    }
    HANDLE stopEvent_ = nullptr;
    std::thread worker_;
    std::mutex errorMutex_;
    QString error_;
};
std::unique_ptr<AudioOutput> createAudioOutput() { return std::make_unique<WasapiOutput>(); }
}
