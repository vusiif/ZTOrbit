#pragma once

#include <ztgui/PluginFiles.h>
#include <ztgui/ZeroTierService.h>
#include <ztprotocol/PluginLogger.h>

namespace vusiif::ztgui::util {
// Borrowed from onCreate until onDestroy returns. Never delete this object.
class HostServices {
public:
    virtual ~HostServices() = default;
    virtual PluginFiles &files() = 0;
    virtual ZeroTierService &zeroTier() = 0;
    // New services follow the existing virtual entries; the host owns them.
    virtual PluginLogger &logs() = 0;
};
}
