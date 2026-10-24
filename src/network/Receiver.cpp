#include "network/Receiver.h"
#include "network/LanAdvertiser.h"
#include <QCryptographicHash>
#include <QHostInfo>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QVariant>
#include <cstring>
namespace minemic {
namespace {
QByteArray randomBytes(int count) {
    QByteArray bytes(count, '\0');
    for (auto& byte : bytes) byte = static_cast<char>(QRandomGenerator::system()->generate() & 255);
    return bytes;
}
bool sameBytes(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) return false;
    unsigned int difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i) difference |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return difference == 0;
}
bool hexId(const QString& value) {
    if (value.size() != 32) return false;
    for (auto c : value) if (!(c >= QLatin1Char('0') && c <= QLatin1Char('9')) && !(c >= QLatin1Char('a') && c <= QLatin1Char('f'))) return false;
    return true;
}
}
Receiver::Receiver(AudioBuffer& buffer, QObject* parent, std::unique_ptr<AudioReference> reference, const QString& initialCode)
    : QObject(parent), buffer_(buffer), reference_(reference ? std::move(reference) : createAudioReference()) {
    bool valid = initialCode.size() == 6;
    for (auto c : initialCode) valid = valid && c >= QLatin1Char('0') && c <= QLatin1Char('9');
    if (valid) code_ = initialCode; else rotateCode();
}
Receiver::~Receiver() { stopReference(); }
void Receiver::setReferenceDevice(const QString& id) {
    if (referenceDevice_ == id) return;
    const bool restart = referenceWanted_;
    stopReference(); referenceDevice_ = id;
    if (restart) startReference();
}
void Receiver::stopReference() {
    referenceWanted_ = false; referenceFlowing_ = false; lastReferenceSent_ = 0;
    pendingReference_.reset(); referenceFutureLead_ = 0;
    emit referenceStatusChanged(tr("外放参考：未启用，手机开启回声抑制后自动使用"));
    if (referenceTimer_) { delete referenceTimer_; referenceTimer_ = nullptr; }
    reference_->stop();
}
void Receiver::startReference() {
    stopReference();
    QString error;
    if (!reference_->supported() || !reference_->start(referenceDevice_, error)) {
        reply(ownerSocket_, {{"type", "reference"}, {"state", "unavailable"}, {"detail", error.left(200)}});
        emit referenceStatusChanged(tr("外放参考不可用：%1").arg(error)); return;
    }
    referenceWanted_ = true; referenceSequence_ = 0; lastReferenceSent_ = referenceClockMicros();
    lastReferenceReport_ = lastReferenceSent_; referenceFramesRead_ = referenceStaleDrops_ = referenceFutureDrops_ = 0;
    emit referenceStatusChanged(tr("外放参考：已开启，等待电脑播放声音"));
    referenceEpoch_ = QRandomGenerator::system()->generate();
    referenceTimer_ = new QTimer(this); referenceTimer_->setTimerType(Qt::PreciseTimer); referenceTimer_->setInterval(10);
    connect(referenceTimer_, &QTimer::timeout, this, &Receiver::sendReferences); referenceTimer_->start();
    reply(ownerSocket_, {{"type", "reference"}, {"state", "starting"}});
}
void Receiver::sendReferences() {
    const auto error = reference_->takeError();
    if (!error.isEmpty()) {
        stopReference();
        reply(ownerSocket_, {{"type", "reference"}, {"state", "unavailable"}, {"detail", error.left(200)}});
        emit referenceStatusChanged(tr("外放参考不可用：%1").arg(error)); return;
    }
    if (referenceFlowing_ && referenceClockMicros() - lastReferenceSent_ > 1000000) {
        referenceFlowing_ = false;
        emit referenceStatusChanged(tr("外放参考：等待电脑播放声音；麦克风继续工作"));
    }
    ReferenceFrame frame;
    for (int count = 0; count < 16; ++count) {
        if (pendingReference_) { frame = *pendingReference_; pendingReference_.reset(); }
        else {
            if (!reference_->take(frame)) break;
            ++referenceFramesRead_;
        }
        const auto now = referenceClockMicros();
        if (!referenceWanted_ || !ownerSocket_ || !socket_ || !peerPort_ || !online_) continue;
        // Loopback may expose audio before its scheduled physical playback.
        // Hold one frame until it enters the phone's existing 20 ms window;
        // never rewrite its timestamp or delay the microphone upload path.
        if (frame.timeMicros > now && frame.timeMicros - now > 20000) {
            const auto lead = frame.timeMicros - now;
            if (lead <= 200000) { pendingReference_ = frame; break; }
            ++referenceFutureDrops_; referenceFutureLead_ = lead; continue;
        }
        if (frame.timeMicros <= now && now - frame.timeMicros > 50000) { ++referenceStaleDrops_; continue; }
        const auto sequence = referenceSequence_++;
        for (int channel = 0; channel < 2; ++channel) {
            QByteArray body(1008, '\0'); std::memcpy(body.data(), "MREF", 4);
            body[4] = 1; body[5] = 2; body[6] = static_cast<char>(channel);
            auto put = [&](int offset, quint64 value, int size = 4) {
                for (int i = 0; i < size; ++i) body[offset + i] = static_cast<char>(value >> ((size - 1 - i) * 8));
            };
            put(8, session_); put(12, sequence); put(16, frame.position, 8); put(24, frame.timeMicros, 8);
            put(32, 48000); put(36, 480, 2); put(38, 960, 2); put(40, referenceEpoch_ + frame.epoch);
            for (int i = 0; i < 480; ++i) {
                const auto sample = static_cast<quint16>(frame.pcm[2*i + channel]);
                body[48 + 2*i] = static_cast<char>(sample); body[49 + 2*i] = static_cast<char>(sample >> 8);
            }
            const auto bytes = body + QMessageAuthenticationCode::hash(body, key_, QCryptographicHash::Sha256);
            if (socket_->writeDatagram(bytes, peer_, peerPort_) != bytes.size()) {
                const auto error = socket_->errorString(); stopReference();
                reply(ownerSocket_, {{"type", "reference"}, {"state", "unavailable"}, {"detail", error.left(200)}});
                emit referenceStatusChanged(tr("外放参考回传失败：%1").arg(error)); return;
            }
        }
        lastReferenceSent_ = referenceClockMicros();
        referenceStaleDrops_ = referenceFutureDrops_ = 0;
        if (!referenceFlowing_) {
            referenceFlowing_ = true;
            emit referenceStatusChanged(tr("外放参考：正在向已连接手机回传"));
        }
    }
    if (referenceWanted_ && !referenceFlowing_ && referenceClockMicros() - lastReferenceReport_ >= 1000000) {
        QString detail = reference_->status();
        if (referenceStaleDrops_ || referenceFutureDrops_) {
            detail += tr("；参考时间不同步：过期 %1 帧，超前 %2 帧（领先 %3 ms）").arg(referenceStaleDrops_).arg(referenceFutureDrops_).arg(referenceFutureLead_ / 1000);
        } else if (referenceFramesRead_ && !peerPort_) {
            detail += tr("；等待手机音频通道建立");
        }
        if (detail.isEmpty()) detail = tr("等待当前外放设备的播放数据");
        emit referenceStatusChanged(tr("外放参考：%1").arg(detail));
        reply(ownerSocket_, {{"type", "reference"}, {"state", "waiting"}, {"detail", detail.left(200)}});
        lastReferenceReport_ = referenceClockMicros();
    }
}
void Receiver::rotateCode() {
    QString next;
    do { next = QStringLiteral("%1").arg(QRandomGenerator::system()->bounded(1000000U), 6, 10, QLatin1Char('0')); } while (next == code_);
    code_ = next; emit pairingCodeChanged(code_);
}
QByteArray Receiver::authenticate(const Packet& packet, const QByteArray& key) {
    const auto bytes = encodePacket(packet);
    QByteArray body(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    return body + QMessageAuthenticationCode::hash(body, key, QCryptographicHash::Sha256);
}
void Receiver::startListening(quint16 port) {
    if (listening_) { emit listening(server_->serverPort()); return; }
    stopListening(); rejected_ = 0; attempts_ = 0; attemptsWindow_.start();
    server_ = new QTcpServer(this); server_->setMaxPendingConnections(8);
    if (!server_->listen(QHostAddress::AnyIPv4, port)) {
        auto message = server_->errorString(); delete server_; server_ = nullptr;
        emit errorOccurred(tr("无法监听控制端口：%1").arg(message)); return;
    }
    socket_ = new QUdpSocket(this);
    if (!socket_->bind(QHostAddress::AnyIPv4, server_->serverPort(), QAbstractSocket::DontShareAddress)) {
        auto message = socket_->errorString(); stopListening();
        emit errorOccurred(tr("无法监听音频端口：%1").arg(message)); return;
    }
    socket_->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 65536);
    connect(server_, &QTcpServer::newConnection, this, &Receiver::acceptControls);
    connect(socket_, &QUdpSocket::readyRead, this, &Receiver::receivePending);
    timer_ = new QTimer(this); timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &Receiver::tick); timer_->start();
    advertiser_ = new LanAdvertiser(this);
    connect(advertiser_, &LanAdvertiser::warning, this, &Receiver::discoveryWarning);
    advertiser_->start(server_->serverPort());
    listening_ = true;
    emit listening(server_->serverPort());
}
void Receiver::stopListening() {
    const bool wasListening = listening_; listening_ = false;
    releaseOwner(QStringLiteral("stopped"));
    if (wasListening) rotateCode();
    if (advertiser_) { delete advertiser_; advertiser_ = nullptr; }
    if (timer_) { delete timer_; timer_ = nullptr; }
    if (socket_) { delete socket_; socket_ = nullptr; }
    if (server_) { delete server_; server_ = nullptr; }
    const auto clients = controls_.keys(); controls_.clear();
    for (auto* client : clients) { client->disconnect(this); client->abort(); client->deleteLater(); }
    emit stopped();
}
void Receiver::manualDisconnect() { releaseOwner(QStringLiteral("pc")); }
void Receiver::refreshPairingCode() { rotateCode(); }
void Receiver::releaseOwner(const QString& reason) {
    stopReference();
    if (ownerSocket_) {
        reply(ownerSocket_, {{"type", "disconnected"}, {"reason", reason}});
        ownerSocket_->disconnectFromHost();
    }
    ownerSocket_.clear(); clientId_.clear(); connectNonce_.clear(); token_.fill(QLatin1Char('0')); token_.clear();
    key_.fill(0); key_.clear(); peer_.clear(); peerName_.clear(); peerPort_ = 0; session_ = 0;
    haveAudio_ = online_ = false; seenSequences_ = 0; lastAudio_.invalidate(); buffer_.reset();
    if (timer_) timer_->setInterval(1000);
    emit peerChanged(QString()); emit connectionChanged(false, false);
}
void Receiver::acceptControls() {
    while (server_ && server_->hasPendingConnections()) {
        auto* client = server_->nextPendingConnection();
        if (controls_.size() >= 8) { client->abort(); client->deleteLater(); continue; }
        client->setParent(this); client->setReadBufferSize(4096);
        client->setSocketOption(QAbstractSocket::LowDelayOption, 1); controls_.insert(client, {});
        connect(client, &QTcpSocket::readyRead, this, [this, client] { readControl(client); });
        connect(client, &QTcpSocket::disconnected, this, [this, client] {
            if (ownerSocket_ == client) { stopReference(); ownerSocket_.clear(); online_ = false; emit connectionChanged(true, false); }
            controls_.remove(client); client->deleteLater();
        });
        QTimer::singleShot(5000, client, [this, client] { if (ownerSocket_ != client) client->disconnectFromHost(); });
    }
}
void Receiver::reply(QTcpSocket* client, QJsonObject message) {
    if (!client || client->state() != QAbstractSocket::ConnectedState) return;
    message.insert("app", "MineMic"); message.insert("v", "2");
    if (client->bytesToWrite() > 8192) { client->abort(); return; }
    client->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}
void Receiver::readControl(QTcpSocket* client) {
    if (!controls_.contains(client)) return;
    controls_[client] += client->readAll();
    if (controls_[client].size() > 4096) { client->abort(); return; }
    for (int count = 0; count < 16; ++count) {
        if (!controls_.contains(client)) break;
        auto& pending = controls_[client];
        const auto newline = pending.indexOf('\n'); if (newline < 0) break;
        const auto line = pending.first(newline); pending.remove(0, newline + 1);
        QJsonParseError parse;
        const auto json = QJsonDocument::fromJson(line, &parse);
        if (line.size() > 1024 || parse.error != QJsonParseError::NoError || !json.isObject()) { client->abort(); return; }
        handleControl(client, json.object());
        if (client->state() != QAbstractSocket::ConnectedState) break;
    }
}
void Receiver::welcome(QTcpSocket* client, const QString& nonce, bool resume) {
    stopReference();
    if (ownerSocket_ && ownerSocket_ != client) ownerSocket_->disconnectFromHost();
    ownerSocket_ = client; peer_ = client->peerAddress(); peerPort_ = 0;
    if (resume || !session_) {
        do { session_ = QRandomGenerator::system()->generate(); } while (!session_);
        key_ = randomBytes(16); haveAudio_ = false; seenSequences_ = 0; buffer_.reset();
    }
    lastAudio_.start(); online_ = true; timer_->setInterval(250);
    reply(client, {{"type", "welcome"}, {"nonce", nonce}, {"session", QString::number(session_)},
                   {"key", QString::fromLatin1(key_.toHex())}, {"token", token_},
                   {"pcName", QHostInfo::localHostName()}, {"port", QString::number(server_->serverPort())},
                   {"caps", reference_->supported() ? "ref-pcm-stereo-v1" : ""}});
    emit peerChanged(QStringLiteral("%1 (%2)").arg(peerName_, peer_.toString())); emit connectionChanged(true, true);
}
void Receiver::handleControl(QTcpSocket* client, const QJsonObject& m) {
    auto reject = [&](const QString& reason) { reply(client, {{"type", "error"}, {"reason", reason}}); };
    if (m.value("app") != "MineMic" || m.value("v") != "2") { reject("version"); client->disconnectFromHost(); return; }
    const auto type = m.value("type").toString();
    if (type == "clock" && ownerSocket_ == client && reference_->supported()) {
        const auto received = referenceClockMicros();
        bool valid = false; const auto t1 = m.value("t1").toString(); t1.toULongLong(&valid);
        if (!valid || t1.size() > 20) { reject("invalid_request"); return; }
        reply(client, {{"type", "clock"}, {"t1", t1}, {"t2", QString::number(received)}, {"t3", QString::number(referenceClockMicros())}});
        return;
    }
    if (type == "reference" && ownerSocket_ == client && reference_->supported()) {
        const auto enabled = m.value("enabled").toString();
        if (enabled == "0") { stopReference(); reply(client, {{"type", "reference"}, {"state", "off"}}); }
        else if (enabled == "1") {
            if (!referenceWanted_) startReference();
            else reply(client, {{"type", "reference"}, {"state", "starting"}});
        }
        else reject("invalid_request");
        return;
    }
    if (type == "ping" && ownerSocket_ == client) {
        lastAudio_.restart();
        if (!online_) { online_ = true; emit connectionChanged(true, true); }
        reply(client, {{"type", "pong"}}); return;
    }
    if (type == "disconnect") {
        const bool owns = !clientId_.isEmpty() && m.value("clientId").toString() == clientId_ &&
            sameBytes(m.value("token").toString().toLatin1(), token_.toLatin1());
        if (!owns) { reject("not_owner"); return; }
        if (client != ownerSocket_) reply(client, {{"type", "disconnected"}, {"reason", "phone"}});
        releaseOwner("phone"); return;
    }
    if (type == "resume") {
        if (clientId_.isEmpty() || m.value("clientId").toString() != clientId_ ||
            !sameBytes(m.value("token").toString().toLatin1(), token_.toLatin1())) { reject("not_owner"); return; }
        if (!hexId(m.value("nonce").toString())) { reject("invalid_request"); return; }
        welcome(client, m.value("nonce").toString(), true); return;
    }
    if (type != "connect") { reject("invalid_request"); return; }
    const auto mode = m.value("mode").toString();
    const auto id = m.value("clientId").toString(), nonce = m.value("nonce").toString();
    if (!hexId(id) || !hexId(nonce) || (mode != "code" && mode != "manual")) { reject("invalid_request"); return; }
    if (!attemptsWindow_.isValid() || attemptsWindow_.elapsed() >= 1000) { attemptsWindow_.restart(); attempts_ = 0; }
    if (++attempts_ > 12) { reject("rate_limited"); return; }
    if (mode == "code" && m.value("code").toString() != code_) { reject("wrong_code"); return; }
    if (!clientId_.isEmpty()) {
        if (id == clientId_ && nonce == connectNonce_ && client->peerAddress() == peer_) welcome(client, nonce, false);
        else reject("busy");
        return;
    }
    clientId_ = id; connectNonce_ = nonce; token_ = QString::fromLatin1(randomBytes(16).toHex());
    peerName_ = m.value("name").toString().left(64);
    if (peerName_.isEmpty()) peerName_ = tr("手机");
    welcome(client, nonce, false);
}
void Receiver::receivePending() {
    drainScheduled_ = false;
    if (!socket_) return;
    for (int count = 0; count < 64 && socket_->hasPendingDatagrams(); ++count) {
        if (!session_ || socket_->pendingDatagramSize() < static_cast<qint64>(HeaderSize + TagSize) || socket_->pendingDatagramSize() > static_cast<qint64>(MaxDatagram)) {
            char ignored; socket_->readDatagram(&ignored, 1); ++rejected_; continue;
        }
        const auto datagram = socket_->receiveDatagram();
        const auto bytes = datagram.data(), body = bytes.first(bytes.size() - TagSize);
        if (datagram.senderAddress() != peer_ || (peerPort_ && datagram.senderPort() != peerPort_) ||
            !sameBytes(bytes.last(TagSize), QMessageAuthenticationCode::hash(body, key_, QCryptographicHash::Sha256))) { ++rejected_; continue; }
        Packet packet;
        if (!decodePacket(reinterpret_cast<const std::uint8_t*>(body.constData()), body.size(), packet) || packet.session != session_) { ++rejected_; continue; }
        if (packet.type != PacketType::Audio && packet.type != PacketType::Heartbeat) { ++rejected_; continue; }
        if (!peerPort_) peerPort_ = static_cast<quint16>(datagram.senderPort());
        if (packet.type == PacketType::Heartbeat) continue;
        const auto distance = sequenceDistance(packet.sequence, newestSequence_);
        if (haveAudio_ && (distance <= -64 || (distance <= 0 && (seenSequences_ & (quint64(1) << -distance))) ||
            packet.timestamp - newestTimestamp_ != static_cast<quint32>(distance * static_cast<std::int64_t>(FramesPerPacket)))) { ++rejected_; continue; }
        if (buffer_.push(packet.sequence, decodePcm(packet))) {
            if (!haveAudio_ || distance > 0) {
                seenSequences_ = !haveAudio_ || distance >= 64 ? 1 : (seenSequences_ << distance) | 1;
                newestSequence_ = packet.sequence; newestTimestamp_ = packet.timestamp; haveAudio_ = true; lastAudio_.restart();
            } else seenSequences_ |= quint64(1) << -distance;
        } else ++rejected_;
    }
    if (socket_ && socket_->hasPendingDatagrams() && !drainScheduled_) {
        drainScheduled_ = true; QMetaObject::invokeMethod(this, &Receiver::receivePending, Qt::QueuedConnection);
    }
}
void Receiver::tick() {
    if (session_ && online_ && lastAudio_.isValid() && lastAudio_.elapsed() >= 3000) {
        online_ = false; buffer_.reset(); emit connectionChanged(true, false);
    }
    const auto s = buffer_.stats();
    emit statistics(s.accepted, rejected_, s.missing, s.late, s.resyncs, static_cast<int>(s.queuedFrames * 1000 / SampleRate), buffer_.takePeak());
}
}
