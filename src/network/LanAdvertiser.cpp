#include "network/LanAdvertiser.h"
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QVariant>
#include <QtEndian>
namespace minemic {
namespace {
const QByteArray Service = "_minemic._tcp.local";
void u16(QByteArray& out, quint16 value) { char b[2]; qToBigEndian(value, b); out.append(b, 2); }
void u32(QByteArray& out, quint32 value) { char b[4]; qToBigEndian(value, b); out.append(b, 4); }
QByteArray name(const QByteArray& value) {
    QByteArray out;
    for (const auto& label : value.split('.')) { out.append(static_cast<char>(label.size())); out.append(label); }
    out.append('\0'); return out;
}
void record(QByteArray& out, const QByteArray& owner, quint16 type, quint32 ttl, const QByteArray& data) {
    out += name(owner); u16(out, type); u16(out, type == 12 ? 1 : 0x8001); u32(out, ttl);
    u16(out, static_cast<quint16>(data.size())); out += data;
}
bool readName(const QByteArray& bytes, qsizetype& offset, QByteArray& value) {
    auto cursor = offset; bool jumped = false; int steps = 0; value.clear();
    while (cursor < bytes.size() && ++steps <= 64) {
        const auto length = static_cast<unsigned char>(bytes[cursor++]);
        if (!length) { if (!jumped) offset = cursor; return true; }
        if ((length & 0xc0) == 0xc0) {
            if (cursor >= bytes.size()) return false;
            const int target = ((length & 0x3f) << 8) | static_cast<unsigned char>(bytes[cursor++]);
            if (!jumped) offset = cursor;
            jumped = true; cursor = target; continue;
        }
        if (length > 63 || cursor + length > bytes.size() || value.size() + length + 1 > 255) return false;
        if (!value.isEmpty()) value += '.';
        value += bytes.mid(cursor, length); cursor += length;
    }
    return false;
}
}
LanAdvertiser::LanAdvertiser(QObject* parent) : QObject(parent) {
    const auto id = QByteArray::number(QRandomGenerator::system()->generate64(), 16);
    instance_ = "MineMic-" + id + "." + Service; host_ = "minemic-" + id + ".local";
    connect(&timer_, &QTimer::timeout, this, [this] { announce(); });
    connect(&socket_, &QUdpSocket::readyRead, this, [this] {
        int count = 0;
        while (socket_.hasPendingDatagrams() && ++count <= 32) {
            const auto datagram = socket_.receiveDatagram(2048);
            if (relevantQuery(datagram.data(), instance_, host_) && (!responseTimer_.isValid() || responseTimer_.elapsed() >= 250)) {
                // Multicast answers are valid for normal mDNS and QU browse requests.
                announce(); responseTimer_.restart();
                break;
            }
        }
    });
}
LanAdvertiser::~LanAdvertiser() { if (port_) announce(0); }
void LanAdvertiser::start(quint16 port) {
    if (!socket_.bind(QHostAddress::AnyIPv4, 5353, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit warning(tr("自动发现不可用，请在手机上手动输入 PC 地址。")); return;
    }
    socket_.setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp) || interface.flags() & QNetworkInterface::IsLoopBack ||
            !(interface.flags() & QNetworkInterface::CanMulticast)) continue;
        bool usable = false;
        for (const auto& entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && addresses_.size() < 8) {
                addresses_ += entry.ip(); usable = true;
            }
        }
        if (usable && socket_.joinMulticastGroup(QHostAddress("224.0.0.251"), interface)) interfaces_ += interface;
    }
    if (interfaces_.isEmpty()) { emit warning(tr("未找到可用的局域网发现接口，可使用手动 IP 连接。")); return; }
    port_ = port; announce();
    QTimer::singleShot(1000, this, [this] { announce(); });
    timer_.start(60000);
}
bool LanAdvertiser::relevantQuery(const QByteArray& bytes, const QByteArray& instance, const QByteArray& host) {
    if (bytes.size() < 12 || (static_cast<unsigned char>(bytes[2]) & 0x80)) return false;
    const auto count = qFromBigEndian<quint16>(bytes.constData() + 4);
    if (!count || count > 32) return false;
    qsizetype offset = 12;
    for (unsigned i = 0; i < count; ++i) {
        QByteArray value;
        if (!readName(bytes, offset, value) || offset + 4 > bytes.size()) return false;
        const auto type = qFromBigEndian<quint16>(bytes.constData() + offset); offset += 4;
        value = value.toLower();
        if ((value == Service || value == instance.toLower() || value == host.toLower() || value == "_services._dns-sd._udp.local") &&
            (type == 12 || type == 33 || type == 16 || type == 1 || type == 255)) return true;
    }
    return false;
}
QByteArray LanAdvertiser::records(quint32 ttl) const {
    QByteArray out; u16(out, 0); u16(out, 0x8400); u16(out, 0);
    u16(out, static_cast<quint16>(4 + addresses_.size())); u16(out, 0); u16(out, 0);
    record(out, "_services._dns-sd._udp.local", 12, ttl, name(Service));
    record(out, Service, 12, ttl, name(instance_));
    QByteArray srv; u16(srv, 0); u16(srv, 0); u16(srv, port_); srv += name(host_);
    record(out, instance_, 33, ttl, srv);
    record(out, instance_, 16, ttl, QByteArray("\x03v=2", 4));
    for (const auto& address : addresses_) { QByteArray ip; u32(ip, address.toIPv4Address()); record(out, host_, 1, ttl, ip); }
    return out;
}
void LanAdvertiser::announce(quint32 ttl) {
    if (!port_) return;
    const auto bytes = records(ttl);
    for (const auto& interface : interfaces_) {
        socket_.setMulticastInterface(interface); socket_.writeDatagram(bytes, QHostAddress("224.0.0.251"), 5353);
    }
}
}
