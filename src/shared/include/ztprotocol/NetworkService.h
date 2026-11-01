#pragma once
#include <ztprotocol/NetworkData.h>
#include <QObject>
#include <QList>
#include <memory>

namespace vusiif::zto::util {
struct RelayInfo {
    QString nodeId, moonId, virtualIp;
    quint16 port = protocol::NetworkPort;
    double averageLatency = -1, lastLatency = -1;
    bool available = false;
    qint64 sampledAt = 0, bytesPerSecond = 0, queuedBytes = 0;
};
struct RelayResult {
    QList<RelayInfo> relays;
    QString error;
    bool ok() const { return error.isEmpty(); }
};
using RelayCallback = std::function<void(const RelayResult &)>;
struct NetworkMessage { QString networkId, senderId; QByteArray data; };
using NetworkCallback = std::function<void(const NetworkMessage &)>;
// The handle is owned by the supplied context. Cancel before releasing captures.
class NetworkSubscription : public QObject {
public:
    explicit NetworkSubscription(QObject *context) : QObject(context) {}
    virtual void cancel() = 0;
};
}
