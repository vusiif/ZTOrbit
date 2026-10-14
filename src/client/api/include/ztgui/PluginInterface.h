#pragma once
#include <ztgui/PluginApi.h>
#include <QtPlugin>

namespace vusiif::ztgui::api {
// In-process interface: host and plugins must use matching Qt/toolchains.
class PluginInterface {
public:
    virtual ~PluginInterface() = default;
    virtual PluginDefinition definition() = 0;
};
}
#define ZTGUI_PLUGIN_IID "vusiif.ztgui.api.PluginInterface/3.0"
Q_DECLARE_INTERFACE(vusiif::ztgui::api::PluginInterface, ZTGUI_PLUGIN_IID)
