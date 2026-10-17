#pragma once
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QPluginLoader>
#include <ztserver/PluginInterface.h>
#include "ZeroTierQueries.h"
#include <memory>
#include <vector>
#include <future>
#include "NetworkTransport.h"
#include "PluginPackage.h"
#include <QLockFile>
namespace vusiif::ztgui::server {
class PluginServices;
class ServerHost final : public QObject {
public:
    explicit ServerHost(QString directory);
    ~ServerHost() override;
    bool start(const QHostAddress &, quint16);
    void stop();
    network::NetworkTransport &transport() { return m_transport; }
    quint16 port() const { return m_tcp.serverPort(); }
    QHostAddress peerAddress(api::ConnectionId) const;
    bool sendTcp(const QString &, api::ConnectionId, const QByteArray &);
    bool sendUdp(const QString &, const api::Endpoint &, const QByteArray &);
    void disconnect(api::ConnectionId);
    qint64 queuedTcpBytes(api::ConnectionId) const;
private:
    struct Connection { QTcpSocket *socket; QByteArray input; QSet<QString> routes; int requests = 0; qint64 window = 0, lastSeen = 0; };
    struct Plugin {
        std::unique_ptr<QPluginLoader> library;
        api::PluginInterface *entry = nullptr;
        std::unique_ptr<PluginServices> services;
        api::PluginInfo info;
        util::CancellationSource cancellation;
        std::future<api::StartResult> task;
        bool ready = false;
        std::unique_ptr<QTemporaryDir> cache;
        std::unique_ptr<QLockFile> cacheLock;
    };
    QString m_directory;
    QTcpServer m_tcp;
    QUdpSocket m_udp;
    QMap<api::ConnectionId, Connection> m_connections;
    std::vector<Plugin> m_plugins;
    api::ConnectionId m_next = 1;
    ZeroTierQueries m_queries;
    network::NetworkTransport m_transport{true};
    QString m_node;
    bool m_networkPollPending = false;
    qint64 m_networkPolledAt = 0;
    QTimer m_timer;
    bool m_stopping = false;
    void scan();
    void accept();
    void read(api::ConnectionId);
    void datagrams();
    void builtin(api::ConnectionId, const QByteArray &);
    Plugin *plugin(const QString &);
    void poll();
};
}
