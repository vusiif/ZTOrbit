#pragma once
#include <zto/PluginServices.h>
#include <QObject>
#include <QProcess>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QPointer>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <vector>
namespace vusiif::zto::plugins {
class BrokerLease;
class ServiceBroker final : public QObject {
    Q_OBJECT
public:
    explicit ServiceBroker(QObject *parent=nullptr);
    ~ServiceBroker() override;
    void start(const QString &entry,const QString &root,const QJsonObject &manifest);
    void stop();
    bool ready() const { return m_ready; }
    bool preparing() const { return m_handshaken && !m_ready && !m_stopping; }
    bool forced() const { return m_forced; }
    QString failure() const { return m_failure; }
    bool exited() const;
    bool idle() const { return m_leases.isEmpty() && m_pending.isEmpty() && !m_dispatchDepth; }
    bool dispatching() const { return m_dispatchDepth!=0; }
    quint64 generation() const { return m_generation; }
    qint64 pid() const { return m_process.processId(); }
    bool hasLease(const QString &id) const { return m_leases.contains(id); }    std::shared_ptr<util::ServiceLease> acquire(const QString &service,const QString &api);
signals:
    void becameReady();
    void failed(const QString &error);
    void changed();
private:
    friend class BrokerLease;
    struct Pending { QPointer<QObject> context; util::ServiceCallback callback; qint64 deadline=0; QString lease; QMetaObject::Connection destroyed; };
    QLocalServer m_server;
    QLocalServer m_dataServer;
    QPointer<QLocalSocket> m_socket;
    QPointer<QLocalSocket> m_dataSocket;
    QProcess m_process;
    QTimer m_timer;
    QJsonArray m_provides;
    QMap<QString,Pending> m_pending;
    QMap<QString,std::weak_ptr<BrokerLease>> m_leases;
    QMap<QString,QList<Pending>> m_closing;
    QSet<QString> m_releasing;
    QMap<QString,quint64> m_sequences;
    QByteArray m_buffer,m_dataBuffer;
    QString m_credential,m_serverName,m_failure;
    QString m_cacheRoot;
    qint64 m_workerPid=0;
    quint64 m_generation=0,m_request=0;
    qint64 m_startDeadline=0,m_stopDeadline=0;
    bool m_ready=false,m_stopping=false,m_failed=false,m_forced=false;
    bool m_controlHello=false,m_dataHello=false;
    bool m_handshaken=false,m_deferred=false;
    int m_dispatchDepth=0;
    void *m_job=nullptr;
    void *m_startInformation=nullptr; // Borrowed QProcess creation output, consumed in started().
    void read(bool dataChannel=false);
    bool send(const QJsonObject &,const QByteArray & = {});
    void fail(const QString &error);
    void call(const QString &lease,const QString &service,const QString &method,const QJsonObject &data,
              const QByteArray &payload,QObject *context,util::ServiceCallback completed);
    void close(const QString &lease,QObject *context,util::ServiceCallback completed);
    void tryRelease(const QString &lease);
    void deliver(Pending pending,const util::ServiceResult &result);
};
// Bound to one consumer instance. Only declarations resolved by the loader are exposed.
class ServiceClient final : public util::PluginServices {
public:
    ~ServiceClient() override { closeAll(); }
    struct Provider { QPointer<ServiceBroker> broker; QJsonArray services; };
    QMap<QString,Provider> providers;
    std::shared_ptr<util::ServiceLease> acquire(const QString &,const QString &,const QString &,QString *) override;
    void closeAll();
    void beginStop() { m_closed=true; }
    bool hasCallbacks() const { return m_callbacks.use_count()>1; }
    bool drained() const;
private:
    bool m_closed=false;
    std::unique_ptr<QObject> m_callbackScope=std::make_unique<QObject>();
    std::shared_ptr<int> m_callbacks=std::make_shared<int>(0);
    std::vector<std::weak_ptr<util::ServiceLease>> m_leases;
    std::vector<std::pair<QPointer<ServiceBroker>,QString>> m_resources;
};
}
