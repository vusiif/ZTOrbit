#include "ClientNetworkService.h"
#include "ZeroTierClient.h"
#include "NetworkTransport.h"
#include <QCoreApplication>
#include <QJsonObject>
namespace vusiif::ztgui::network {
ClientNetworkService &ClientNetworkService::instance() {
    static QPointer<ClientNetworkService> service;
    if (!service) service = new ClientNetworkService(QCoreApplication::instance());
    return *service;
}
ClientNetworkService::ClientNetworkService(QObject *parent) : QObject(parent),m_transport(std::make_unique<NetworkTransport>()) {
    auto *info = new zerotier::JsonPoller(this,"info",true);
    auto *networks = new zerotier::JsonPoller(this,"listnetworks");
    connect(info,&zerotier::JsonPoller::updated,this,[this](const QJsonDocument &doc) { m_node=doc.object()["address"].toString(); m_transport->setNetworks(m_node,m_networks); });
    connect(networks,&zerotier::JsonPoller::updated,this,[this](const QJsonDocument &doc) { m_networks=doc.array(); m_transport->setNetworks(m_node,m_networks); });
    info->setActive(true); networks->setActive(true);
}
}
