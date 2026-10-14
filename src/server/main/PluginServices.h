#pragma once
#include <ztserver/HostServices.h>
#include "ZeroTierQueries.h"
#include <QFile>
namespace vusiif::ztgui::server {
class ServerHost;
class PluginServices final : public QObject, public api::HostServices, public util::PluginFiles, public util::PluginLogger {
public:
    PluginServices(ServerHost &host, const QString &id, const QString &name, const QString &root);
    util::PluginFiles &files() override { return *this; }
    util::PluginLogger &logs() override { return *this; }
    api::ZeroTierQueries &zeroTier() override { return m_queries; }
    quint16 port() const override;
    QHostAddress peerAddress(api::ConnectionId) const override;
    bool sendTcp(api::ConnectionId, const QByteArray &) override;
    bool sendUdp(const api::Endpoint &, const QByteArray &) override;
    void disconnect(api::ConnectionId) override;
    qint64 queuedTcpBytes(api::ConnectionId) const override;
    void sendDataTo(const QString &, const QString &, const QByteArray &, QObject *, util::SendCallback) override;
    void broadcastDataTo(const QString &, const QStringList &, const QByteArray &, QObject *, util::SendCallback) override;
    void broadcastDataBalancedTo(const QString &, const QStringList &, const QByteArray &, QObject *, util::SendCallback) override;
    void getNetworkRelays(const QString &, QObject *, util::RelayCallback) override;
    util::NetworkSubscription *subscribeNetworkData(const QString &, QObject *, util::NetworkCallback) override;
    util::FileResult readFile(const QString &) override;
    util::FileResult writeFile(const QString &, const QByteArray &) override;
    util::LogResult write(util::LogLevel, const QString &) override;
    void stopRequests() { m_queries.stop(); for (auto *job : m_requests.children()) delete job; m_stopped=true; }
private:
    ServerHost &m_host;
    QString m_id, m_sharedDataRoot, m_pluginName, m_dataRoot;
    QFile m_logFile;
    ZeroTierQueries m_queries;
    QObject m_requests;
    bool m_stopped=false;
    QObject *requestContext(QObject *);
    QString filePath(const QString &, bool, QString &) const;
};
}
