#pragma once
#include <ztprotocol/ServiceWire.h>
#include <ztprotocol/BulkChannel.h>
#include <QCoreApplication>
#include <QLocalSocket>
#include <QTimer>
#include <QJsonArray>
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <functional>
#include <memory>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace vusiif::zto::protocol::service {
// Header-only worker-side transport. No host implementation or media dependency.
class Worker final : public QObject {
public:
    using Reply=std::function<void(const QJsonObject &,const QByteArray &,const QString &)>;
    std::function<void(const QString &,const QString &,const QString &,const QJsonObject &,const QByteArray &,Reply)> onCall;
    std::function<void(const QString &)> onRelease;
    std::function<void(const QString &,Reply)> onReleaseAsync;
    std::function<void(const QString &,const QJsonObject &,const QByteArray &)> onData;
    std::function<void()> onShutdown;
    std::function<void()> onConnected;
    explicit Worker(const QJsonArray &provides,QObject *parent=nullptr):QObject(parent),m_provides(provides) {
        m_socket.setReadBufferSize(MaxFrame*2);
        m_dataSocket.setReadBufferSize(MaxFrame*2);
        connect(&m_socket,&QLocalSocket::connected,this,[this] {
            send(hello("control"));
        });
        connect(&m_dataSocket,&QLocalSocket::connected,this,[this] {
            const auto bytes=frame(hello("data"));
            m_dataSocket.write(bytes);
        });
        connect(&m_socket,&QLocalSocket::readyRead,this,[this] { read(false); });
        connect(&m_dataSocket,&QLocalSocket::readyRead,this,[this] { read(true); });
        connect(&m_socket,&QLocalSocket::disconnected,this,[this] { shutdown(); });
        connect(&m_dataSocket,&QLocalSocket::disconnected,this,[this] { shutdown(); });
        connect(&m_socket,&QLocalSocket::errorOccurred,this,[this] { shutdown(); });
        connect(&m_dataSocket,&QLocalSocket::errorOccurred,this,[this] { shutdown(); });
        m_watch.setInterval(250);
        connect(&m_watch,&QTimer::timeout,this,[this] {
            if((!m_accepted || !m_dataAccepted) && QDateTime::currentMSecsSinceEpoch()>m_deadline) { shutdown(); return; }
#ifdef Q_OS_WIN
            if(!m_parentHandle || WaitForSingleObject(HANDLE(m_parentHandle),0)!=WAIT_TIMEOUT) shutdown();
#endif
        });
    }
    ~Worker() override {
#ifdef Q_OS_WIN
        if(m_parentHandle) CloseHandle(HANDLE(m_parentHandle));
#endif
    }
    // Call before start() for slow hardware preparation. The event thread must
    // stay responsive; call markReady() there only after preparation completes.
    void deferReady() { m_deferred=true; m_prepared=false; }
    void markReady() { m_prepared=true; publishReady(); }
    QString generation() const { return m_generation; }
    bool sendData(const QString &lease,const QJsonObject &header,const QByteArray &payload={}) {
        auto c=m_channels.value(lease); return c && c->send(header,payload);
    }
    bool start(const QStringList &args) {
        auto argument=[&](const QString &key) { const auto i=args.indexOf(key); return i>=0 && i+1<args.size()?args[i+1]:QString(); };
        const auto name=argument("--service-socket"); m_generation=argument("--service-generation");
        m_token=qEnvironmentVariable("ZTO_SERVICE_TOKEN"); qunsetenv("ZTO_SERVICE_TOKEN");
        if(name.isEmpty() || m_generation.isEmpty() || m_token.size()!=64) return false;
#ifdef Q_OS_WIN
        bool ok=false; const auto pid=argument("--host-pid").toULong(&ok);
        if(!ok || !pid) return false;
        m_parentHandle=OpenProcess(SYNCHRONIZE,FALSE,pid); if(!m_parentHandle) return false;
#endif
        m_deadline=QDateTime::currentMSecsSinceEpoch()+15000; m_watch.start(); m_socket.connectToServer(name); m_dataSocket.connectToServer(name+"-data"); return true;
    }
    void event(const QString &lease,const QJsonObject &data,const QByteArray &payload={}) {
        if(m_accepted && m_dataAccepted && m_leases.contains(lease)) send({{"type","event"},{"generation",m_generation},{"lease",lease},{"data",data}},payload);
    }
private:
    QLocalSocket m_socket,m_dataSocket;
    QTimer m_watch;
    QJsonArray m_provides;
    QMap<QString,QString> m_leases;
    QMap<QString,std::shared_ptr<BulkChannel>> m_channels;
    QMap<QString,quint64> m_next;
    QMap<QString,QMap<quint64,Message>> m_ordered;
    QString m_generation,m_token;
    QByteArray m_buffer,m_dataBuffer;
    qint64 m_deadline=0;
    bool m_accepted=false,m_dataAccepted=false,m_closing=false;
    bool m_deferred=false,m_prepared=true,m_readySent=false;
    void *m_parentHandle=nullptr;
    QJsonObject hello(const QString &channel) const {
        QJsonObject h{{"type","hello"},{"protocol",Protocol},{"token",m_token},{"generation",m_generation},
                      {"pid",QString::number(QCoreApplication::applicationPid())},{"provides",m_provides},{"channel",channel}};
        if(m_deferred) h["preparing"]=true;
        return h;
    }
    void publishReady() {
        if(m_deferred && m_prepared && m_accepted && m_dataAccepted && !m_readySent && !m_closing) {
            m_readySent=true; send({{"type","ready"},{"generation",m_generation}});
        }
    }
    bool send(const QJsonObject &header,const QByteArray &payload={}) {
        const auto bytes=frame(header,payload);
        auto &socket=payload.isEmpty()?m_socket:m_dataSocket;
        if(bytes.isEmpty() || socket.bytesToWrite()>1024*1024) { shutdown(); return false; }
        return socket.write(bytes)==bytes.size();
    }
    void shutdown() {
        if(m_closing) return; m_closing=true; m_watch.stop();
        if(onShutdown) onShutdown(); m_channels.clear(); m_leases.clear(); m_ordered.clear(); m_next.clear(); m_socket.abort(); m_dataSocket.abort(); QCoreApplication::quit();
    }
    void read(bool dataChannel) {
        auto &socket=dataChannel?m_dataSocket:m_socket; auto &buffer=dataChannel?m_dataBuffer:m_buffer;
        buffer+=socket.readAll();
        for(;;) {
            Message message; const auto status=take(buffer,message); if(status==0) break;
            if(status<0) { shutdown(); return; }
            const auto h=message.header;
            if(h["generation"].toString()!=m_generation) { shutdown(); return; }
            auto &accepted=dataChannel?m_dataAccepted:m_accepted;
            if(!accepted) {
                if(h["type"]!="accepted" || (m_deferred && h["deferredReady"]!=true)) { shutdown(); return; }
                accepted=true;
                if(m_accepted && m_dataAccepted) { m_token.clear(); if(onConnected) onConnected(); publishReady(); }
                continue;
            }
            if(h["type"]=="shutdown") { shutdown(); return; }
            if(h["type"]!="call" || (m_deferred && !m_readySent)) { shutdown(); return; }
            const auto lease=h["lease"].toString(); bool valid=false; const auto sequence=h["sequence"].toString().toULongLong(&valid);
            const auto next=m_next.value(lease,1);
            if(lease.size()!=32 || !valid || sequence<next || sequence-next>128 || m_ordered.value(lease).contains(sequence)) { shutdown(); return; }
            qsizetype queued=0; for(const auto &q:m_ordered) queued+=q.size(); if(queued>=128) { shutdown(); return; }
            m_ordered[lease].insert(sequence,std::move(message));
            while(m_ordered.value(lease).contains(m_next.value(lease,1))) {
                const auto expected=m_next.value(lease,1); auto ordered=m_ordered[lease].take(expected); m_next[lease]=expected+1;
                dispatch(ordered); if(m_closing) return;
                if(m_ordered.value(lease).isEmpty()) m_ordered.remove(lease);
            }
        }
        if(buffer.size()>MaxFrame) shutdown();
    }
    void dispatch(const Message &message) {
            const auto h=message.header;
            const auto lease=h["lease"].toString(),request=h["request"].toString(),service=h["service"].toString(),method=h["method"].toString();
            auto replied=std::make_shared<bool>(false);
            Reply reply=[this,request,replied](const QJsonObject &data,const QByteArray &payload,const QString &error) {
                if(*replied || m_closing) return; *replied=true;
                send({{"type","reply"},{"generation",m_generation},{"request",request},{"data",data},{"error",error}},payload);
            };
            if(lease.size()!=32 || request.isEmpty()) { shutdown(); return; }
            if(method=="$release") {
                m_channels.remove(lease);
                auto released=[this,lease,reply](const QJsonObject &d,const QByteArray &p,const QString &e) {
                    if(e.isEmpty()) { m_leases.remove(lease); m_next.remove(lease); m_ordered.remove(lease); } reply(d,p,e);
                };
                if(onReleaseAsync) onReleaseAsync(lease,std::move(released));
                else { if(onRelease) onRelease(lease); released({}, {}, {}); } return;
            }
            bool supported=false; for(const auto &p:m_provides) if(p.toObject()["service"]==service) supported=true;
            if(!supported || (m_leases.contains(lease) && m_leases[lease]!=service) || (!m_leases.contains(lease) && m_leases.size()>=32)) { reply({}, {}, "Invalid service/session limit"); return; }
            m_leases[lease]=service;
            if(method=="$channel") {
                if(m_channels.contains(lease)) { reply({}, {}, "Channel already exists"); return; }
                auto channel=std::make_shared<BulkChannel>();
                auto descriptor=channel->listen(lease,m_generation);
                if(descriptor.isEmpty()) { reply({}, {}, "Cannot create data channel"); return; }
                channel->received=[this,lease](const QJsonObject &d,const QByteArray &p) { if(onData) onData(lease,d,p); };
                channel->failed=[this,lease](const QString &e) { event(lease,{{"event","channelError"},{"message",e}}); };
                m_channels.insert(lease,std::move(channel)); reply(descriptor,{},{}); return;
            }
            if(onCall) onCall(lease,service,method,h["data"].toObject(),message.payload,std::move(reply));
            else reply({}, {}, "Service unimplemented");
    }
};
}
