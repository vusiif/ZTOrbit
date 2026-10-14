#pragma once

#include <ztgui/PluginApi.h>
#include <ztgui/HostServices.h>
#include <ztgui/Cancellation.h>
#include <QtPlugin>

namespace vusiif::ztgui::api {
struct CreateResult {
    bool success = true;
    QString message;
};
using CreateTask = std::function<CreateResult()>;

struct MenuLocation {
    PluginInfo plugin;
    RegisteredMenu menu; // Includes global ID, local ID and display information.
    bool builtin = false;
};

// All hooks run on the GUI thread. Only the returned CreateTask runs on a worker.
class PluginLifecycleInterface {
public:
    virtual ~PluginLifecycleInterface() = default;
    // Called once after registration. Empty task means immediately ready.
    virtual CreateTask onCreate(util::HostServices &services, const util::CancellationToken &cancellation) {
        Q_UNUSED(services); Q_UNUSED(cancellation); return {};
    }
    // Called once after tasks finish, requests are canceled and pages/factories
    // are destroyed, before the root QObject and library are released.
    virtual void onDestroy() {}
    // Called for each successful change of menu ID, including within one plugin.
    // Exit of the old menu precedes entry of the new menu, without reentry.
    virtual void onEnteredFrom(const MenuLocation &previous) { Q_UNUSED(previous); }
    virtual void onExitedTo(const MenuLocation &next) { Q_UNUSED(next); }
};
}
#define ZTGUI_PLUGIN_LIFECYCLE_IID "vusiif.ztgui.api.PluginLifecycleInterface/3.0"
Q_DECLARE_INTERFACE(vusiif::ztgui::api::PluginLifecycleInterface, ZTGUI_PLUGIN_LIFECYCLE_IID)
