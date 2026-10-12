#pragma once
// Frozen API 0.3 host-services layout from before the logging extension.
#include <ztgui/PluginFiles.h>
#include <ztgui/ZeroTierService.h>
namespace vusiif::ztgui::util {
class HostServices {
public:
    virtual ~HostServices() = default;
    virtual PluginFiles &files() = 0;
    virtual ZeroTierService &zeroTier() = 0;
};
}
