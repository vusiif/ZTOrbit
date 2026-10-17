#pragma once
#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>

namespace vusiif::ztgui::util {
struct ServiceResult {
    QJsonObject data;
    QByteArray payload;
    QString error;
    bool ok() const { return error.isEmpty(); }
};
using ServiceCallback = std::function<void(const ServiceResult &)>;
// All methods and callbacks run on the host event thread. The context owns the
// callback lifetime. Binary data is framed separately from the JSON envelope.
class ServiceLease {
public:
    virtual ~ServiceLease() = default;
    virtual quint64 generation() const = 0;
    virtual bool available() const = 0;
    // Hold while dispatching callbacks on a negotiated direct data channel.
    // The host delays consumer DLL unload until all guards have been released.
    virtual std::shared_ptr<void> callbackGuard() { return {}; }
    virtual void call(const QString &method, const QJsonObject &data, const QByteArray &payload,
                      QObject *context, ServiceCallback completed) = 0;
    virtual void subscribe(QObject *context, ServiceCallback received) = 0;
    // Idempotent; completion means provider resources have been released.
    virtual void close(QObject *context, ServiceCallback completed) = 0;
};
class PluginServices {
public:
    virtual ~PluginServices() = default;
    virtual std::shared_ptr<ServiceLease> acquire(const QString &provider, const QString &service,
                                                 const QString &api, QString *error = nullptr) = 0;
};
// Separate optional interface: HostServices' existing vtable is unchanged.
// Query with dynamic_cast<ServiceAccess *>(&hostServices).
class ServiceAccess {
public:
    virtual ~ServiceAccess() = default;
    virtual PluginServices &pluginServices() = 0;
};
}
