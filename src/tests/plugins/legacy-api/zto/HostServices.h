#pragma once
// Frozen API 0.3 host-services layout from before the logging extension.
#include <zto/PluginFiles.h>
#include <zto/ZeroTierService.h>
namespace vusiif::zto::util {
class HostServices {
public:
    virtual ~HostServices() = default;
    virtual PluginFiles &files() = 0;
    virtual ZeroTierService &zeroTier() = 0;
};
}
