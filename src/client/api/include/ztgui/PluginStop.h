#pragma once
#include <ztgui/PluginLifecycle.h>
namespace vusiif::ztgui::api {
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
#define ZTGUI_PLUGIN_STOP_IID "vusiif.ztgui.api.PluginStopInterface/1.0"
Q_DECLARE_INTERFACE(vusiif::ztgui::api::PluginStopInterface, ZTGUI_PLUGIN_STOP_IID)
