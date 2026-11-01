#include "ClientNetworkService.h"
#include "ZeroTierClient.h"
#include "NetworkTransport.h"
#include <ztprotocol/NetworkDelivery.h>
#include <QCoreApplication>
#include <QJsonObject>
namespace vusiif::zto::network {
namespace {
// Cancellation releases demand even if the subscription object remains owned
// by the plugin's context until later.
class Subscription final : public util::NetworkSubscription {
public:
    explicit Subscription(QObject *context) : NetworkSubscription(context), lease(new QObject(this)) {}
    QPointer<QObject> lease;
    void cancel() override { delete lease.data(); }
    ~Subscription() override { cancel(); }
};
}
ClientNetworkService &ClientNetworkService::instance() {
    static QPointer<ClientNetworkService> service;
    if (!service) service = new ClientNetworkService(QCoreApplication::instance());
    return *service;
}
ClientNetworkService::ClientNetworkService(QObject *parent) : QObject(parent),
    m_info(new zerotier::JsonPoller(this,"info",true)),
    m_networkPoller(new zerotier::JsonPoller(this,"listnetworks")) {
    connect(m_info,&zerotier::JsonPoller::updated,this,[this](const QJsonDocument &doc) {
        m_node=doc.object()["address"].toString(); m_infoReady=true; dispatch();
    });
    connect(m_networkPoller,&zerotier::JsonPoller::updated,this,[this](const QJsonDocument &doc) {
        m_networks=doc.array(); m_networksReady=true; dispatch();
    });
    m_readyTimeout.setSingleShot(true); m_readyTimeout.setInterval(6500);
    connect(&m_readyTimeout,&QTimer::timeout,this,[this] {
        // Unavailable networks fail through the normal transport callback.
        m_infoReady=true; m_networksReady=true; dispatch();
    });
    m_idle.setSingleShot(true); m_idle.setInterval(0);
    connect(&m_idle,&QTimer::timeout,this,[this] {
        if(!m_users.isEmpty()) return;
        m_info->setActive(false); m_networkPoller->setActive(false); m_readyTimeout.stop();
        m_pending.clear();
        // Also stops relay maintenance, sockets, RPCs and probe deadlines.
        m_transport.reset(); m_node.clear(); m_networks={};
        m_infoReady=false; m_networksReady=false; m_configured=false;
        m_configuredNode.clear(); m_configuredNetworks={};
    });
}
ClientNetworkService::~ClientNetworkService() {
    for(auto *user:m_users) disconnect(user,nullptr,this,nullptr);
}
void ClientNetworkService::activate() {
    if(m_transport) return;
    m_transport=std::make_unique<NetworkTransport>();
    m_readyTimeout.start(); m_info->setActive(true); m_networkPoller->setActive(true);
}
void ClientNetworkService::withTransport(QObject *context, std::function<void(NetworkTransport &)> action) {
    if(!context || !action || context->thread()!=thread() || QThread::currentThread()!=thread()) return;
    m_idle.stop();
    if(!m_users.contains(context)) {
        m_users.insert(context);
        connect(context,&QObject::destroyed,this,[this,context] {
            m_pending.remove(context); m_users.remove(context);
            if(m_users.isEmpty()) m_idle.start();
        });
    }
    m_pending.insert(context,std::move(action));
    activate(); dispatch();
}
void ClientNetworkService::dispatch() {
    if(!m_transport || !m_infoReady || !m_networksReady) return;
    m_readyTimeout.stop();
    if(!m_configured || m_configuredNode!=m_node || m_configuredNetworks!=m_networks) {
        m_transport->setNetworks(m_node,m_networks);
        m_configuredNode=m_node; m_configuredNetworks=m_networks; m_configured=true;
    }
    auto pending=std::move(m_pending); m_pending.clear();
    for(auto it=pending.begin();it!=pending.end();++it) {
        const QPointer<QObject> alive(it.key());
        QTimer::singleShot(0,it.key(),[this,alive,action=std::move(it.value())] {
            if(alive && m_transport) action(*m_transport);
        });
    }
}
util::NetworkSubscription *ClientNetworkService::subscribe(const QString &plugin,const QString &network,
                                                          QObject *context,util::NetworkCallback callback) {
    if(!context || !callback || context->thread()!=thread() || QThread::currentThread()!=thread() || !protocol::validNetworkId(network)) return nullptr;
    auto *sub=new Subscription(context); const QPointer<QObject> lease=sub->lease;
    withTransport(lease,[plugin,network,lease,callback=std::move(callback)](NetworkTransport &transport) {
        if(lease) transport.subscribe(plugin,network,lease,std::move(callback));
    });
    return sub;
}
}
