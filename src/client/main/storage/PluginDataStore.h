#pragma once

#include "CoordinationLock.h"
#include "ztgui/PluginStorage.h"

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace vusiif::ztgui::storage {

using util::LegacyImportState;
using util::StorageResult;

// Owns the stable-ID plugin data layout, host-owned plugin metadata, legacy
// display-name migration and per-user plugin leases.
class PluginDataStore {
public:
    // Display-name claims of every authenticated package in the same install
    // directory. Registration also writes them to .ztgui/plugins/<id>.json; the
    // in-memory copy keeps the migration decision correct when that write is
    // impossible or when one record cannot be read.
    struct InstalledNameClaims {
        QList<QPair<QString, QString>> installed; // stable id, declared name
        QStringList issues;                       // registration problems
        bool authoritative = false;               // installed covers the directory
    };

    PluginDataStore(QString pluginsDataRoot, QString metadataRoot, QString runId, QString userKey);

    QString pluginsDataRoot() const { return m_pluginsDataRoot; }
    QString metadataDirectory() const { return m_metadataRoot; }
    QString runId() const { return m_runId; }
    QString userKey() const { return m_userKey; }
    QStringList warnings() const { return m_warnings; }

    static QString directoryName(const QString &pluginId);
    QString dataDirectory(const QString &pluginId) const;
    // Registers ownership, acquires this run's lease and migrates a unique
    // legacy display-name directory when that is unambiguous.
    QString prepare(const QString &pluginId, const QString &displayName, QString &error);
    void release(const QString &pluginId);

    LegacyImportState legacyImportState(const QString &pluginId) const;
    // When the caller already holds the coordination lock it must pass it here;
    // acquiring it again would violate the fixed lock order.
    StorageResult completeLegacyImport(const QString &pluginId, CoordinationLock *held = nullptr);

    // Records the display-name ownership declared by every authenticated
    // installed package before any instance may migrate a legacy directory.
    // Two packages that share a display name then block the automatic
    // migration for both instead of the first one claiming the data. The
    // returned claims are also the in-memory truth for consumers that cannot
    // rely on the metadata write.
    InstalledNameClaims registerInstalledNames(const QList<QPair<QString, QString>> &installed) const;
    // Must be called before prepare() with the scan-wide claims when they are
    // available; without them an unreadable ownership record anywhere in the
    // root is treated as an unconfirmed claim.
    void setInstalledNameClaims(const InstalledNameClaims &claims) { m_nameClaims = claims; }
    const InstalledNameClaims &installedNameClaims() const { return m_nameClaims; }
    // Non-empty while a pending/failed shared-root migration still owns this
    // plugin's persistent data directory. Writes must be refused until the
    // item settles so defaults never occupy the migration target.
    QString pendingMigration(const QString &pluginId, const QString &displayName) const;

    bool hasOtherLeases(const QString &pluginId, QString &error) const;
    bool deleteDataDirectory(const QString &pluginId, QString &error, CoordinationLock *held = nullptr);

    QJsonObject ownershipRecord(const QString &pluginId, QString &error) const;

    bool acquireCoordination(CoordinationLock &lock, QString &error) const;

private:
    QString m_pluginsDataRoot;
    QString m_metadataRoot;
    QString m_runId;
    QString m_userKey;
    InstalledNameClaims m_nameClaims;
    mutable QStringList m_warnings;
    QString ownershipFile(const QString &pluginId) const;
    QString leaseFile(const QString &pluginId) const;
    QStringList legacyCandidates(const QJsonObject &ownership, const QString &displayName) const;
    bool nameClaimedByOther(const QString &pluginId, const QString &name) const;
    bool nameClaimedByInstalled(const QString &pluginId, const QString &name) const;
    // Empty when every installed ID's display-name ownership can be confirmed,
    // otherwise the reason why an automatic legacy migration must wait.
    QString ownershipUncertainty() const;
    bool writeOwnership(const QString &pluginId, const QString &displayName, QString &error) const;
    QJsonObject readOwnership(const QString &pluginId, QString &error) const;
    void addWarning(const QString &warning) const;
};

} // namespace vusiif::ztgui::storage
