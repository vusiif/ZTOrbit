#pragma once

#include <QJsonDocument>
#include <QString>
#include <functional>
#include <ztprotocol/NetworkData.h>
#include <ztprotocol/NetworkService.h>
#include <ztprotocol/NetworkDelivery.h>

class QObject;

namespace vusiif::ztgui::util {
struct JsonResult {
    QJsonDocument document;
    QString error;
    bool ok() const { return error.isEmpty() && !document.isNull(); }
};
using JsonCallback = std::function<void(const JsonResult &)>;

// Call on the GUI thread with a live GUI-thread QObject context.
// Completion is asynchronous on that thread. Destroying context cancels the
// request and releases its callback. Shutdown cancels all plugin requests.
class ZeroTierService {
public:
    virtual ~ZeroTierService() = default;
    virtual void getZTInfo(QObject *context, JsonCallback completed) = 0;
    virtual void getZTPeers(QObject *context, JsonCallback completed) = 0;
    virtual void getZTNetworks(QObject *context, JsonCallback completed) = 0;
    virtual void getZTMoons(QObject *context, JsonCallback completed) = 0;
    virtual void sendDataTo(const QString &networkId, const QString &memberId, const QByteArray &data,
                            QObject *context, SendCallback completed) = 0;
    virtual void broadcastDataTo(const QString &networkId, const QStringList &memberIds, const QByteArray &data,
                                 QObject *context, SendCallback completed) = 0;
    virtual void broadcastDataBalancedTo(const QString &networkId, const QStringList &memberIds, const QByteArray &data,
                                         QObject *context, SendCallback completed) = 0;
    virtual void getNetworkRelays(const QString &networkId, QObject *context, RelayCallback completed) = 0;
    virtual NetworkSubscription *subscribeNetworkData(const QString &networkId, QObject *context, NetworkCallback received) = 0;
};
}
