#include "NetworkTransport.h"
#include <ztprotocol/NetworkDelivery.h>
#include <ztprotocol/NetworkAddress.h>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QTimer>
#include <QDateTime>
#include <QUuid>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace vusiif::ztgui::network {
namespace p = protocol;
using Reply = std::function<void(QJsonObject)>;
static qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
static QJsonObject failure(const QString &e) { return {{"ok", false}, {"error", e}}; }
static QJsonObject success(const QJsonValue &v) { return {{"ok", true}, {"data", v}}; }
static void rpc(QObject *owner, QHostAddress source, QHostAddress destination, quint16 port,
                const QString &op, const QJsonObject &data, Reply callback, int deadline = 1500) {
    if(owner->findChildren<QTcpSocket *>().size()>=128) { callback(failure("too_many_pending_requests")); return; }
    auto *job = new QObject(owner); auto *socket = new QTcpSocket(job); socket->setProxy(QNetworkProxy::NoProxy);
    socket->setReadBufferSize(p::MaxTcpBytes + 4); auto *timer = new QTimer(job); timer->setSingleShot(true);
    auto doneFlag = std::make_shared<bool>(false); auto input = std::make_shared<QByteArray>();
    auto done = [job, socket, timer, doneFlag, callback](QJsonObject reply) {
        if (*doneFlag) return; *doneFlag = true; timer->stop(); socket->abort(); job->deleteLater(); callback(reply);
    };
    QObject::connect(timer, &QTimer::timeout, job, [done] { done(failure("request_timeout")); });
    QObject::connect(socket, &QTcpSocket::errorOccurred, job, [done, socket] { done(failure(socket->errorString())); });
    QObject::connect(socket, &QTcpSocket::disconnected, job, [done] { done(failure("connection_closed")); });
    QObject::connect(socket, &QTcpSocket::connected, job, [socket, op, data] {
        socket->write(p::frame("builtin", QJsonDocument(QJsonObject{{"v",1},{"id",1},{"op",op},{"data",data}}).toJson(QJsonDocument::Compact)));
    });
    QObject::connect(socket, &QTcpSocket::readyRead, job, [socket, input, done] {
        *input += socket->readAll(); if (input->size() > p::MaxTcpBytes + 4) { done(failure("response_too_large")); return; }
        p::Packet packet; const auto status = p::takeFrame(*input, packet);
        if (status == p::ReadResult::Incomplete) return;
        const auto doc = QJsonDocument::fromJson(packet.payload); const auto object = doc.object();
        if (status != p::ReadResult::Packet || packet.plugin != "builtin" || !doc.isObject()
            || object["v"].toInt() != 1 || object["id"].toInt() != 1) done(failure("invalid_response"));
        else done(object);
    });
    if (!source.isNull() && !socket->bind(source)) { done(failure("source_bind_failed")); return; }
    timer->start(deadline); socket->connectToHost(destination, port);
}

struct Relay {
    util::RelayInfo info; QList<double> samples; int failures = 0; bool pending = false; qint64 reserved = 0;
};
struct Binding { QString relay; qint64 used = 0; int outside = 0; };
struct Network {
    QString id, node; QHostAddress local, controller; quint16 controllerPort = p::NetworkPort;
    QJsonArray members; QHash<QString,QJsonObject> membersById; qint64 membersAt = 0; QMap<QString, Relay> relays;
    QHash<QString, quint16> endpoints; QHash<QString,qint64> endpointAt; QHash<QString, Binding> bindings;
    QUdpSocket *socket = nullptr; bool refreshing = false; QList<Reply> waiting;
};
class TransportWorker final : public QObject {
public:
    TransportWorker(bool server, NetworkTransport *facade) : server(server), facade(facade) {}
    bool server; NetworkTransport *facade; QMap<QString, Network> networks; QJsonObject config;
    QHash<QString, qint64> seen; qint64 forwardedBytes = 0, rateStart = now();
    struct Probe { QString network, relay; QHostAddress address; QElapsedTimer elapsed; std::function<void(double, QJsonObject)> callback; };
    QHash<QString, std::shared_ptr<Probe>> probes;
    void start() {
        auto *timer = new QTimer(this); timer->setInterval(2000);
        connect(timer, &QTimer::timeout, this, [this] {
            const auto t = now();
            for (auto i = seen.begin(); i != seen.end();) i = i.value() < t ? seen.erase(i) : ++i;
            for (auto &n : networks) {
                for (auto i = n.bindings.begin(); i != n.bindings.end();) i = t - i->used > 30000 ? n.bindings.erase(i) : ++i;
                for (auto &r : n.relays) r.reserved = 0;
                refresh(n.id, {});
                for (auto &r : n.relays) {
                    if (r.pending) continue; r.pending = true;
                    const auto key = r.info.virtualIp + ':' + QString::number(r.info.port); const auto id = n.id;
                    probe(id, QHostAddress(r.info.virtualIp), r.info.port, [this, id, key](double ms, QJsonObject stats) {
                        if (!networks.contains(id) || !networks[id].relays.contains(key)) return;
                        auto &r = networks[id].relays[key]; r.pending = false;
                        if (ms < 0) { if (++r.failures >= 3) r.info.available = false; }
                        else {
                            r.failures = 0; r.samples.append(ms); if (r.samples.size() > 10) r.samples.removeFirst();
                            double sum = 0; for (double sample : r.samples) sum += sample;
                            r.info.averageLatency = sum / r.samples.size(); r.info.lastLatency = ms;
                            r.info.available = true; r.info.sampledAt = now();
                            r.info.bytesPerSecond = qint64(stats["bytesPerSecond"].toDouble());
                            r.info.queuedBytes = qint64(stats["queuedBytes"].toDouble());
                        }
                        // Count samples, rather than send calls, when a stable assignment leaves the latency band.
                        auto &n = networks[id]; const auto eligible = candidates(n);
                        for (auto &b : n.bindings) {
                            if (b.relay == key) b.outside = eligible.contains(key) ? 0 : b.outside + 1;
                        }
                    });
                }
            }
            if (t - rateStart > 10000) { rateStart = t; forwardedBytes = 0; }
        }); timer->start();
    }
    QJsonObject member(const Network &n, const QString &id) const {
        if (now() - n.membersAt > 30000) return {};
        return n.membersById.value(id.toLower());
    }
    bool authorized(const Network &n, const QString &node, const QHostAddress &source) const {
        return !source.isNull() && p::memberAddress(member(n, node)) == source;
    }
    void configure(QString node, QJsonArray descriptors) {
        QSet<QString> retained;
        for (const auto &v : descriptors) {
            const auto d = v.toObject(); const auto id = d["nwid"].toString(d["id"].toString()).toLower();
            if (!p::validNetworkId(id) || !p::validMemberId(node)) continue;
            QHostAddress local;
            for (const auto &a : d["assignedAddresses"].toArray()) {
                QHostAddress ip(a.toString().section('/',0,0)); if (ip.protocol() == QAbstractSocket::IPv4Protocol) { local = ip; break; }
            }
            if (local.isNull()) continue; retained.insert(id);
            if (networks.contains(id) && networks[id].local != local) { delete networks[id].socket; networks.remove(id); }
            auto &n = networks[id]; n.id = id; n.node = node.toLower(); n.local = local; n.controller = p::networkServer(d);
            // Explicit endpoint overrides are used only by isolated transport tests.
            if (d.contains("transportController")) n.controller = QHostAddress(d["transportController"].toString());
            n.controllerPort = quint16(d["transportControllerPort"].toInt(p::NetworkPort));
            if (!n.socket) {
                n.socket = new QUdpSocket(this); n.socket->setProxy(QNetworkProxy::NoProxy);
                if (!n.socket->bind(local, server ? 0 : p::ClientNetworkPort)) { delete n.socket; n.socket = nullptr; networks.remove(id); continue; }
                connect(n.socket, &QUdpSocket::readyRead, this, [this, id] {
                    if (!networks.contains(id)) return; auto *s = networks[id].socket;
                    while (s->hasPendingDatagrams()) {
                        const auto dg = s->receiveDatagram(p::MaxUdpBytes + 1); p::Packet packet;
                        if (dg.data().size() <= p::MaxUdpBytes && p::decode(dg.data(), packet) && packet.plugin == "builtin")
                            datagram(dg.senderAddress(), dg.senderPort(), packet.payload);
                    }
                });
            }
            refresh(id, {});
        }
        for (auto i = networks.begin(); i != networks.end();) {
            if (!retained.contains(i.key())) { delete i->socket; i = networks.erase(i); } else ++i;
        }
    }
    void refresh(const QString &id, Reply done) {
        if (!networks.contains(id)) { if (done) done(failure("network_unavailable")); return; }
        auto &n = networks[id]; if (done) n.waiting.append(done); if (n.refreshing) return; n.refreshing = true;
        rpc(this, n.local, n.controller, n.controllerPort, "controller.members", {{"networkId",id}}, [this,id](QJsonObject result) {
            if (!networks.contains(id)) return;
            auto &n = networks[id];
            if (result["ok"].toBool() && result["data"].isArray()) { n.members = result["data"].toArray(); n.membersById.clear();
                for(const auto &v:n.members) { const auto m=v.toObject(); if(p::validMemberId(m["nodeId"].toString())) n.membersById[m["nodeId"].toString().toLower()]=m; }
                n.membersAt = now(); }
            rpc(this, n.local, n.controller, n.controllerPort, "network.relays", {{"networkId",id}}, [this,id](QJsonObject response) {
                if (!networks.contains(id)) return; auto &n = networks[id];
                QJsonArray list = response["data"].toObject()["relays"].toArray();
                if (list.isEmpty()) list.append(QJsonObject{{"nodeId",id.left(10)},{"moonId","000000"+id.left(10)},
                                                          {"virtualIp",n.controller.toString()},{"port",n.controllerPort}});
                QSet<QString> retained;
                for (const auto &v : list) {
                    const auto o = v.toObject(); const auto ip = QHostAddress(o["virtualIp"].toString()); const int port = o["port"].toInt(p::NetworkPort);
                    if (!p::validMemberId(o["nodeId"].toString()) || p::memberAddress(member(n,o["nodeId"].toString())) != ip || port < 1 || port > 65535) continue;
                    const auto key = ip.toString()+':'+QString::number(port); retained.insert(key);
                    auto &r = n.relays[key]; r.info.nodeId = o["nodeId"].toString().toLower(); r.info.moonId = o["moonId"].toString();
                    r.info.virtualIp = ip.toString(); r.info.port = quint16(port);
                }
                for (auto i = n.relays.begin(); i != n.relays.end();) i = retained.contains(i.key()) ? ++i : n.relays.erase(i);
                const auto endpoints = response["data"].toObject()["endpoints"].toObject();
                for (auto i = endpoints.begin(); i != endpoints.end(); ++i) {
                    const int port = i.value().toInt(); if (port == p::ClientNetworkPort || port == p::NetworkPort) n.endpoints[i.key()] = quint16(port);
                }
                // Register advertised receiving capability on each host, never write ZeroTier configuration.
                for (const auto &r : n.relays) rpc(this,n.local,QHostAddress(r.info.virtualIp),r.info.port,"network.register",
                    {{"networkId",id},{"senderId",n.node},{"port",server ? p::NetworkPort : p::ClientNetworkPort}}, [](QJsonObject) {});
                n.refreshing = false; auto waiting = std::move(n.waiting); n.waiting.clear();
                for (auto &callback : waiting) callback(n.membersAt && now()-n.membersAt <= 30000 ? success(QJsonObject{}) : failure("member_addresses_unavailable"));
            });
        }, 20000);
    }
    QStringList candidates(const Network &n) const {
        double minimum = 1e30; QStringList result;
        for (const auto &r : n.relays) if (r.info.available && now()-r.info.sampledAt <= 10000) minimum = std::min(minimum,r.info.averageLatency);
        const auto ceiling = minimum + std::max(5., minimum*.2);
        for (auto i=n.relays.begin(); i!=n.relays.end(); ++i)
            if (i->info.available && now()-i->info.sampledAt <= 10000 && i->info.averageLatency >= 0 && i->info.averageLatency <= ceiling) result.append(i.key());
        return result;
    }
    void probe(const QString &id, const QHostAddress &address, quint16 port, std::function<void(double,QJsonObject)> callback) {
        if (!networks.contains(id)) { callback(-1,{}); return; }
        auto pr = std::make_shared<Probe>(); pr->network=id; pr->address=address; pr->callback=callback; pr->elapsed.start();
        const auto nonce = QUuid::createUuid().toString(QUuid::Id128); probes.insert(nonce,pr);
        auto &n=networks[id]; const QJsonObject request{{"op","network.probe"},{"networkId",id},{"senderId",n.node},{"nonce",nonce}};
        n.socket->writeDatagram(p::encode("builtin",QJsonDocument(request).toJson(QJsonDocument::Compact)),address,port);
        QTimer::singleShot(1000,this,[this,nonce] { auto p=probes.take(nonce); if(p) p->callback(-1,{}); });
    }
    void datagram(const QHostAddress &source, quint16 port, const QByteArray &payload) {
        p::Delivery delivery;
        if (p::decodeDelivery(payload,delivery)) {
            if (!networks.contains(delivery.network)) return; auto &n=networks[delivery.network];
            if (delivery.target != n.node || p::memberAddress(member(n,delivery.sender)).isNull()) return;
            bool trusted = authorized(n,delivery.sender,source);
            for (const auto &r:n.relays) if (QHostAddress(r.info.virtualIp)==source) trusted=true;
            if (!trusted) return;
            const auto key=delivery.network+'/'+delivery.sender+'/'+delivery.plugin+'/'+QString::fromLatin1(delivery.messageId.toHex());
            if (seen.value(key)>now()) return; if (seen.size()>=8192) return; seen[key]=now()+30000;
            QMetaObject::invokeMethod(facade,[f=facade,delivery] { emit f->received(delivery.plugin,{delivery.network,delivery.sender,delivery.data}); },Qt::QueuedConnection);
            return;
        }
        auto object=QJsonDocument::fromJson(payload).object(); if (object["op"].toString()!="network.probe") return;
        const auto nonce=object["nonce"].toString();
        if (object["reply"].toBool()) {
            const auto pr=probes.value(nonce);
            if (!pr || pr->address!=source || pr->network!=object["networkId"].toString()) return;
            probes.remove(nonce); pr->callback(pr->elapsed.nsecsElapsed()/1e6,object); return;
        }
        const auto id=object["networkId"].toString(); if (!networks.contains(id)) return; auto &n=networks[id];
        if (!authorized(n,object["senderId"].toString(),source)) return;
        object["reply"]=true; object["bytesPerSecond"]=double(forwardedBytes*1000/std::max<qint64>(1000,now()-rateStart)); object["queuedBytes"]=0;
        n.socket->writeDatagram(p::encode("builtin",QJsonDocument(object).toJson(QJsonDocument::Compact)),source,port);
    }
    void builtin(const QHostAddress &source,const QJsonObject &request,Reply done,std::shared_ptr<std::atomic_bool> cancelled) {
        const auto data=request["data"].toObject(); const auto id=data["networkId"].toString().toLower();
        const auto op=request["op"].toString();
        if (!networks.contains(id)) { done(failure("network_unavailable")); return; }
        auto execute=[this,source,request,data,id,op,done,cancelled](QJsonObject refreshed) {
            if(cancelled->load()) return;
            if (!refreshed["ok"].toBool() || !networks.contains(id)) { done(failure("members_unavailable")); return; }
            auto &n=networks[id]; QString sender=data["senderId"].toString().toLower();
            if (op=="network.relays") {
                bool allowed=false; for (const auto &v:n.members) if (p::memberAddress(v.toObject())==source) allowed=true;
                if(!allowed) { done(failure("unauthorized")); return; }
                QJsonArray list=config[id].toArray();
                const auto primary=p::networkServer(QJsonObject{{"assignedAddresses",QJsonArray{n.local.toString()+"/24"}}});
                list.prepend(QJsonObject{{"nodeId",id.left(10)},{"moonId","000000"+id.left(10)},{"virtualIp",primary.toString()},{"port",p::NetworkPort}});
                QJsonObject endpoints; for(auto i=n.endpoints.begin();i!=n.endpoints.end();++i)
                    if(now()-n.endpointAt.value(i.key())<=10000) endpoints[i.key()]=i.value();
                done(success(QJsonObject{{"relays",list},{"endpoints",endpoints}})); return;
            }
            if(!authorized(n,sender,source)) { done(failure("unauthorized_sender")); return; }
            if(op=="network.register") {
                const int port=data["port"].toInt(); if(port!=p::ClientNetworkPort && port!=p::NetworkPort) { done(failure("invalid_port")); return; }
                // Prefer the GUI receiver when both host types register on the same node.
                if (port==p::ClientNetworkPort || !n.endpoints.contains(sender) || now()-n.endpointAt.value(sender)>10000) {
                    n.endpoints[sender]=quint16(port); n.endpointAt[sender]=now();
                } else if(n.endpoints[sender]==port) n.endpointAt[sender]=now();
                done(success(QJsonObject{})); return;
            }
            if(op!="network.forward") { done(failure("unknown_operation")); return; }
            const auto plugin=data["pluginId"].toString(); const auto message=QByteArray::fromHex(data["messageId"].toString().toLatin1());
            const auto decoded=QByteArray::fromBase64Encoding(data["payload"].toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
            const auto targets=data["members"].toArray();
            if(!p::validPluginId(plugin)||message.size()!=16||!decoded||decoded.decoded.size()>p::maxNetworkDataBytes(plugin)||targets.size()>4096) { done(failure("invalid_forward")); return; }
            int sent=0; QJsonArray failed; QSet<QString> unique;
            for(const auto &v:targets) {
                const auto target=v.toString().toLower(); if(unique.contains(target)) continue; unique.insert(target);
                const auto address=p::memberAddress(member(n,target));
                if(!p::validMemberId(target)||address.isNull()) { failed.append(target); continue; }
                const auto key=id+'/'+sender+'/'+plugin+'/'+QString::fromLatin1(message.toHex())+'/'+target;
                if(seen.value(key)>now()) { ++sent; continue; }
                const auto bytes=p::encodeDelivery({id,sender,target,plugin,message,decoded.decoded});
                if(bytes.isEmpty()||seen.size()>=8192||n.socket->writeDatagram(bytes,address,n.endpoints.value(target,p::ClientNetworkPort))!=bytes.size()) failed.append(target);
                else { seen[key]=now()+30000; forwardedBytes+=bytes.size(); ++sent; }
            }
            done(success(QJsonObject{{"recipients",sent},{"failedMembers",failed}}));
        };
        if (networks[id].membersAt && now()-networks[id].membersAt<=30000) execute(success(QJsonObject{}));
        else if(op=="network.relays") { done(failure("members_unavailable")); } // Avoid discovery recursively querying itself.
        else refresh(id,execute);
    }
    void send(QString plugin,QString id,QStringList targets,QByteArray data,bool balanced,
              std::shared_ptr<std::atomic_bool> cancelled,std::function<void(util::SendResult)> done) {
        id=id.toLower(); QSet<QString> unique; QStringList normalized;
        for(auto target:targets) { target=target.toLower(); if(!p::validMemberId(target)) { done({0,"invalid_member_id"}); return; } if(!unique.contains(target)) { unique.insert(target); normalized.append(target); } }
        if(!p::validPluginId(plugin)||!p::validNetworkId(id)||data.size()>p::maxNetworkDataBytes(plugin)||normalized.size()>4096) { done({0,"invalid_network_or_payload"}); return; }
        if(normalized.isEmpty()) { done({}); return; }
        auto perform=[this,plugin,id,normalized,data,balanced,cancelled,done](QJsonObject response) {
            if(cancelled->load()) return;
            if(!response["ok"].toBool()||!networks.contains(id)) { done({0,response["error"].toString(),normalized}); return; }
            auto &n=networks[id]; auto result=std::make_shared<util::SendResult>();
            const auto message=QUuid::createUuid().toRfc4122(); QMap<QString,QStringList> groups;
            for(const auto &target:normalized) {
                if(p::memberAddress(member(n,target)).isNull()) { result->failedMembers.append(target); continue; }
                QString relay;
                if(balanced) {
                    const auto eligible=candidates(n); const auto bindingKey=plugin+'/'+n.node+'/'+target;
                    auto &b=n.bindings[bindingKey];
                    if(!b.relay.isEmpty()&&n.relays.contains(b.relay)&&n.relays[b.relay].info.available&&now()-n.relays[b.relay].info.sampledAt<=10000&&b.outside<3) relay=b.relay;
                    else if(!eligible.isEmpty()) {
                        relay=*std::min_element(eligible.begin(),eligible.end(),[&](const QString &a,const QString &b) {
                            const auto &ra=n.relays[a]; const auto &rb=n.relays[b];
                            return ra.info.bytesPerSecond+ra.info.queuedBytes+ra.reserved < rb.info.bytesPerSecond+rb.info.queuedBytes+rb.reserved;
                        }); b.relay=relay; b.outside=0;
                    }
                    b.used=now(); if(!relay.isEmpty()) n.relays[relay].reserved+=data.size()+52+plugin.size();
                } else {
                    const auto key=n.controller.toString()+':'+QString::number(n.controllerPort);
                    if(n.relays.contains(key)&&((n.relays[key].info.available&&now()-n.relays[key].info.sampledAt<=10000)
                        || (n.relays[key].info.sampledAt==0 && n.relays[key].failures<3))) relay=key;
                }
                groups[relay].append(target);
            }
            auto outstanding=std::make_shared<int>(groups.size());
            auto finish=[result,outstanding,done] { if(--*outstanding==0) { if(!result->failedMembers.isEmpty()) result->error="some_members_not_queued"; done(*result); } };
            auto direct=[this,id,plugin,data,message,result](const QStringList &members) {
                if(!networks.contains(id)) { result->failedMembers+=members; return; } auto &n=networks[id];
                for(const auto &target:members) {
                    const auto bytes=p::encodeDelivery({id,n.node,target,plugin,message,data}); const auto address=p::memberAddress(member(n,target));
                    if(address.isNull()||n.socket->writeDatagram(bytes,address,n.endpoints.value(target,p::ClientNetworkPort))!=bytes.size()) result->failedMembers.append(target);
                    else ++result->recipients;
                }
            };
            if(groups.isEmpty()) { result->error="no_authorized_targets"; done(*result); return; }
            for(auto i=groups.begin();i!=groups.end();++i) {
                const auto members=i.value(); if(i.key().isEmpty()) { direct(members); finish(); continue; }
                const auto initial=i.key(); QJsonArray array; for(const auto &member:members) array.append(member);
                const QJsonObject request{{"networkId",id},{"senderId",n.node},{"pluginId",plugin},{"members",array},
                    {"messageId",QString::fromLatin1(message.toHex())},{"payload",QString::fromLatin1(data.toBase64())}};
                auto attempted=std::make_shared<QSet<QString>>(); auto retry=std::make_shared<std::function<void(QString)>>();
                std::weak_ptr<std::function<void(QString)>> weak=retry;
                *retry=[this,id,plugin,members,data,request,result,direct,finish,cancelled,balanced,attempted,weak](QString key) {
                    if(cancelled->load()) return;
                    if(key.isEmpty()||!networks.contains(id)||!networks[id].relays.contains(key)) { direct(members); finish(); return; }
                    attempted->insert(key); const auto r=networks[id].relays[key].info; const auto keep=weak.lock();
                    rpc(this,networks[id].local,QHostAddress(r.virtualIp),r.port,"network.forward",request,
                        [this,id,plugin,key,members,data,result,direct,finish,cancelled,balanced,attempted,keep](QJsonObject ack) {
                            if(cancelled->load()) return;
                            const auto response=ack["data"].toObject(); QStringList failed;
                            for(const auto &v:response["failedMembers"].toArray()) if(members.contains(v.toString())) failed.append(v.toString());
                            const int count=response["recipients"].toInt(-1);
                            if(ack["ok"].toBool() && count>=0 && count+failed.size()==members.size()) {
                                result->recipients+=count; direct(failed); finish(); return;
                            }
                            QString next;
                            if(networks.contains(id)) {
                                auto &n=networks[id]; if(n.relays.contains(key)) n.relays[key].info.available=false;
                                if(balanced) for(const auto &candidate:candidates(n)) {
                                    if(attempted->contains(candidate)) continue;
                                    if(next.isEmpty() || n.relays[candidate].info.bytesPerSecond+n.relays[candidate].info.queuedBytes+n.relays[candidate].reserved <
                                        n.relays[next].info.bytesPerSecond+n.relays[next].info.queuedBytes+n.relays[next].reserved) next=candidate;
                                }
                                if(!next.isEmpty()) {
                                    n.relays[next].reserved+=(data.size()+52+plugin.size())*members.size();
                                    for(const auto &target:members) n.bindings[plugin+'/'+n.node+'/'+target]={next,now(),0};
                                }
                            }
                            if(!next.isEmpty() && keep) (*keep)(next); else { direct(members); finish(); }
                        });
                };
                (*retry)(initial);
            }
        };
        if(networks.contains(id) && now()-networks[id].membersAt<=2000) perform(success(QJsonObject{}));
        else refresh(id,std::move(perform));
    }
};

class Subscription final : public util::NetworkSubscription {
public:
    Subscription(QObject *context) : NetworkSubscription(context) {}
    QMetaObject::Connection connection; util::NetworkCallback callback;
    void cancel() override { QObject::disconnect(connection); callback={}; }
    ~Subscription() override { cancel(); }
};
NetworkTransport::NetworkTransport(bool server,QObject *parent) : QObject(parent),m_worker(new TransportWorker(server,this)) {
    m_worker->moveToThread(&m_thread); connect(&m_thread,&QThread::finished,m_worker,&QObject::deleteLater); m_thread.start();
    QMetaObject::invokeMethod(m_worker,[w=m_worker] { w->start(); });
}
NetworkTransport::~NetworkTransport() { m_thread.quit(); m_thread.wait(); }
void NetworkTransport::setNetworks(const QString &node,const QJsonArray &networks) { QMetaObject::invokeMethod(m_worker,[w=m_worker,node,networks] { w->configure(node,networks); }); }
void NetworkTransport::setRelayConfig(const QJsonObject &config) { QMetaObject::invokeMethod(m_worker,[w=m_worker,config] { w->config=config; }); }
void NetworkTransport::send(const QString &plugin,const QString &network,const QStringList &members,const QByteArray &data,bool balanced,QObject *context,util::SendCallback callback) {
    if(!context||!callback||context->thread()!=thread()||QThread::currentThread()!=thread()) return;
    auto *job=new QObject(context); auto cancelled=std::make_shared<std::atomic_bool>(false); auto holder=std::make_shared<util::SendCallback>(std::move(callback));
    connect(job,&QObject::destroyed,this,[cancelled,holder] { cancelled->store(true); *holder={}; }); const QPointer<QObject> alive(job);
    QMetaObject::invokeMethod(m_worker,[this,w=m_worker,plugin,network,members,data,balanced,cancelled,holder,alive] {
        w->send(plugin,network,members,data,balanced,cancelled,[this,holder,alive](util::SendResult result) {
            QMetaObject::invokeMethod(this,[holder,alive,result] { if(!alive) return; auto callback=std::move(*holder); alive->deleteLater(); if(callback) callback(result); });
        });
    });
}
void NetworkTransport::relays(const QString &network,QObject *context,util::RelayCallback callback) {
    if(!context||!callback||context->thread()!=thread()||QThread::currentThread()!=thread()) return;
    auto *job=new QObject(context); const QPointer<QObject> alive(job); auto holder=std::make_shared<util::RelayCallback>(std::move(callback));
    connect(job,&QObject::destroyed,this,[holder] { *holder={}; });
    QMetaObject::invokeMethod(m_worker,[this,w=m_worker,network,alive,holder] {
        const auto id=network.toLower(); w->refresh(id,[this,w,id,alive,holder](QJsonObject response) {
            util::RelayResult result; if(!response["ok"].toBool()) result.error=response["error"].toString();
            if(w->networks.contains(id)) for(const auto &r:w->networks[id].relays) { auto info=r.info; if(now()-info.sampledAt>10000) info.available=false; result.relays.append(info); }
            QMetaObject::invokeMethod(this,[alive,holder,result] { if(!alive) return; auto cb=std::move(*holder); alive->deleteLater(); if(cb) cb(result); });
        });
    });
}
util::NetworkSubscription *NetworkTransport::subscribe(const QString &plugin,const QString &network,QObject *context,util::NetworkCallback callback) {
    if(!context||!callback||context->thread()!=thread()||QThread::currentThread()!=thread()||!p::validNetworkId(network)) return nullptr;
    auto *sub=new Subscription(context); sub->callback=std::move(callback);
    sub->connection=connect(this,&NetworkTransport::received,sub,[sub,plugin,id=network.toLower()](const QString &route,const util::NetworkMessage &message) {
        if(route.compare(plugin,Qt::CaseInsensitive)==0&&id==message.networkId&&sub->callback) { auto cb=sub->callback; cb(message); }
    }); return sub;
}
void NetworkTransport::probeMember(const QString &network,const QString &member,QObject *context,std::function<void(double)> callback) {
    if(!context||!callback||context->thread()!=thread()||QThread::currentThread()!=thread()) return; auto holder=std::make_shared<std::function<void(double)>>(std::move(callback)); const QPointer<QObject> alive(context);
    auto connection=connect(context,&QObject::destroyed,this,[holder] { *holder={}; });
    QMetaObject::invokeMethod(m_worker,[this,w=m_worker,network,member,holder,alive,connection] {
        auto done=[this,holder,alive,connection](double value,QJsonObject) { QMetaObject::invokeMethod(this,[holder,alive,connection,value] { disconnect(connection); if(alive&&*holder) (*holder)(value); }); };
        if(!w->networks.contains(network)) { done(-1,{}); return; } auto &n=w->networks[network];
        if(member==n.node) { done(0,{}); return; } const auto address=p::memberAddress(w->member(n,member));
        if(address.isNull()) { done(-1,{}); return; } w->probe(network,address,n.endpoints.value(member,p::ClientNetworkPort),done);
    });
}
void NetworkTransport::builtin(const QHostAddress &source,const QJsonObject &request,QObject *context,std::function<void(const QJsonObject &)> callback) {
    auto holder=std::make_shared<std::function<void(const QJsonObject &)>>(std::move(callback)); const QPointer<QObject> alive(context);
    auto cancelled=std::make_shared<std::atomic_bool>(false);
    auto connection=connect(context,&QObject::destroyed,this,[holder,cancelled] { cancelled->store(true); *holder={}; });
    QMetaObject::invokeMethod(m_worker,[this,w=m_worker,source,request,holder,alive,connection,cancelled] { w->builtin(source,request,[this,holder,alive,connection](QJsonObject result) {
        QMetaObject::invokeMethod(this,[this,holder,alive,connection,result] { disconnect(connection); if(alive&&*holder) (*holder)(result); });
    },cancelled); });
}
void NetworkTransport::datagram(const QHostAddress &source,quint16 port,const QByteArray &payload) { QMetaObject::invokeMethod(m_worker,[w=m_worker,source,port,payload] { w->datagram(source,port,payload); }); }
}
