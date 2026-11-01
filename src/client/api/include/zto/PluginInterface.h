#pragma once
#include <zto/PluginApi.h>
#include <QtPlugin>

namespace vusiif::zto::api {
// In-process interface: host and plugins must use matching Qt/toolchains.
class PluginInterface {
public:
    virtual ~PluginInterface() = default;
    virtual PluginDefinition definition() = 0;
};
}
#define ZTO_PLUGIN_IID "vusiif.zto.api.PluginInterface/3.0"
Q_DECLARE_INTERFACE(vusiif::zto::api::PluginInterface, ZTO_PLUGIN_IID)
