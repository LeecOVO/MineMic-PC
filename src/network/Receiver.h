#pragma once
#include "core/AudioBuffer.h"
#include "audio/AudioReference.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QHash>
class QTcpServer;
class QTcpSocket;
class QUdpSocket;
class QTimer;
namespace minemic {
class LanAdvertiser;
class Receiver : public QObject {
    Q_OBJECT
public:
    explicit Receiver(AudioBuffer& buffer, QObject* parent = nullptr, std::unique_ptr<AudioReference> reference = {}, const QString& initialCode = {});
    ~Receiver() override;
    QString pairingCode() const { return code_; }
    static QByteArray authenticate(const Packet& packet, const QByteArray& key);
public slots:
    void startListening(quint16 port);
    void stopListening();
    void manualDisconnect();
    void refreshPairingCode();
    void setReferenceDevice(const QString& id);
signals:
    void listening(quint16 port);
    void stopped();
    void pairingCodeChanged(const QString& code);
    void referenceStatusChanged(const QString& status);
    void peerChanged(const QString& address);
    void connectionChanged(bool locked, bool online);
    void discoveryWarning(const QString& message);
    void errorOccurred(const QString& message);
    void statistics(quint64 packets, quint64 rejected, quint64 missing, quint64 late, quint64 resyncs, int queuedMs, int peak);
private:
    void acceptControls();
    void readControl(QTcpSocket* socket);
    void handleControl(QTcpSocket* socket, const QJsonObject& message);
    void reply(QTcpSocket* socket, QJsonObject message);
    void welcome(QTcpSocket* socket, const QString& nonce, bool resume);
    void receivePending();
    void tick();
    void releaseOwner(const QString& reason);
    void rotateCode();
    void stopReference();
    void startReference();
    void sendReferences();
    AudioBuffer& buffer_;
    QUdpSocket* socket_ = nullptr;
    QTcpServer* server_ = nullptr;
    QTimer* timer_ = nullptr;
    LanAdvertiser* advertiser_ = nullptr;
    QHash<QTcpSocket*, QByteArray> controls_;
    QPointer<QTcpSocket> ownerSocket_;
    QString code_, clientId_, connectNonce_, token_, peerName_;
    QByteArray key_;
    QHostAddress peer_;
    quint16 peerPort_ = 0;
    quint32 session_ = 0;
    bool haveAudio_ = false, online_ = false;
    quint32 newestSequence_ = 0, newestTimestamp_ = 0;
    quint64 seenSequences_ = 0, rejected_ = 0;
    QElapsedTimer lastAudio_, attemptsWindow_;
    int attempts_ = 0;
    bool drainScheduled_ = false;
    std::unique_ptr<AudioReference> reference_;
    QTimer* referenceTimer_ = nullptr;
    QString referenceDevice_;
    bool referenceWanted_ = false, referenceFlowing_ = false;
    bool listening_ = false;
    quint64 lastReferenceSent_ = 0;
    quint64 lastReferenceReport_ = 0, referenceFramesRead_ = 0, referenceStaleDrops_ = 0, referenceFutureDrops_ = 0;
    quint32 referenceSequence_ = 0, referenceEpoch_ = 0;
};
}
