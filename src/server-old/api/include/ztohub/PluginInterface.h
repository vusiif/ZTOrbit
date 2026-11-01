#pragma once
#include <ztohub/ApiVersion.h>
#include <ztohub/HostServices.h>
#include <ztprotocol/Cancellation.h>
#include <QtPlugin>

namespace vusiif::ztohub::api {
struct PluginInfo {
    QString id, name, version;
    int apiMajor = ApiMajor;
    int apiMinor = ApiMinor;
};
struct StartResult { bool success = true; QString message; };
using StartTask = std::function<StartResult()>;
class PluginInterface {
public:
    virtual ~PluginInterface() = default;
    virtual PluginInfo info() const = 0;
    // onStart runs on the event thread. Only its optional task runs in a worker.
    virtual StartTask onStart(HostServices &, const vusiif::zto::util::CancellationToken &) { return {}; }
    virtual void onTcpPacket(ConnectionId, const QByteArray &) {}
    virtual void onUdpPacket(const Endpoint &, const QByteArray &) {}
    virtual void onConnectionClosed(ConnectionId) {}
    // Called after tasks finish and queries are cancelled, before library release.
    virtual void onStop() {}
};
}
#define ZTOHUB_PLUGIN_IID "vusiif.ztohub.api.PluginInterface/2.0"
Q_DECLARE_INTERFACE(vusiif::ztohub::api::PluginInterface, ZTOHUB_PLUGIN_IID)
