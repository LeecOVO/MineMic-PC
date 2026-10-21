#include "audio/AudioOutput.h"
#include <QCoreApplication>
#include <QThread>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    auto output = minemic::createAudioOutput();
    QString error;
    const auto devices = output->devices(error);
    if (devices.isEmpty()) {
        std::cout << "SKIP: No output device: " << error.toStdString() << '\n';
        return 77;
    }
    minemic::AudioBuffer buffer; // No input packets: output must remain silent.
    if (!output->start(devices.first().id, buffer, error)) {
        std::cerr << "FAIL: Open audio device: " << error.toStdString() << '\n'; return 1;
    }
    QThread::msleep(300);
    error = output->takeError(); output->stop();
    if (!error.isEmpty()) { std::cerr << "FAIL: " << error.toStdString() << '\n'; return 1; }
    std::cout << "Opened 48 kHz mono PCM output, rendered silence, stopped cleanly\n";
}
