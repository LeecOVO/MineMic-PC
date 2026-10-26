#pragma once
#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QNetworkInterface>
namespace minemic {
// Small DNS-SD/mDNS responder for _minemic._tcp.local. No Bonjour DLL on Windows.
class LanAdvertiser final : public QObject {
    Q_OBJECT
public:
    explicit LanAdvertiser(QObject* parent = nullptr);
    ~LanAdvertiser() override;
    void start(quint16 port);
    static bool relevantQuery(const QByteArray& bytes, const QByteArray& instance, const QByteArray& host);
signals:
    void warning(const QString& message);
private:
    void readPending(QUdpSocket& socket, bool& scheduled);
    void announce(quint32 ttl = 120);
    QByteArray records(quint32 ttl, const QNetworkInterface& interface) const;
    QUdpSocket socket_;
    QUdpSocket socket6_;
    QTimer timer_;
    QElapsedTimer responseTimer_;
    QList<QNetworkInterface> interfaces_;
    QList<QNetworkInterface> interfaces6_;
    QByteArray instance_, host_;
    quint16 port_ = 0;
    bool drainScheduled_ = false, drain6Scheduled_ = false;
};
}
