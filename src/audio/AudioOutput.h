#pragma once
#include "core/AudioBuffer.h"
#include <QString>
#include <QVector>
#include <memory>

namespace minemic {
struct OutputDevice { QString id; QString name; };
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    virtual QVector<OutputDevice> devices(QString& error) = 0;
    virtual bool start(const QString& deviceId, AudioBuffer& buffer, QString& error) = 0;
    virtual void stop() = 0;
    // Polled by the low-rate UI timer; never calls Qt from the real-time callback.
    virtual QString takeError() = 0;
};
std::unique_ptr<AudioOutput> createAudioOutput();
}
