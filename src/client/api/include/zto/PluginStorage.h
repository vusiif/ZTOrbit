#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

namespace vusiif::zto::util {
// Optional host extension for coordinated plugin storage. Query it with
// dynamic_cast<util::PluginStorage *>(&host.files()). It does not extend the
// shared PluginFiles vtable.
struct PluginFileSnapshot {
    bool exists = false;
    QByteArray data;
};

struct PluginFileUpdate {
    enum class Action { Keep, Replace, Fail };
    Action action = Action::Keep;
    QByteArray data;
    QString error;

    static PluginFileUpdate keep() { return {}; }
    static PluginFileUpdate replace(QByteArray value) { return {Action::Replace, std::move(value), {}}; }
    static PluginFileUpdate failure(QString message) { return {Action::Fail, {}, std::move(message)}; }
};

struct PluginFileTransaction {
    bool ok = false;
    bool changed = false;
    QByteArray data;
    QString error;
};

struct LegacyImportState {
    bool allowed = false;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

struct StorageResult {
    QString error;
    bool ok() const { return error.isEmpty(); }
};

using PluginFileUpdater = std::function<PluginFileUpdate(const PluginFileSnapshot &)>;

// All calls are thread-safe. updateFile reads the latest file under the host's
// cross-process coordination lock, calls the updater with a snapshot, and only
// commits a Replace action atomically. The updater must do in-memory
// conversion only: it must not re-enter the file service, touch UI/network or
// be retained by the caller. Exceptions keep the original file. An empty
// Replace payload is a real empty file, not "keep".
class PluginStorage {
public:
    virtual ~PluginStorage() = default;
    virtual PluginFileTransaction updateFile(const QString &relativePath,
                                             const PluginFileUpdater &updater) = 0;
    // A plugin may import legacy host settings once. A missing or unreadable
    // host metadata record is never treated as permission by accident.
    virtual LegacyImportState legacyImportAllowed() = 0;
    // One-way and idempotent. Records the current user in host-owned metadata.
    virtual StorageResult completeLegacyImport() = 0;
};
}
