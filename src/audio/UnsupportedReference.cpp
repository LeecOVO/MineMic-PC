#include "audio/AudioReference.h"
namespace minemic {
class UnsupportedReference final : public AudioReference {
public:
    bool supported() const override { return false; }
    bool start(const QString&, QString& error) override { error = QStringLiteral("PC playback reference requires Windows WASAPI"); return false; }
    void stop() override {}
    bool take(ReferenceFrame&) override { return false; }
    QString takeError() override { return {}; }
};
std::unique_ptr<AudioReference> createAudioReference() { return std::make_unique<UnsupportedReference>(); }
}
