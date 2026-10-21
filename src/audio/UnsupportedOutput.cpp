#include "audio/AudioOutput.h"
namespace minemic {
class UnsupportedOutput final : public AudioOutput {
    QVector<OutputDevice> devices(QString& error) override { error = QStringLiteral("Linux audio output is not implemented yet."); return {}; }
    bool start(const QString&, AudioBuffer&, QString& error) override { error = QStringLiteral("Unsupported audio backend"); return false; }
    void stop() override {}
    QString takeError() override { return {}; }
};
std::unique_ptr<AudioOutput> createAudioOutput() { return std::make_unique<UnsupportedOutput>(); }
}
