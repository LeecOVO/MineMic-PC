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
LanAdvertiser::LanAdvertiser(QObject* parent) : QObject(parent), socket_(this), socket6_(this), timer_(this) {
    socket_.setObjectName(QStringLiteral("mdns-ipv4")); socket6_.setObjectName(QStringLiteral("mdns-ipv6"));
    const auto id = QByteArray::number(QRandomGenerator::system()->generate64(), 16);
    instance_ = "MineMic-" + id + "." + Service; host_ = "minemic-" + id + ".local";
    connect(&timer_, &QTimer::timeout, this, [this] { announce(); });
    connect(&socket_, &QUdpSocket::readyRead, this, [this] {
        if (!drainScheduled_) readPending(socket_, drainScheduled_);
    });
    connect(&socket6_, &QUdpSocket::readyRead, this, [this] {
        if (!drain6Scheduled_) readPending(socket6_, drain6Scheduled_);
    });
}
void LanAdvertiser::readPending(QUdpSocket& socket, bool& scheduled) {
    scheduled = false;
    for (int count = 0; count < 32 && socket.hasPendingDatagrams(); ++count) {
        const auto datagram = socket.receiveDatagram(2048);
        if (relevantQuery(datagram.data(), instance_, host_) && (!responseTimer_.isValid() || responseTimer_.elapsed() >= 250)) {
            // Rate-limit replies, never reads: leaving an unread datagram can
            // prevent a subsequent readyRead on some socket backends.
            announce(); responseTimer_.restart();
        }
    }
    if (socket.hasPendingDatagrams()) {
        scheduled = true;
        QMetaObject::invokeMethod(this, [this, &socket, &scheduled] { readPending(socket, scheduled); }, Qt::QueuedConnection);
    }
}
LanAdvertiser::~LanAdvertiser() { if (port_) announce(0); }
void LanAdvertiser::start(quint16 port) {
    const auto flags = QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
    const bool ipv4 = socket_.bind(QHostAddress::AnyIPv4, 5353, flags);
    const bool ipv6 = socket6_.bind(QHostAddress::AnyIPv6, 5353, flags);
    if (!ipv4 && !ipv6) {
        emit warning(tr("自动发现不可用，请在手机上手动输入 PC 地址。")); return;
    }
    socket_.setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
    socket6_.setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp) || interface.flags() & QNetworkInterface::IsLoopBack ||
            !(interface.flags() & QNetworkInterface::CanMulticast)) continue;
        bool usable = false, usable6 = false;
        for (const auto& entry : interface.addressEntries()) {
            usable |= entry.ip().protocol() == QAbstractSocket::IPv4Protocol;
            usable6 |= entry.ip().protocol() == QAbstractSocket::IPv6Protocol;
        }
        if (ipv4 && usable && socket_.joinMulticastGroup(QHostAddress("224.0.0.251"), interface)) interfaces_ += interface;
        if (ipv6 && usable6 && socket6_.joinMulticastGroup(QHostAddress("ff02::fb"), interface)) interfaces6_ += interface;
    }
    if (interfaces_.isEmpty() && interfaces6_.isEmpty()) { emit warning(tr("未找到可用的局域网发现接口，可使用手动 IP 连接。")); return; }
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
            (type == 12 || type == 33 || type == 16 || type == 1 || type == 28 || type == 255)) return true;
    }
    return false;
}
QByteArray LanAdvertiser::records(quint32 ttl, const QNetworkInterface& interface) const {
    QList<QHostAddress> addresses;
    for (const auto& entry : interface.addressEntries()) {
        const auto address = entry.ip();
        if (!address.isNull() && !address.isLoopback() && !address.isMulticast() && addresses.size() < 8 &&
            (address.protocol() == QAbstractSocket::IPv4Protocol || address.protocol() == QAbstractSocket::IPv6Protocol))
            addresses += address;
    }
    QByteArray out; u16(out, 0); u16(out, 0x8400); u16(out, 0);
    u16(out, static_cast<quint16>(4 + addresses.size())); u16(out, 0); u16(out, 0);
    record(out, "_services._dns-sd._udp.local", 12, ttl, name(Service));
    record(out, Service, 12, ttl, name(instance_));
    QByteArray srv; u16(srv, 0); u16(srv, 0); u16(srv, port_); srv += name(host_);
    record(out, instance_, 33, ttl, srv);
    QByteArray txt("\x03v=2", 4); int index = 0;
    // Android 7–13 NSD exposes just one resolved address. These optional hints
    // keep IPv6 selectable there; newer clients use standard AAAA records.
    for (auto address : addresses) if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        address.setScopeId({});
        const auto hint = "ip6-" + QByteArray::number(index++) + '=' + address.toString().toLatin1();
        txt.append(static_cast<char>(hint.size())); txt += hint;
    }
    record(out, instance_, 16, ttl, txt);
    for (const auto& address : addresses) {
        QByteArray ip;
        if (address.protocol() == QAbstractSocket::IPv4Protocol) { u32(ip, address.toIPv4Address()); record(out, host_, 1, ttl, ip); }
        else { const auto bytes = address.toIPv6Address(); ip.append(reinterpret_cast<const char*>(bytes.c), 16); record(out, host_, 28, ttl, ip); }
    }
    return out;
}
void LanAdvertiser::announce(quint32 ttl) {
    if (!port_) return;
    for (const auto& interface : interfaces_) {
        socket_.setMulticastInterface(interface); socket_.writeDatagram(records(ttl, interface), QHostAddress("224.0.0.251"), 5353);
    }
    for (const auto& interface : interfaces6_) {
        QHostAddress group("ff02::fb"); group.setScopeId(interface.name());
        socket6_.setMulticastInterface(interface); socket6_.writeDatagram(records(ttl, interface), group, 5353);
    }
}
}
