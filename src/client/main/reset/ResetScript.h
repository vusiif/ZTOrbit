#pragma once
#include <QString>

namespace vusiif::zto::reset {

// Inputs for the single elevated operation. All paths are absolute and were
// produced by the host from the real system data directory (never the macOS
// temporary session directory).
struct ScriptOptions {
    QString dataRoot;
    // Ordered relative paths from Plan::removals, one per line, read by the
    // elevated script. A file keeps the command line short even when peers.d
    // contains thousands of entries.
    QString removalListPath;
    bool localConfigChanged = false;
    QString localConfigPath;
    QString proposedLocalConfigPath;
    QString expectedLocalConfigSha256;
    QString cliPath; // Used by the macOS API readiness probe.
    // Production always leaves this true. Tests use false to execute only the
    // cleanup/local.conf half against an isolated temporary tree; the generated
    // deletion code is identical in both cases.
    bool manageService = true;
};

// The scripts print "ZTO-RESET stage=<name> status=<ok|failed> [message=...]"
// lines. Stop failure is thrown before the cleanup block, so no deletion code
// can run when the service did not stop.
QString windowsResetScript(const ScriptOptions &options);
QString macResetScript(const ScriptOptions &options);
QString resetScript(const ScriptOptions &options);

} // namespace vusiif::zto::reset
