#include "network/Receiver.h"
#include "network/LanAdvertiser.h"
#include "audio/ReferenceTiming.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QProcess>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QThread>
#include <QTimer>
#include <functional>
#include <iostream>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <QMessageAuthenticationCode>
#include <QCryptographicHash>
using namespace minemic;
namespace {
void check(bool pass, const char* message) { if (!pass) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); } }
bool until(const std::function<bool()>& f, int ms = 1800) {
    QElapsedTimer timer; timer.start();
    do { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); if (f()) return true; QThread::msleep(1); } while (timer.elapsed() < ms);
    return false;
}
void connectTo(QTcpSocket& socket, quint16 port, const QHostAddress& host = QHostAddress::LocalHost) {
    socket.connectToHost(host, port);
    check(until([&] { return socket.state() == QAbstractSocket::ConnectedState; }), "TCP connected");
}
QJsonObject rpc(QTcpSocket& socket, QJsonObject request) {
    request.insert("app", "MineMic"); request.insert("v", "2");
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    check(until([&] { return socket.canReadLine(); }), "control response received");
    return QJsonDocument::fromJson(socket.readLine()).object();
}
QJsonObject connectRequest(QString id, QString nonce, QString code = {}) {
    return {{"type", "connect"}, {"mode", code.isEmpty() ? "manual" : "code"}, {"code", code},
            {"clientId", id}, {"nonce", nonce}, {"name", "Test phone"}};
}
class TestReference final : public AudioReference {
public:
    bool active = false;
    bool available = true;
    bool estimateInvalidClock = false;
    std::int64_t timestampOffset = -10000;
    QString error;
    bool supported() const override { return true; }
    bool start(const QString&, QString&) override { active = true; return true; }
    void stop() override { active = false; }
    bool take(ReferenceFrame& out) override {
        const auto now = referenceClockMicros();
        if (!active || !available) return false;
        if (!origin_) origin_ = now;
        const auto captureTime = timestampOffset > 0 ? origin_ + position_ * 1000000 / 48000 : now;
        if (timestampOffset > 0 ? captureTime > now : now - last_ < 10000) return false;
        last_ = now; out.position = position_; position_ += 480;
        out.timeMicros = static_cast<std::uint64_t>(static_cast<std::int64_t>(captureTime) + timestampOffset); out.epoch = 1;
        if (estimateInvalidClock) {
            out.timeMicros = timing_.observe(out.position, 480, 48000, out.timeMicros, now, false, false).time;
            if (!out.timeMicros) return false;
        }
        for (int i = 0; i < 480; ++i) { out.pcm[2*i] = 1000; out.pcm[2*i+1] = -2000; }
        return true;
    }
    QString takeError() override { const auto result = error; error.clear(); return result; }
    QString status() override { return QStringLiteral("Test speakers · 尚未采集到播放数据"); }
private:
    std::uint64_t position_ = 0, last_ = 0, origin_ = 0;
    ReferenceTiming timing_;
};
void referenceTests() {
    AudioBuffer buffer;
    auto source = std::make_unique<TestReference>(); auto* native = source.get();
    Receiver receiver(buffer, nullptr, std::move(source)); quint16 port = 0;
    QObject::connect(&receiver, &Receiver::listening, [&](quint16 value) { port = value; });
    receiver.startListening(0);
    QTcpSocket owner, stranger; connectTo(owner, port); connectTo(stranger, port);
    const auto welcome = rpc(owner, connectRequest(QString(32, 'a'), QString(32, 'b')));
    check(welcome["caps"] == "ref-pcm-stereo-v1" && !native->active, "reference capability advertised without starting capture");
    check(rpc(stranger, {{"type", "reference"}, {"enabled", "1"}})["type"] == "error" && !native->active, "non-owner cannot capture PC audio");
    const auto clock = rpc(owner, {{"type", "clock"}, {"t1", "1234"}});
    check(clock["t1"] == "1234" && clock["t3"].toString().toULongLong() >= clock["t2"].toString().toULongLong(), "four-timestamp clock exchange");
    const auto key = QByteArray::fromHex(welcome["key"].toString().toLatin1());
    const auto session = welcome["session"].toString().toUInt();
    QUdpSocket audio;
    Packet packet; packet.type = PacketType::Audio; packet.session = session; packet.payload.resize(MaxPayload);
    audio.writeDatagram(Receiver::authenticate(packet, key), QHostAddress::LocalHost, port);
    check(until([&] { return buffer.stats().accepted == 1; }), "reference destination bound by authenticated microphone");
    check(rpc(owner, {{"type", "reference"}, {"enabled", "1"}})["state"] == "starting" && native->active, "only owner starts loopback on request");
    for (int channel = 0; channel < 2; ++channel) {
        check(until([&] { return audio.hasPendingDatagrams(); }), "stereo reference received");
        QByteArray bytes(static_cast<qsizetype>(audio.pendingDatagramSize()), '\0'); audio.readDatagram(bytes.data(), bytes.size());
        check(bytes.size() == 1040 && bytes.first(4) == "MREF" && bytes[6] == channel, "MTU-safe reference channel format");
        check(bytes.last(32) == QMessageAuthenticationCode::hash(bytes.first(1008), key, QCryptographicHash::Sha256), "reference authenticated with current session key");
        const auto value = static_cast<std::int16_t>(static_cast<unsigned char>(bytes[48]) | (static_cast<unsigned char>(bytes[49]) << 8));
        check(value == (channel ? -2000 : 1000), "left and right remain independent");
    }
    native->error = QStringLiteral("Reference loopback format (0x88890008)");
    check(until([&] { return owner.canReadLine(); }), "capture failure reported asynchronously");
    const auto failed = QJsonDocument::fromJson(owner.readLine()).object();
    check(failed["state"] == "unavailable" && failed["detail"].toString().contains("0x88890008") && !native->active,
          "actual capture failure detail reaches phone without releasing ownership");
    check(rpc(owner, {{"type", "reference"}, {"enabled", "1"}})["state"] == "starting", "capture can recover after explicit failure");
    check(rpc(owner, {{"type", "reference"}, {"enabled", "0"}})["state"] == "off" && !native->active, "turning AEC off stops capture");
    rpc(owner, {{"type", "reference"}, {"enabled", "1"}}); owner.abort();
    check(until([&] { return !native->active; }), "network loss stops reference without releasing ownership");
    check(rpc(stranger, connectRequest(QString(32, 'c'), QString(32, 'd')))["reason"] == "busy", "reference disconnect preserves first owner");
    receiver.stopListening();
}
void referenceTimingTests() {
    for (const auto offset : {10000, 100000, 250000, 300000, -100000, 0}) {
        AudioBuffer buffer;
        auto source = std::make_unique<TestReference>(); auto* native = source.get();
        native->timestampOffset = offset; native->available = offset != 0;
        native->estimateInvalidClock = offset == 250000;
        Receiver receiver(buffer, nullptr, std::move(source)); quint16 port = 0;
        QObject::connect(&receiver, &Receiver::listening, [&](quint16 p) { port = p; });
        receiver.startListening(0); const auto code = receiver.pairingCode();
        QTcpSocket owner; connectTo(owner, port);
        const auto welcome = rpc(owner, connectRequest(QString(32, 'a'), QString(32, 'b')));
        QUdpSocket audio; Packet packet; packet.type = PacketType::Audio;
        packet.session = welcome["session"].toString().toUInt(); packet.payload.resize(MaxPayload);
        audio.writeDatagram(Receiver::authenticate(packet, QByteArray::fromHex(welcome["key"].toString().toLatin1())), QHostAddress::LocalHost, port);
        check(until([&] { return buffer.stats().accepted == 1; }), "timing test binds authenticated audio destination");
        check(rpc(owner, {{"type", "reference"}, {"enabled", "1"}})["state"] == "starting", "timing test starts reference");
        if (offset == 10000 || offset == 100000 || offset == 250000) {
            const auto started = referenceClockMicros();
            check(until([&] { return audio.hasPendingDatagrams(); }), "slightly future playback references reach phone instead of being discarded");
            if (offset == 100000) {
                check(referenceClockMicros() - started >= 50000, "scheduled reference waits for playback time without falsifying timestamps");
                QByteArray bytes(static_cast<qsizetype>(audio.pendingDatagramSize()), '\0'); audio.readDatagram(bytes.data(), bytes.size());
                std::uint64_t timestamp = 0;
                for (int i = 24; i < 32; ++i) timestamp = (timestamp << 8) | static_cast<unsigned char>(bytes[i]);
                check(timestamp <= referenceClockMicros() + 21000, "delivered reference fits the existing phone future window");
            }
        } else {
            check(until([&] { return owner.canReadLine(); }, 2400), "waiting reference reports its actual capture stage");
            const auto report = QJsonDocument::fromJson(owner.readLine()).object();
            check(report["state"] == "waiting" && report["detail"].toString().contains("Test speakers"), "capture status names the actual reference endpoint");
            if (offset > 0) check(report["detail"].toString().contains(QStringLiteral("超前")), "far-future reference rejection is visible");
            if (offset < 0) check(report["detail"].toString().contains(QStringLiteral("过期")), "stale reference rejection is visible");
            check(!audio.hasPendingDatagrams(), "invalid or unavailable reference never sends fabricated audio");
        }
        check(native->active && receiver.pairingCode() == code, "reference waiting preserves capture request and pairing code");
        receiver.stopListening();
    }
}
void disconnectHandshakeTests() {
    AudioBuffer buffer; Receiver receiver(buffer); quint16 port = 0; bool locked = false;
    QObject::connect(&receiver, &Receiver::listening, [&](quint16 p) { port = p; });
    QObject::connect(&receiver, &Receiver::connectionChanged, [&](bool value, bool) { locked = value; });
    receiver.startListening(0); const auto code = receiver.pairingCode();
    QTcpSocket first; connectTo(first, port);
    auto welcome = rpc(first, connectRequest(QString(32, 'a'), QString(32, 'b')));
    check(welcome["instance"].toString().size() == 32, "server process advertises an opaque restart identity");
    QElapsedTimer elapsed; elapsed.start(); receiver.manualDisconnect();
    check(!locked && elapsed.elapsed() < 150 && receiver.pairingCode() == code, "PC release changes logical state immediately without waiting for ACK");
    check(until([&] { return first.canReadLine(); }, 150), "PC disconnect notification is flushed before closing TCP");
    auto fin = QJsonDocument::fromJson(first.readLine()).object();
    check(fin["type"] == "disconnected" && fin["reason"] == "pc" && !fin["closeId"].toString().isEmpty(), "PC sends an identifiable disconnect notification");
    QTcpSocket next; connectTo(next, port);
    check(rpc(next, connectRequest(QString(32, 'c'), QString(32, 'd')))["type"] == "welcome", "new owner can connect while old FIN waits for ACK");
    first.write(QJsonDocument(QJsonObject{{"app", "MineMic"}, {"v", "2"}, {"type", "disconnect_ack"}, {"closeId", fin["closeId"]}}).toJson(QJsonDocument::Compact) + '\n');
    check(until([&] { return first.state() == QAbstractSocket::UnconnectedState; }, 350), "acknowledged old control is reclaimed");
    check(locked && rpc(next, {{"type", "ping"}})["type"] == "pong", "late ACK cannot release a new owner's session");
    elapsed.restart(); receiver.stopListening();
    check(!locked && elapsed.elapsed() < 150 && receiver.pairingCode() != code, "stop immediately releases ownership and rotates pairing code");
    check(until([&] { return next.canReadLine(); }, 150), "stopping receiver does not abort its unsent notification");
    fin = QJsonDocument::fromJson(next.readLine()).object();
    check(fin["type"] == "disconnected" && fin["reason"] == "stopped", "phone is told that PC reception stopped");
    receiver.startListening(port);
    QTcpSocket again; connectTo(again, port);
    check(rpc(again, connectRequest(QString(32, 'a'), QString(32, 'b')))["type"] == "welcome", "stopped receiver can listen and pair again on the same port");
    check(until([&] { return next.state() == QAbstractSocket::UnconnectedState; }, 350), "missing ACK cannot retain an old control socket indefinitely");
    check(rpc(again, {{"type", "ping"}})["type"] == "pong", "old disconnect deadline cannot close a restarted session");
    receiver.stopListening();
}
void discoveryBurstTests() {
    for (const bool ipv6 : {false, true}) {
    LanAdvertiser advertiser;
    const auto host = ipv6 ? QHostAddress::LocalHostIPv6 : QHostAddress::LocalHost;
    auto* socket = advertiser.findChild<QUdpSocket*>(ipv6 ? QStringLiteral("mdns-ipv6") : QStringLiteral("mdns-ipv4"));
    check(socket && socket->bind(host, 0), "discovery test binds an isolated IPv4/IPv6 UDP endpoint");
    QUdpSocket sender;
    QByteArray query(12, '\0'); query[5] = 1;
    query += QByteArray("\x08_minemic\x04_tcp\x05local\0\0\x0c\0\x01", 25);
    const QByteArray noise(12, '\0');
    bool firstNotification = false, pendingAfterFirst = false;
    QObject::connect(socket, &QUdpSocket::readyRead, &advertiser, [&] {
        if (!firstNotification) {
            firstNotification = true; pendingAfterFirst = socket->hasPendingDatagrams();
        }
    });
    for (int i = 0; i < 8; ++i)
        sender.writeDatagram(i == 0 ? query : noise, host, socket->localPort());
    check(until([&] { return firstNotification; }), "discovery receives the first query burst");
    check(!pendingAfterFirst, "a matching query must not leave a short burst unread after readyRead");
    // A phone's browse/resolve burst can leave both relevant and unrelated
    // queries pending in one readyRead notification. Read beyond the first hit.
    for (int i = 0; i < 80; ++i)
        check(sender.writeDatagram(i == 0 ? query : noise, host, socket->localPort()) > 0,
              "discovery query burst sent");
    check(until([&] { return !socket->hasPendingDatagrams(); }, 500), "discovery drains the entire burst after its first matching query");
    for (int i = 0; i < 80; ++i)
        sender.writeDatagram(i == 79 ? query : noise, host, socket->localPort());
    check(until([&] { return !socket->hasPendingDatagrams(); }, 500), "discovery continues beyond its per-event work limit");
    sender.writeDatagram(query, host, socket->localPort());
    check(until([&] { return !socket->hasPendingDatagrams(); }, 500), "subsequent phone discovery still drains without restarting reception");
    }
}

void ipv6SessionTests() {
    AudioBuffer buffer;
    Receiver receiver(buffer, nullptr, std::make_unique<TestReference>()); quint16 port = 0;
    QObject::connect(&receiver, &Receiver::listening, [&](quint16 value) { port = value; });
    receiver.startListening(0); check(port != 0, "dual-stack listener starts");
    const auto code = receiver.pairingCode();
    QTcpSocket owner, other; connectTo(owner, port, QHostAddress::LocalHostIPv6); connectTo(other, port);
    auto welcome = rpc(owner, connectRequest(QString(32, 'a'), QString(32, 'b'), code));
    check(welcome["type"] == "welcome", "IPv6 code pairing succeeds");
    check(rpc(other, connectRequest(QString(32, 'c'), QString(32, 'd'), code))["reason"] == "busy", "IPv4 cannot steal IPv6 ownership");
    auto key = QByteArray::fromHex(welcome["key"].toString().toLatin1());
    QUdpSocket audio; check(audio.bind(QHostAddress::LocalHostIPv6, 0), "IPv6 audio bound");
    Packet packet; packet.type = PacketType::Audio; packet.session = welcome["session"].toString().toUInt(); packet.payload.resize(MaxPayload);
    audio.writeDatagram(Receiver::authenticate(packet, key), QHostAddress::LocalHostIPv6, port);
    check(until([&] { return buffer.stats().accepted == 1; }), "authenticated microphone audio arrives over IPv6");
    check(rpc(owner, {{"type", "reference"}, {"enabled", "1"}})["state"] == "starting", "IPv6 reference requested");
    check(until([&] { return audio.hasPendingDatagrams(); }), "PC returns reference over IPv6");
    const auto reference = audio.receiveDatagram().data();
    check(reference.size() == 1040 && reference.first(4) == "MREF" &&
          reference.last(32) == QMessageAuthenticationCode::hash(reference.first(1008), key, QCryptographicHash::Sha256), "IPv6 reference authenticates");
    const auto token = welcome["token"];
    owner.abort(); QCoreApplication::processEvents();
    QTcpSocket resumed; connectTo(resumed, port, QHostAddress::LocalHostIPv6);
    welcome = rpc(resumed, {{"type", "resume"}, {"clientId", QString(32, 'a')}, {"token", token}, {"nonce", QString(32, 'e')}});
    check(welcome["type"] == "welcome" && receiver.pairingCode() == code, "IPv6 owner resumes without rotating code");
    check(rpc(resumed, {{"type", "disconnect"}, {"clientId", QString(32, 'a')}, {"token", token}})["type"] == "disconnected", "IPv6 disconnect acknowledged");
    check(rpc(other, connectRequest(QString(32, 'c'), QString(32, 'd'), code))["type"] == "welcome", "IPv4 can pair after IPv6 owner releases");
    receiver.stopListening();
    QByteArray query(12, '\0'); query[5] = 1;
    query += QByteArray("\x01x\0\0\x1c\0\x01", 7);
    check(LanAdvertiser::relevantQuery(query, "instance", "x"), "mDNS responds to AAAA queries");
}
void repeatedOwnerReleaseTests() {
    AudioBuffer buffer; Receiver receiver(buffer); quint16 port = 0; bool locked = false;
    QObject::connect(&receiver, &Receiver::listening, [&](quint16 value) { port = value; });
    QObject::connect(&receiver, &Receiver::connectionChanged, [&](bool value, bool) { locked = value; });
    receiver.startListening(0);
    std::vector<std::unique_ptr<QTcpSocket>> phones;
    std::vector<std::unique_ptr<QTcpSocket>> pendingProbes;
    for (int i = 0; i < 6; ++i) {
        auto probe = std::make_unique<QTcpSocket>(); connectTo(*probe, port);
        pendingProbes.push_back(std::move(probe));
    }
    for (int attempt = 0; attempt < 24; ++attempt) {
        // Exercise slot pressure without violating the existing 12/s pairing
        // rate limit. Idle discovery probes still occupy the other slots.
        if (attempt && attempt % 10 == 0) until([] { return false; }, 1050);
        auto phone = std::make_unique<QTcpSocket>(); connectTo(*phone, port);
        const QString digit = QString::number(attempt % 16, 16);
        const auto welcome = rpc(*phone, connectRequest(digit.repeated(32), QString(32, 'e'), receiver.pairingCode()));
        check(welcome["type"] == "welcome" && locked, "code pairing remains available across repeated PC releases");
        if (attempt % 3 == 0) {
            phone->abort();
            check(until([&] { return !phone->isOpen(); }), "killed phone closes its local transport");
        }
        const auto code = receiver.pairingCode(); receiver.manualDisconnect();
        check(!locked && receiver.pairingCode() == code, "PC release clears a live or killed owner's reservation without changing the code");
        if (attempt % 4 == 0) receiver.refreshPairingCode();
        // Keep unacknowledged FIN sockets alive to cover immediate reconnects
        // while their bounded shutdown timers are still pending.
        phones.push_back(std::move(phone));
    }
    QTcpSocket current; connectTo(current, port);
    check(rpc(current, connectRequest(QString(32, 'f'), QString(32, 'a'), receiver.pairingCode()))["type"] == "welcome",
          "a new owner pairs after control-slot pressure without stopping reception");
    check(until([&] {
        return std::all_of(phones.begin(), phones.end(), [](const auto& phone) {
            return phone->state() == QAbstractSocket::UnconnectedState;
        });
    }, 400), "retired controls are eventually reclaimed under repeated reconnects");
    check(locked && rpc(current, {{"type", "ping"}})["type"] == "pong", "retired timers cannot close the current owner after control-slot reclamation");
    receiver.stopListening();
}
void coldOwnerRecoveryTests() {
    AudioBuffer buffer; Receiver receiver(buffer); quint16 port = 0; bool locked = false;
    QObject::connect(&receiver, &Receiver::listening, [&](quint16 value) { port = value; });
    QObject::connect(&receiver, &Receiver::connectionChanged, [&](bool value, bool) { locked = value; });
    receiver.startListening(0); const auto code = receiver.pairingCode();
    QTcpSocket original; connectTo(original, port);
    const auto first = rpc(original, connectRequest(QString(32, 'a'), QString(32, 'b'), code));
    original.abort(); until([] { return false; }, 10);
    check(locked, "process loss preserves the first owner's reservation");
    QTcpSocket restarted; connectTo(restarted, port);
    auto request = connectRequest(QString(32, 'a'), QString(32, 'c'), code);
    request.insert("token", QString(32, 'd'));
    check(rpc(restarted, request)["reason"] == "busy", "a persistent identity alone cannot reclaim ownership");
    request.insert("token", first["token"]);
    request.insert("code", code == "000000" ? "000001" : "000000");
    check(rpc(restarted, request)["reason"] == "wrong_code", "cold recovery validates the code before using a saved lease");
    request.insert("code", code);
    const auto recovered = rpc(restarted, request);
    check(recovered["type"] == "welcome" && recovered["session"] != first["session"] && recovered["key"] != first["key"],
          "a restarted phone with its saved token obtains a fresh audio session");
    QTcpSocket stranger; connectTo(stranger, port);
    request.insert("clientId", QString(32, 'e'));
    check(rpc(stranger, request)["reason"] == "busy", "cold recovery cannot transfer a saved lease to a different identity");
    check(rpc(restarted, {{"type", "ping"}})["type"] == "pong" && receiver.pairingCode() == code,
          "cold recovery preserves the active owner and pairing code");
    receiver.stopListening();
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); AudioBuffer buffer;
    const bool waitingFixture = argc > 1 && QString::fromLocal8Bit(argv[1]) == "--serve-reference-waiting";
    const bool referenceFixture = waitingFixture || (argc > 1 && (QString::fromLocal8Bit(argv[1]) == "--serve-reference" || QString::fromLocal8Bit(argv[1]) == "--python-reference-sender" || QString::fromLocal8Bit(argv[1]) == "--python-ipv6-reference-sender"));
    std::unique_ptr<AudioReference> reference;
    if (referenceFixture) {
        auto source = std::make_unique<TestReference>(); source->available = !waitingFixture;
        reference = std::move(source);
    }
    Receiver receiver(buffer, nullptr, std::move(reference));
    quint16 port = 0; bool locked = false;
    QObject::connect(&receiver, &Receiver::listening, [&](quint16 p) { port = p; });
    QObject::connect(&receiver, &Receiver::connectionChanged, [&](bool owner, bool) { locked = owner; });
    receiver.startListening(0); check(port != 0, "TCP and UDP bound on same port");
    const QString code = receiver.pairingCode();
    if (argc == 2 && (QString::fromLocal8Bit(argv[1]) == "--serve" || referenceFixture)) {
        std::cout << port << ' ' << code.toStdString() << std::endl;
        QTimer::singleShot(60000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    check(code.size() == 6 && std::all_of(code.begin(), code.end(), [](QChar c) { return c >= '0' && c <= '9'; }), "exactly six decimal digits");
    const auto mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (argc == 4 && (mode == "--python-sender" || mode == "--python-reference-sender" || mode == "--python-ipv6-sender" || mode == "--python-ipv6-reference-sender")) {
        QProcess sender;
        QStringList arguments{QString::fromLocal8Bit(argv[3]), mode.contains("ipv6") ? "::1" : "127.0.0.1", "--port", QString::number(port), "--code", code, "--seconds", "0.3"};
        if (referenceFixture) arguments << "--reference";
        sender.start(QString::fromLocal8Bit(argv[2]), arguments);
        check(sender.waitForStarted(2000), "Python starts"); std::uint64_t maximum = 0;
        check(until([&] { maximum = std::max(maximum, buffer.stats().accepted); return sender.state() == QProcess::NotRunning; }, 8000), "Python finishes");
        if (sender.exitCode()) std::cerr << sender.readAllStandardError().constData();
        const auto stdoutBytes = sender.readAllStandardOutput();
        if (referenceFixture) check(stdoutBytes.contains("Authenticated reference packets:") && !stdoutBytes.contains("Authenticated reference packets: 0"), "Python validates reference packets");
        check(sender.exitCode() == 0 && maximum >= 10, "Python pairs by six-digit code and streams v2 audio");
        check(!locked && receiver.pairingCode() == code, "Python explicit disconnect preserves code");
        receiver.stopListening(); std::cout << "Python interop passed\n"; return 0;
    }
    const QString idA(32, 'a'), nonceA(32, 'b'), idB(32, 'c'), nonceB(32, 'd');
    QTcpSocket first, other; connectTo(first, port); connectTo(other, port);
    const auto wrong = code == "000000" ? "000001" : "000000";
    check(rpc(first, connectRequest(idA, nonceA, wrong))["reason"] == "wrong_code", "wrong code rejected");
    check(!locked, "wrong code does not claim PC");
    auto welcome = rpc(first, connectRequest(idA, nonceA, code));
    check(welcome["type"] == "welcome" && locked, "first matching phone claims PC");
    receiver.startListening(port);
    check(locked && receiver.pairingCode() == code, "repeated start preserves owner and pairing code");
    check(rpc(first, connectRequest(idA, nonceA, code))["session"] == welcome["session"], "retry is idempotent");
    check(rpc(other, connectRequest(idB, nonceB, code))["reason"] == "busy", "second code connection rejected");
    check(rpc(other, connectRequest(idB, nonceB))["reason"] == "busy", "second manual IP connection rejected");
    const auto token = welcome["token"].toString(); first.abort();
    until([] { return false; }, 3400);
    check(locked && receiver.pairingCode() == code, "network loss/timeout preserves owner and code");
    check(rpc(other, connectRequest(idB, nonceB))["reason"] == "busy", "other phone still rejected after timeout");
    QTcpSocket resumed; connectTo(resumed, port);
    check(rpc(resumed, {{"type", "resume"}, {"clientId", idA}, {"token", QString(32, 'f')}, {"nonce", nonceB}})["reason"] == "not_owner", "invalid resume credential rejected");
    welcome = rpc(resumed, {{"type", "resume"}, {"clientId", idA}, {"token", token}, {"nonce", nonceB}});
    check(welcome["type"] == "welcome" && receiver.pairingCode() == code, "original owner resumes without code rotation");
    const auto key = QByteArray::fromHex(welcome["key"].toString().toLatin1());
    const auto session = welcome["session"].toString().toUInt();
    QUdpSocket audio;
    Packet packet; packet.type = PacketType::Audio; packet.session = session; packet.payload.resize(MaxPayload);
    for (unsigned seq : {0U, 2U, 1U, 3U}) { packet.sequence = seq; packet.timestamp = seq * FramesPerPacket;
        audio.writeDatagram(Receiver::authenticate(packet, key), QHostAddress::LocalHost, port); }
    check(until([&] { return buffer.stats().accepted == 4; }), "v2 authenticated audio accepted and reordered");
    auto tampered = Receiver::authenticate(packet, key); tampered[tampered.size() - 1] = static_cast<char>(tampered.back() ^ 1);
    audio.writeDatagram(tampered, QHostAddress::LocalHost, port);
    Packet bye; bye.type = PacketType::Bye; bye.session = session;
    audio.writeDatagram(Receiver::authenticate(bye, key), QHostAddress::LocalHost, port);
    until([] { return false; }, 50);
    check(locked && buffer.stats().accepted == 4, "UDP BYE and invalid audio cannot release owner");
    auto released = rpc(resumed, {{"type", "disconnect"}, {"clientId", idA}, {"token", token}});
    check(released["type"] == "disconnected" && !locked && receiver.pairingCode() == code, "phone disconnect releases ownership and preserves code");
    check(rpc(other, connectRequest(idB, nonceB, code))["type"] == "welcome", "same code can pair after release");
    receiver.refreshPairingCode();
    const auto nextCode = receiver.pairingCode();
    check(nextCode != code && locked, "manual refresh changes code without releasing owner");
    check(rpc(other, {{"type", "ping"}})["type"] == "pong", "manual refresh preserves active session");
    QTcpSocket stranger; connectTo(stranger, port);
    check(rpc(stranger, connectRequest(idA, nonceA, code))["reason"] == "wrong_code", "manual refresh invalidates previous code");
    check(rpc(stranger, connectRequest(idA, nonceA, nextCode))["reason"] == "busy", "manual refresh preserves exclusive ownership");
    receiver.manualDisconnect();
    check(!locked && receiver.pairingCode() == nextCode, "PC disconnect preserves code too");
    QTcpSocket manual; connectTo(manual, port);
    check(rpc(manual, connectRequest(idA, nonceA))["type"] == "welcome", "manual IP pairs without code after release");
    receiver.manualDisconnect();
    receiver.stopListening();
    const auto stoppedCode = receiver.pairingCode();
    check(stoppedCode != nextCode, "stopping reception rotates code");
    receiver.stopListening();
    check(receiver.pairingCode() == stoppedCode, "repeated stop does not rotate an already stopped receiver");
    receiver.startListening(0);
    check(receiver.pairingCode() == stoppedCode, "starting reception preserves stopped code");
    {
        Receiver restored(buffer, nullptr, {}, QStringLiteral("000007"));
        check(restored.pairingCode() == QStringLiteral("000007"), "persisted six-digit code including leading zeros is restored");
    }
    QByteArray query(12, '\0'); query[5] = 1;
    query += QByteArray("\x08_minemic\x04_tcp\x05local\0\0\x0c\0\x01", 25);
    check(LanAdvertiser::relevantQuery(query, "x", "y"), "Bonjour browse query recognized");
    QByteArray malicious(12, '\0'); malicious[5] = 1; malicious += QByteArray::fromHex("c00c000c0001");
    check(!LanAdvertiser::relevantQuery(malicious, "x", "y"), "DNS compression loop bounded");
    receiver.stopListening(); referenceTests(); referenceTimingTests(); disconnectHandshakeTests(); discoveryBurstTests(); repeatedOwnerReleaseTests(); coldOwnerRecoveryTests(); ipv6SessionTests();
    std::cout << "Pairing, first-owner lock, resume, rotation, audio, reference and DNS tests passed\n";
}
