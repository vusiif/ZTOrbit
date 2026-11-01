#pragma once
#include <ztprotocol/PluginLogger.h>
#include <ztprotocol/PluginFiles.h>
#include <ztprotocol/NetworkData.h>
#include <ztprotocol/NetworkService.h>
#include <ztprotocol/NetworkDelivery.h>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <functional>

namespace vusiif::ztohub::api {
using ConnectionId = quint64;
struct Endpoint { QHostAddress address; quint16 port = 0; };
struct JsonResult {
    QJsonDocument document;
    QString error;
    bool ok() const { return error.isEmpty() && !document.isNull(); }
};
using JsonCallback = std::function<void(const JsonResult &)>;
class ZeroTierQueries {
public:
    virtual ~ZeroTierQueries() = default;
    // Event-thread only. Destroying context or stopping the plugin cancels requests.
    // Operations: info, peers, networks, moons, controller.networks, controller.members.
    virtual void query(const QString &operation, const QJsonObject &parameters,
                       QObject *context, JsonCallback completed) = 0;
};
// Borrowed through onStop. Files/logs are thread-safe; other methods are event-thread only.
class HostServices {
public:
    virtual ~HostServices() = default;
    virtual vusiif::zto::util::PluginFiles &files() = 0;
    virtual vusiif::zto::util::PluginLogger &logs() = 0;
    virtual ZeroTierQueries &zeroTier() = 0;
    virtual quint16 port() const = 0;
    virtual QHostAddress peerAddress(ConnectionId connection) const = 0;
    // All sends are automatically wrapped with this plugin's registered route ID.
    virtual bool sendTcp(ConnectionId connection, const QByteArray &payload) = 0;
    virtual bool sendUdp(const Endpoint &endpoint, const QByteArray &payload) = 0;
    virtual void disconnect(ConnectionId connection) = 0;
    // Pending bytes on the event thread; -1 means the connection no longer exists.
    virtual qint64 queuedTcpBytes(ConnectionId connection) const = 0;
    virtual void sendDataTo(const QString &networkId, const QString &memberId, const QByteArray &data,
                            QObject *context, vusiif::zto::util::SendCallback completed) = 0;
    virtual void broadcastDataTo(const QString &networkId, const QStringList &memberIds, const QByteArray &data,
                                 QObject *context, vusiif::zto::util::SendCallback completed) = 0;
    virtual void broadcastDataBalancedTo(const QString &networkId, const QStringList &memberIds, const QByteArray &data,
                                         QObject *context, vusiif::zto::util::SendCallback completed) = 0;
    virtual void getNetworkRelays(const QString &networkId, QObject *context, vusiif::zto::util::RelayCallback completed) = 0;
    virtual vusiif::zto::util::NetworkSubscription *subscribeNetworkData(const QString &networkId, QObject *context, vusiif::zto::util::NetworkCallback received) = 0;
};
}
