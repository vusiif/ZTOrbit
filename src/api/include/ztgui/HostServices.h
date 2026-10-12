#pragma once

#include <ztgui/PluginFiles.h>
#include <ztgui/ZeroTierService.h>

namespace vusiif::ztgui::util {
// Borrowed from onCreate until onDestroy returns. Never delete this object.
class HostServices {
public:
    virtual ~HostServices() = default;
    virtual PluginFiles &files() = 0;
    virtual ZeroTierService &zeroTier() = 0;
};
}
