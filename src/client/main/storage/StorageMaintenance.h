#pragma once

#include "StorageContext.h"
#include "PluginDataStore.h"

#include <QJsonDocument>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <algorithm>

namespace vusiif::zto::storage {
struct StorageUsage {
    QString root;
    QString settings;
    QString logs;
    QString pluginData;
    QString cache;
    QString mode;
    qint64 logsBytes = 0;
    qint64 pluginDataBytes = 0;
    qint64 cacheBytes = 0;
    int logFiles = 0;
    QStringList warnings;
};

struct OrphanItem {
    QString id;
    QString directory;
    QString path;
    qint64 bytes = 0;
    bool deletable = false;
    QString reason;
};

struct CleanupReport {
    qint64 releasedBytes = 0;
    QStringList released;
    QStringList skipped;
    QStringList failed;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Non-destructive result of applying the same retention rules as
// cleanupLogs()/cleanupCaches(). Used to show pending work before confirmation.
struct RetentionPreview {
    int logFiles = 0;
    qint64 logBytes = 0;
    int cacheFiles = 0;
    qint64 cacheBytes = 0;
    QString error;
    bool empty() const { return logFiles == 0 && cacheFiles == 0; }
};

// Host maintenance for logs, disposable plugin caches and unowned plugin data.
// All destructive operations validate ownership and skip links/unknown actors.
class StorageMaintenance {
public:
    explicit StorageMaintenance(const StorageContext &context);
    StorageUsage usage() const;
    RetentionPreview previewRetention() const;
    // installedIds includes disabled, incompatible and failed installed packages.
    // When uncertainPackages is true an installed package exists whose ID could
    // not be authenticated; stable-ID data is then ambiguous and never deletable.
    QList<OrphanItem> orphans(const QStringList &installedIds, bool uncertainPackages = false) const;
    CleanupReport cleanupLogs();
    CleanupReport cleanupCaches();
    CleanupReport cleanupOrphanData(const QStringList &ids);
    static qint64 directoryBytes(const QString &path);

private:
    StorageContext m_context;
};

} // namespace vusiif::zto::storage
