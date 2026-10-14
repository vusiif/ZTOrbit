#pragma once
#include <QObject>
#include <QJsonArray>
#include <memory>
namespace vusiif::ztgui::network { class NetworkTransport; }
namespace vusiif::ztgui::zerotier { class JsonPoller; }
namespace vusiif::ztgui::network {
class ClientNetworkService final : public QObject {
public:
    static ClientNetworkService &instance();
    NetworkTransport &transport() { return *m_transport; }
private:
    explicit ClientNetworkService(QObject *parent);
    std::unique_ptr<NetworkTransport> m_transport;
    QString m_node;
    QJsonArray m_networks;
};
}
