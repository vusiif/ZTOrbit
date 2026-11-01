#pragma once

#include <zto/PluginFiles.h>
#include <zto/ZeroTierService.h>
#include <ztprotocol/PluginLogger.h>

namespace vusiif::zto::util {
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
