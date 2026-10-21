#include "ServiceBroker.h"
#include "PluginPackage.h"
#include <ztprotocol/ServiceWire.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QUuid>
#include <QScopedValueRollback>
#include <QThread>
#include <QEvent>
#include <algorithm>
#include <exception>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace vusiif::ztgui::plugins {
using namespace util;
static void discardCallbackScope(std::unique_ptr<QObject> &scope) {
    if(!scope) return;
    auto *object=scope.release();
    QCoreApplication::removePostedEvents(object,QEvent::MetaCall);
    object->deleteLater(); // A callback can be deleting its own receiver scope.
}
class BrokerLease final : public QObject, public ServiceLease {
public:
    QPointer<ServiceBroker> broker;
    QString id,service;
    quint64 epoch=0;
    bool closed=false;
    QPointer<QObject> eventContext;
    ServiceCallback eventCallback;
    QPointer<QObject> consumerScope=this;
    std::weak_ptr<int> consumerCallbacks;
    std::unique_ptr<QObject> errorScope=std::make_unique<QObject>();
    std::vector<std::weak_ptr<int>> deferred;
    bool overflowReported=false;
    ~BrokerLease() override { discardCallbackScope(errorScope); if(broker && !closed) broker->close(id,nullptr,{}); }
    void post(QObject *scope,QObject *context,ServiceCallback callback,const ServiceResult &result) {
        if(!scope || !context || !callback) return;
        deferred.erase(std::remove_if(deferred.begin(),deferred.end(),[](const auto &v) { return v.expired(); }),deferred.end());
        if(deferred.size()>=128) {
            if(broker && !overflowReported) {
                overflowReported=true;
                QTimer::singleShot(0,broker,[target=broker] { if(target) target->fail("Service rejection queue limit exceeded"); });
            }
            return;
        }
        auto marker=std::make_shared<int>(0); deferred.push_back(marker);
        // The consumer token keeps a nested event loop from unloading its DLL.
        // The receiver scope cancels the function object even for a host-owned context.
        QTimer::singleShot(0,scope,[ctx=QPointer<QObject>(context),callback=std::move(callback),result,
                                   marker,active=consumerCallbacks.lock(),target=broker] {
            if(!ctx) return;
            if(target) {
                const QScopedValueRollback<int> guard(target->m_dispatchDepth,target->m_dispatchDepth+1);
                try { callback(result); } catch(...) { target->fail("Deferred service callback failed"); }
            } else { try { callback(result); } catch(...) { qWarning()<<"Deferred service callback failed"; } }
        });
    }
    void reject(QObject *context,ServiceCallback callback,const QString &error) { post(errorScope.get(),context,std::move(callback),{{},{},error}); }
    void completeClose(QObject *context,ServiceCallback callback) { post(consumerScope,context,std::move(callback),{}); }
    quint64 generation() const override { return epoch; }
    bool available() const override { return !closed && broker && broker->ready() && broker->generation()==epoch; }
    std::shared_ptr<void> callbackGuard() override { return consumerCallbacks.lock(); }
    void call(const QString &method,const QJsonObject &data,const QByteArray &payload,QObject *context,ServiceCallback callback) override {
        if(QThread::currentThread()!=thread() || !context || context->thread()!=thread()) return;
        if(!available()) { reject(context,std::move(callback),"Service lease unavailable"); return; }
        broker->call(id,service,method,data,payload,context,std::move(callback));
    }
    void subscribe(QObject *context,ServiceCallback callback) override {
        if(!available() || !context || QThread::currentThread()!=thread() || context->thread()!=thread()) return;
        eventContext=context; eventCallback=std::move(callback);
        if(context) QObject::connect(context,&QObject::destroyed,this,[this] { eventCallback={}; eventContext=nullptr; });
    }
    void close(QObject *context,ServiceCallback callback) override {
        eventCallback={}; eventContext=nullptr;
        discardCallbackScope(errorScope);
        closed=true;
        if(!broker || !broker->hasLease(id)) { completeClose(context,std::move(callback)); return; }
        broker->close(id,context,std::move(callback));
    }
};
ServiceBroker::ServiceBroker(QObject *parent):QObject(parent) {
    m_generation=QRandomGenerator::global()->generate64();
    // Decimal strings on the wire avoid JSON's 53-bit integer limit.
    connect(&m_server,&QLocalServer::newConnection,this,[this] {
        auto *socket=m_server.nextPendingConnection();
        if(m_socket || m_stopping) { socket->abort(); socket->deleteLater(); return; }
        m_socket=socket; socket->setParent(this); socket->setReadBufferSize(protocol::service::MaxFrame*2);
        connect(socket,&QLocalSocket::readyRead,this,[this] { read(false); });
        connect(socket,&QLocalSocket::disconnected,this,[this] { if(!m_stopping) fail("Worker IPC disconnected"); });
        if(socket->bytesAvailable()) read(false);
    });
    connect(&m_dataServer,&QLocalServer::newConnection,this,[this] {
        auto *socket=m_dataServer.nextPendingConnection();
        if(m_dataSocket || m_stopping) { socket->abort(); socket->deleteLater(); return; }
        m_dataSocket=socket; socket->setParent(this); socket->setReadBufferSize(protocol::service::MaxFrame*2);
        connect(socket,&QLocalSocket::readyRead,this,[this] { read(true); });
        connect(socket,&QLocalSocket::disconnected,this,[this] { if(!m_stopping) fail("Worker data IPC disconnected"); });
        if(socket->bytesAvailable()) read(true);
    });
    connect(&m_process,&QProcess::started,this,[this] {
#ifdef Q_OS_WIN
        // QProcess owns the creation output until process cleanup. Use its
        // exact handles; enumerating threads can select an injected/helper one.
        auto *info=static_cast<PROCESS_INFORMATION *>(std::exchange(m_startInformation,nullptr));
        if(!info || !info->hProcess || !info->hThread || !m_job || !AssignProcessToJobObject(HANDLE(m_job),info->hProcess)) {
            fail(QString("Cannot bind worker to host job (Win32 %1)").arg(GetLastError())); return;
        }
        const auto count=ResumeThread(info->hThread);
        if(count!=1) { fail(QString("Cannot resume worker primary thread (suspend count %1, Win32 %2)").arg(count).arg(GetLastError())); return; }
        qInfo()<<"[worker] Owned process started; pid"<<m_process.processId();
#endif
        m_workerPid=m_process.processId();
        const auto ownershipError=package::registerCacheWorker(m_cacheRoot,m_workerPid);
        if(!ownershipError.isEmpty()) { fail(QString("Cannot record worker ownership: %1").arg(ownershipError)); return; }
        emit changed();
    });
    connect(&m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError) { if(!m_stopping) fail(m_process.errorString()); });
    connect(&m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus status) {
        if(m_workerPid>0) { package::unregisterCacheWorker(m_cacheRoot,m_workerPid); m_workerPid=0; }
        m_ready=false;
        if(!m_stopping) fail(QString("Worker exited (%1, %2)").arg(code).arg(int(status)));
        else emit changed();
    });
    // Bound diagnostics; no accumulation of worker stdout/stderr.
    connect(&m_process,&QProcess::readyReadStandardOutput,this,[this] { m_process.readAllStandardOutput(); });
    connect(&m_process,&QProcess::readyReadStandardError,this,[this] { const auto bytes=m_process.readAllStandardError(); if(!bytes.isEmpty()) qWarning().noquote()<<"[worker]"<<QString::fromUtf8(bytes.left(4096)); });
    m_timer.setInterval(100);
    connect(&m_timer,&QTimer::timeout,this,[this] {
        const auto now=QDateTime::currentMSecsSinceEpoch();
        if(!m_ready && !m_stopping && now>m_startDeadline) {
            if(m_handshaken) fail(QString("Worker service preparation timed out (60 s, pid=%1)").arg(m_process.processId()));
            else fail(QString("Worker handshake timed out (control=%1, data=%2, pid=%3)").arg(m_controlHello).arg(m_dataHello).arg(m_process.processId()));
        }
        const auto keys=m_pending.keys();
        for(const auto &id:keys) if(m_pending.contains(id) && now>m_pending[id].deadline) {
            const auto pending=m_pending.take(id); deliver(pending,{{}, {}, "Service request timed out"});
            // Unknown remote resource state cannot be treated as released.
            fail("Service request timed out"); break;
        }
        if(m_stopping && !exited() && now>m_stopDeadline && !m_forced) {
            m_forced=true;
#ifdef Q_OS_WIN
            if(m_job) TerminateJobObject(HANDLE(m_job),1);
#endif
            m_process.kill(); qWarning()<<"[worker] Forced owned process termination";
        }
        if(m_stopping && exited()) { m_timer.stop(); emit changed(); }
    });
}
ServiceBroker::~ServiceBroker() {
    // Emergency process teardown only. Normal shutdown has already observed exit.
#ifdef Q_OS_WIN
    if(m_job) { CloseHandle(HANDLE(m_job)); m_job=nullptr; }
#endif
    if(m_process.state()!=QProcess::NotRunning) { m_process.kill(); m_process.waitForFinished(3000); }
    if(m_workerPid>0) { package::unregisterCacheWorker(m_cacheRoot,m_workerPid); m_workerPid=0; }
    m_server.close(); m_dataServer.close();
}
void ServiceBroker::start(const QString &entry,const QString &root,const QJsonObject &manifest) {
    m_cacheRoot=root;
    m_provides=manifest["provides"].toArray();
    m_credential=QUuid::createUuid().toString(QUuid::Id128)+QUuid::createUuid().toString(QUuid::Id128);
    m_serverName="ztgui-"+QUuid::createUuid().toString(QUuid::Id128);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if(!m_server.listen(m_serverName)) { fail(m_server.errorString()); return; }
    m_dataServer.setSocketOptions(QLocalServer::UserAccessOption);
    if(!m_dataServer.listen(m_serverName+"-data")) { fail(m_dataServer.errorString()); return; }
#ifdef Q_OS_WIN
    m_job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!m_job || !SetInformationJobObject(HANDLE(m_job),JobObjectExtendedLimitInformation,&limits,sizeof limits)) { fail("Cannot create worker ownership job"); return; }
    m_process.setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments *args) {
        args->flags|=CREATE_NO_WINDOW|CREATE_SUSPENDED;
        m_startInformation=args->processInformation;
    });
#endif
    auto env=QProcessEnvironment::systemEnvironment();
    // Child-only environment. Never add worker libraries/plugins to the host.
    env.insert("PATH",QDir::toNativeSeparators(QFileInfo(entry).absolutePath())+";"+QDir::toNativeSeparators(root+"/runtime/bin")+";"+env.value("SystemRoot")+"/System32");
    env.insert("QT_PLUGIN_PATH",root+"/runtime/qt-plugins"); env.remove("QML_IMPORT_PATH"); env.remove("QML2_IMPORT_PATH");
    env.insert("QT_QPA_PLATFORM_PLUGIN_PATH",root+"/runtime/qt-plugins/platforms");
    env.remove("QT_QPA_PLATFORM"); // A headless parent test must not disable child capture.
    env.insert("QT_FORCE_STDERR_LOGGING","1"); // CREATE_NO_WINDOW must still forward Qt diagnostics through the pipe.
    env.insert("ZTGUI_SERVICE_TOKEN",m_credential);
    m_process.setProcessEnvironment(env); m_process.setWorkingDirectory(root);
    m_process.setProgram(entry); m_process.setArguments({"--service-socket",m_serverName,"--service-generation",QString::number(m_generation),"--host-pid",QString::number(QCoreApplication::applicationPid())});
    m_startDeadline=QDateTime::currentMSecsSinceEpoch()+15000; m_timer.start(); m_process.start();
}
bool ServiceBroker::send(const QJsonObject &header,const QByteArray &payload) {
    auto socket=payload.isEmpty()?m_socket:m_dataSocket;
    if(!socket || socket->state()!=QLocalSocket::ConnectedState || socket->bytesToWrite()>1024*1024) return false;
    const auto bytes=protocol::service::frame(header,payload); return !bytes.isEmpty() && socket->write(bytes)==bytes.size();
}
void ServiceBroker::deliver(Pending pending,const ServiceResult &result) {
    QObject::disconnect(pending.destroyed);
    if(!pending.context || !pending.callback) return;
    const QScopedValueRollback<int> guard(m_dispatchDepth,m_dispatchDepth+1);
    try { pending.callback(result); }
    catch(const std::exception &e) { fail("Service callback failed: "+QString::fromUtf8(e.what())); }
    catch(...) { fail("Service callback threw an exception"); }
}
void ServiceBroker::read(bool dataChannel) {
    const QScopedValueRollback<int> dispatch(m_dispatchDepth,m_dispatchDepth+1);
    auto socket=dataChannel?m_dataSocket:m_socket; if(!socket) return;
    if(m_stopping) { socket->readAll(); return; }
    auto &buffer=dataChannel?m_dataBuffer:m_buffer;
    buffer+=socket->readAll();
    for(;;) {
        protocol::service::Message msg; const auto status=protocol::service::take(buffer,msg);
        if(status==0) break;
        if(status<0) { fail("Malformed service frame"); return; }
        const auto &h=msg.header;
        if(!m_ready) {
            if(m_handshaken) {
                if(dataChannel || h["type"]!="ready" || h["generation"].toString()!=QString::number(m_generation) || !msg.payload.isEmpty()) {
                    fail("Invalid worker preparation completion"); return;
                }
                m_ready=true; qInfo()<<"[worker] Service preparation completed; pid"<<m_process.processId();
                emit becameReady(); emit changed(); continue;
            }
            if(m_stopping || h["type"]!="hello" || h["token"]!=m_credential || h["protocol"].toString()!=protocol::service::Protocol
                || h["generation"].toString()!=QString::number(m_generation) || h["pid"].toString()!=QString::number(m_process.processId()) || h["provides"]!=m_provides
                || h["channel"].toString()!=(dataChannel?"data":"control") || (dataChannel?m_dataHello:m_controlHello)) { fail("Worker handshake/capability mismatch"); return; }
            const bool deferred=h["preparing"].toBool();
            if((h.contains("preparing") && !h["preparing"].isBool()) || ((m_controlHello || m_dataHello) && deferred!=m_deferred)) { fail("Worker preparation capability mismatch"); return; }
            m_deferred=deferred;
            (dataChannel?m_dataHello:m_controlHello)=true;
            if(m_controlHello && m_dataHello) {
                m_handshaken=true; m_credential.clear(); m_server.close(); m_dataServer.close();
                const auto accepted=protocol::service::frame({{"type","accepted"},{"generation",QString::number(m_generation)},{"deferredReady",true}});
                m_socket->write(accepted); m_dataSocket->write(accepted);
                if(m_deferred) {
                    m_startDeadline=QDateTime::currentMSecsSinceEpoch()+60000;
                    qInfo()<<"[worker] IPC authenticated; waiting for service preparation; pid"<<m_process.processId();
                } else { m_ready=true; emit becameReady(); }
                emit changed();
            }
            continue;
        }
        if(h["generation"].toString()!=QString::number(m_generation)) { fail("Stale worker generation"); return; }
        const auto id=h["request"].toString();
        if(h["type"]=="reply") {
            if(!m_pending.contains(id)) continue;
            auto pending=m_pending.take(id);
            deliver(std::move(pending),{h["data"].toObject(),msg.payload,h["error"].toString()});
            for(const auto &lease:m_closing.keys()) tryRelease(lease);
            emit changed();
        } else if(h["type"]=="event") {
            auto lease=m_leases.value(h["lease"].toString()).lock();
            if(lease && lease->available() && lease->eventContext && lease->eventCallback) {
                deliver({lease->eventContext,lease->eventCallback,0,{},{}},{h["data"].toObject(),msg.payload,h["error"].toString()});
            }
        } else { fail("Unknown service message"); return; }
    }
    if(buffer.size()>protocol::service::MaxFrame) fail("Oversized service buffer");
}
void ServiceBroker::fail(const QString &error) {
    if(m_failed) return; m_failed=true; m_ready=false;
    m_failure=error;
    qWarning().noquote()<<"[worker]"<<error;
    const QScopedValueRollback<int> dispatch(m_dispatchDepth,m_dispatchDepth+1);
    auto pending=std::move(m_pending); m_pending.clear();
    const auto leases=m_leases; m_leases.clear(); m_sequences.clear();
    for(auto &p:pending) deliver(std::move(p),{{},{},error});
    const auto closing=std::move(m_closing); m_closing.clear(); m_releasing.clear();
    for(const auto &waiters:closing) for(const auto &waiter:waiters) deliver(waiter,{{},{},error});
    for(const auto &weak:leases) if(auto lease=weak.lock()) {
        auto cb=std::move(lease->eventCallback); auto ctx=lease->eventContext; lease->closed=true;
        if(ctx && cb) deliver({ctx,std::move(cb),0,{},{}},{{},{},error});
    }
    emit failed(error); stop();
}
void ServiceBroker::stop() {
    if(m_stopping) return; m_stopping=true;
    send({{"type","shutdown"},{"generation",QString::number(m_generation)}});
    m_ready=false; m_server.close(); m_dataServer.close(); m_stopDeadline=QDateTime::currentMSecsSinceEpoch()+5000;
    m_timer.start(); emit changed();
}
bool ServiceBroker::exited() const {
    if(m_process.state()!=QProcess::NotRunning) return false;
#ifdef Q_OS_WIN
    if(m_job) { JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{}; if(!QueryInformationJobObject(HANDLE(m_job),JobObjectBasicAccountingInformation,&info,sizeof info,nullptr) || info.ActiveProcesses) return false; }
#endif
    return true;
}
std::shared_ptr<ServiceLease> ServiceBroker::acquire(const QString &service,const QString &api) {
    if(!m_ready || m_leases.size()>=128 || !m_provides.contains(QJsonObject{{"service",service},{"api",api}})) return {};
    auto lease=std::make_shared<BrokerLease>(); lease->broker=this; lease->service=service; lease->epoch=m_generation;
    lease->id=QUuid::createUuid().toString(QUuid::Id128); m_leases.insert(lease->id,lease); emit changed(); return lease;
}
void ServiceBroker::call(const QString &lease,const QString &service,const QString &method,const QJsonObject &data,const QByteArray &payload,QObject *context,ServiceCallback callback) {
    if(!context || context->thread()!=thread()) return;
    if(m_pending.size()>=128 || !m_ready) { if(auto owner=m_leases.value(lease).lock()) owner->reject(context,std::move(callback),"Service unavailable or busy"); return; }
    const auto request=QString::number(++m_request);
    m_pending.insert(request,{context,std::move(callback),QDateTime::currentMSecsSinceEpoch()+10000,lease});
    m_pending[request].destroyed=connect(context,&QObject::destroyed,this,[this,request] { if(m_pending.contains(request)) { m_pending[request].callback={}; m_pending[request].context=nullptr; } });
    if(!send({{"type","call"},{"generation",QString::number(m_generation)},{"request",request},{"sequence",QString::number(++m_sequences[lease])},{"lease",lease},{"service",service},{"method",method},{"data",data}},payload))
        QTimer::singleShot(0,this,[this] { fail("Service write failed"); });
}
void ServiceBroker::close(const QString &lease,QObject *context,ServiceCallback callback) {
    if(!m_leases.contains(lease) || !m_ready) { auto owner=m_leases.value(lease).lock(); m_leases.remove(lease); if(owner) owner->completeClose(context,std::move(callback)); return; }
    const bool first=!m_closing.contains(lease);
    auto &waiters=m_closing[lease]; if(context && callback) waiters.append({context,std::move(callback),0,lease,{}});
    if(first) {
        // Drain calls on both physical channels before releasing a session.
        for(auto &p:m_pending) if(p.lease==lease) { p.callback={}; p.context=nullptr; }
    }
    tryRelease(lease);
}
void ServiceBroker::tryRelease(const QString &lease) {
    if(!m_ready || !m_closing.contains(lease) || m_releasing.contains(lease)) return;
    for(const auto &p:m_pending) if(p.lease==lease) return;
    m_releasing.insert(lease);
    call(lease,{},"$release",{}, {},this,[this,lease](const ServiceResult &result) {
        if(result.ok()) { m_leases.remove(lease); m_sequences.remove(lease); }
        m_releasing.remove(lease); const auto waiters=m_closing.take(lease);
        for(const auto &waiter:waiters) deliver(waiter,result);
        emit changed();
    });
}
std::shared_ptr<ServiceLease> ServiceClient::acquire(const QString &provider,const QString &service,const QString &api,QString *error) {
    const auto p=providers.value(provider.toCaseFolded());
    if(m_closed || !p.broker || QThread::currentThread()!=p.broker->thread() || !p.services.contains(QJsonObject{{"service",service},{"api",api}})) { if(error) *error="Service not declared, provider unavailable or consumer stopping"; return {}; }
    m_resources.erase(std::remove_if(m_resources.begin(),m_resources.end(),[](const auto &r) { return !r.first || !r.first->hasLease(r.second); }),m_resources.end());
    m_leases.erase(std::remove_if(m_leases.begin(),m_leases.end(),[](const auto &l) { return l.expired(); }),m_leases.end());
    if(m_resources.size()>=32) { if(error) *error="Consumer lease limit reached"; return {}; }
    auto lease=p.broker->acquire(service,api); if(!lease) { if(error) *error="Provider is not ready"; return {}; }
    auto implementation=std::static_pointer_cast<BrokerLease>(lease);
    implementation->consumerScope=m_callbackScope.get(); implementation->consumerCallbacks=m_callbacks;
    m_leases.push_back(lease); m_resources.emplace_back(p.broker,std::static_pointer_cast<BrokerLease>(lease)->id); return lease;
}
void ServiceClient::closeAll() {
    m_closed=true;
    for(const auto &weak:m_leases) if(auto lease=weak.lock()) {
        std::static_pointer_cast<BrokerLease>(lease)->consumerScope=nullptr;
        lease->close(nullptr,{});
    }
    discardCallbackScope(m_callbackScope);
}
bool ServiceClient::drained() const {
    for(const auto &weak:m_leases) if(auto generic=weak.lock()) {
        const auto lease=std::static_pointer_cast<BrokerLease>(generic);
        if(lease->broker && lease->broker->hasLease(lease->id)) return false;
    }
    // Only this consumer's resources block it; another consumer may stay active.
    for(const auto &p:m_resources) if(p.first && p.first->hasLease(p.second)) return false;
    return true;
}
}
