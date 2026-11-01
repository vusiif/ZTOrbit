#pragma once
#include <QObject>
#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <ztprotocol/NetworkService.h>
#include <functional>
#include <memory>
namespace vusiif::zto::network { class NetworkTransport; }
namespace vusiif::zto::zerotier { class JsonPoller; }
namespace vusiif::zto::network {
class ClientNetworkService final : public QObject {
public:
    static ClientNetworkService &instance();
    ~ClientNetworkService() override;
    void withTransport(QObject *context, std::function<void(NetworkTransport &)> action);
    util::NetworkSubscription *subscribe(const QString &plugin, const QString &network,
                                        QObject *context, util::NetworkCallback callback);
private:
    explicit ClientNetworkService(QObject *parent);
    std::unique_ptr<NetworkTransport> m_transport;
    QString m_node;
    QJsonArray m_networks;
    QString m_configuredNode;
    QJsonArray m_configuredNetworks;
    zerotier::JsonPoller *m_info, *m_networkPoller;
    QTimer m_readyTimeout, m_idle;
    QSet<QObject *> m_users;
    QHash<QObject *, std::function<void(NetworkTransport &)>> m_pending;
    bool m_infoReady=false, m_networksReady=false, m_configured=false;
    void activate();
    void dispatch();
};
}
