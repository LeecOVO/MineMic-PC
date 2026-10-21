#include "network/Receiver.h"
#include "network/LanAdvertiser.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QThread>
#include <QTimer>
#include <functional>
#include <iostream>
#include <cstdlib>
#include <algorithm>
using namespace minemic;
namespace {
void check(bool pass, const char* message) { if (!pass) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); } }
bool until(const std::function<bool()>& f, int ms = 1800) {
    QElapsedTimer timer; timer.start();
    do { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); if (f()) return true; QThread::msleep(1); } while (timer.elapsed() < ms);
    return false;
}
void connectTo(QTcpSocket& socket, quint16 port) {
    socket.connectToHost(QHostAddress::LocalHost, port);
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
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); AudioBuffer buffer; Receiver receiver(buffer);
    quint16 port = 0; bool locked = false;
    QObject::connect(&receiver, &Receiver::listening, [&](quint16 p) { port = p; });
    QObject::connect(&receiver, &Receiver::connectionChanged, [&](bool owner, bool) { locked = owner; });
    receiver.startListening(0); check(port != 0, "TCP and UDP bound on same port");
    const QString code = receiver.pairingCode();
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == "--serve") {
        std::cout << port << ' ' << code.toStdString() << std::endl;
        QTimer::singleShot(60000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    check(code.size() == 6 && std::all_of(code.begin(), code.end(), [](QChar c) { return c >= '0' && c <= '9'; }), "exactly six decimal digits");
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--python-sender") {
        QProcess sender;
        sender.start(QString::fromLocal8Bit(argv[2]), {QString::fromLocal8Bit(argv[3]), "127.0.0.1", "--port", QString::number(port), "--code", code, "--seconds", "0.3"});
        check(sender.waitForStarted(2000), "Python starts"); std::uint64_t maximum = 0;
        check(until([&] { maximum = std::max(maximum, buffer.stats().accepted); return sender.state() == QProcess::NotRunning; }, 8000), "Python finishes");
        if (sender.exitCode()) std::cerr << sender.readAllStandardError().constData();
        check(sender.exitCode() == 0 && maximum >= 10, "Python pairs by six-digit code and streams v2 audio");
        check(!locked && receiver.pairingCode() != code, "Python explicit disconnect rotates code");
        receiver.stopListening(); std::cout << "Python interop passed\n"; return 0;
    }
    const QString idA(32, 'a'), nonceA(32, 'b'), idB(32, 'c'), nonceB(32, 'd');
    QTcpSocket first, other; connectTo(first, port); connectTo(other, port);
    const auto wrong = code == "000000" ? "000001" : "000000";
    check(rpc(first, connectRequest(idA, nonceA, wrong))["reason"] == "wrong_code", "wrong code rejected");
    check(!locked, "wrong code does not claim PC");
    auto welcome = rpc(first, connectRequest(idA, nonceA, code));
    check(welcome["type"] == "welcome" && locked, "first matching phone claims PC");
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
    check(released["type"] == "disconnected" && !locked && receiver.pairingCode() != code, "phone manual disconnect releases and rotates code");
    check(rpc(other, connectRequest(idB, nonceB, code))["reason"] == "wrong_code", "old pairing code expires");
    const auto nextCode = receiver.pairingCode();
    check(rpc(other, connectRequest(idB, nonceB))["type"] == "welcome", "manual IP pairs with no code when available");
    receiver.manualDisconnect();
    check(!locked && receiver.pairingCode() != nextCode, "PC manual disconnect rotates code too");
    QByteArray query(12, '\0'); query[5] = 1;
    query += QByteArray("\x08_minemic\x04_tcp\x05local\0\0\x0c\0\x01", 25);
    check(LanAdvertiser::relevantQuery(query, "x", "y"), "Bonjour browse query recognized");
    QByteArray malicious(12, '\0'); malicious[5] = 1; malicious += QByteArray::fromHex("c00c000c0001");
    check(!LanAdvertiser::relevantQuery(malicious, "x", "y"), "DNS compression loop bounded");
    receiver.stopListening(); std::cout << "Pairing, first-owner lock, resume, rotation, audio and DNS tests passed\n";
}
