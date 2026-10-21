#pragma once
#include "audio/AudioOutput.h"
#include "network/Receiver.h"
#include <QMainWindow>
#include <QThread>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QSystemTrayIcon;

namespace minemic {
class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    void refreshDevices();
    void updateConnectionInfo();
    void setRunning(bool running);
    void start();
    void stop();
    void showError(const QString& error);
    AudioBuffer buffer_;
    std::unique_ptr<AudioOutput> output_;
    QThread networkThread_;
    Receiver* receiver_ = nullptr;
    QString pairingCode_;
    QString peerLabel_;
    bool locked_ = false;
    bool running_ = false, busy_ = false;
    QComboBox *devices_ = nullptr, *addresses_ = nullptr;
    QSpinBox* port_ = nullptr;
    QLineEdit* connection_ = nullptr;
    QPushButton *start_ = nullptr, *refresh_ = nullptr, *disconnect_ = nullptr;
    QLabel *status_ = nullptr, *statistics_ = nullptr, *gainText_ = nullptr;
    QProgressBar* meter_ = nullptr;
    QSlider* gain_ = nullptr;
    QCheckBox* muted_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
};
QIcon applicationIcon();
}
