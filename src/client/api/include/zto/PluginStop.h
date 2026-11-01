#pragma once
#include <zto/PluginLifecycle.h>
namespace vusiif::zto::api {
// Called on the GUI thread before pages/factories are destroyed. Complete only
// after sessions, callbacks and business threads have drained. The host queues
// completion onto its event thread. Failure retains
// this DLL and its providers. onDestroy then performs final, bounded cleanup.
class PluginStopInterface {
public:
    virtual ~PluginStopInterface() = default;
    virtual void requestStop(QObject *context, std::function<void(CreateResult)> completed) = 0;
};
}
#define ZTO_PLUGIN_STOP_IID "vusiif.zto.api.PluginStopInterface/1.0"
Q_DECLARE_INTERFACE(vusiif::zto::api::PluginStopInterface, ZTO_PLUGIN_STOP_IID)
