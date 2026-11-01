#pragma once
#include <ztprotocol/NetworkService.h>
#include <QJsonDocument>
#include <QHostAddress>
#include <QThread>
#include <QPointer>

namespace vusiif::zto::network {
// Host implementation shared by both applications; never exported as SDK API.
class TransportWorker;
class NetworkTransport final : public QObject {
    Q_OBJECT
public:
    explicit NetworkTransport(bool server = false, QObject *parent = nullptr);
    ~NetworkTransport() override;
    void setNetworks(const QString &nodeId, const QJsonArray &networks);
    void setRelayConfig(const QJsonObject &config);
    void send(const QString &plugin, const QString &network, const QStringList &members, const QByteArray &data,
              bool balanced, QObject *context, util::SendCallback callback);
    void relays(const QString &network, QObject *context, util::RelayCallback callback);
    util::NetworkSubscription *subscribe(const QString &plugin, const QString &network, QObject *context, util::NetworkCallback callback);
    void probeMember(const QString &network, const QString &member, QObject *context, std::function<void(double)> callback);
    void builtin(const QHostAddress &source, const QJsonObject &request, QObject *context,
                 std::function<void(const QJsonObject &)> callback);
    void datagram(const QHostAddress &source, quint16 port, const QByteArray &payload);
signals:
    void received(const QString &plugin, const vusiif::zto::util::NetworkMessage &message);
private:
    QThread m_thread;
    TransportWorker *m_worker;
};
}
