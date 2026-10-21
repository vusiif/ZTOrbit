#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace vusiif::ztgui::storage {

// Resolves the writable storage root once per process. The application
// directory is shared by every user of an installation; when it cannot be
// written, a per-user profile below QStandardPaths::AppLocalDataLocation is
// used instead. The choice is recorded for this installation and survives
// later permission changes (no automatic move back to the shared root).
class StorageContext {
public:
    enum class Mode { Shared, Personal, Ephemeral };

    StorageContext() = default;

    static StorageContext resolve(const QString &applicationDirectory = {});
    // Tests pass explicit roots so no real user settings are touched.
    static StorageContext forTesting(const QString &root);
    // Probes applicationDirectory and falls back below personalBase. Used by
    // storage tests to exercise the real selection/migration path.
    static StorageContext resolveForTesting(const QString &applicationDirectory, const QString &personalBase);

    // Normalizes a path for the install key. Windows compares case-insensitively
    // and uses backslashes; other platforms keep their filesystem semantics.
    static QString normalizedPath(const QString &path);
    static QString installKey(const QString &applicationDirectory = {});
    static QString userKey();
    static bool validPluginId(const QString &id);
    static QString pluginDataDirectoryName(const QString &pluginId);

    bool isValid() const { return !m_activeRoot.isEmpty(); }
    bool persistent() const { return m_mode != Mode::Ephemeral; }
    Mode mode() const { return m_mode; }
    bool personal() const { return m_mode == Mode::Personal; }
    QString activeRoot() const { return m_activeRoot; }
    QString sharedRoot() const { return m_sharedRoot; }
    QString personalRoot() const { return m_personalRoot; }
    QString routeFile() const { return m_routeFile; }
    // Random identifier for this process run, used by leases and cache sessions.
    QString runId() const { return m_runId; }
    QString installKeyValue() const { return m_installKey; }
    QString userKeyValue() const { return m_userKey; }
    QString pluginPackageCacheRoot() const;
    QString settingsFile() const;
    QString logsDirectory() const;
    QString pluginDataDirectory() const;
    QString pluginDataDirectory(const QString &pluginId) const;
    QString cacheDirectory() const;
    QString metadataDirectory() const;
    QByteArray ownershipRecord() const;
    QStringList warnings() const { return m_warnings; }
    QString error() const { return m_error; }
    // False while a shared-root migration still has unsettled items. prepare()
    // keeps returning true (the context is usable), but consumers must not
    // write defaults into a plugin directory that is still waiting for a copy.
    bool migrationComplete() const { return m_migrationComplete; }
    bool migrationCorrupt() const { return m_migrationCorrupt; }
    // True while the "settings" migration item is unsettled: the host INI does
    // not exist yet (or could not be restored), so creating it with defaults
    // would make the retry see an existing target and skip the original file.
    bool settingsMigrationPending() const { return m_settingsMigrationPending; }

    // Creates the directory layout and the host ownership record. Returns false
    // only when the context has no usable root at all; ownership conflicts and
    // migration problems are reported through error()/warnings().
    bool prepare(QString &error);

private:
    QString m_activeRoot;
    QString m_sharedRoot;
    QString m_personalRoot;
    QString m_routeFile;
    QString m_installKey;
    QString m_userKey;
    QString m_runId;
    Mode m_mode = Mode::Shared;
    bool m_selectedPersonal = false;
    bool m_hadRoute = false;
    bool m_migrationComplete = true;
    bool m_migrationCorrupt = false;
    bool m_settingsMigrationPending = false;
    QString m_error;
    QStringList m_warnings;

    bool selectRoot(QString &error);
    static StorageContext resolveInternal(const QString &applicationDirectory, const QString &personalBase);
    bool probeWritable(const QString &root, QString &error);
    bool prepareLayout(QString &error);
    bool writeOwnership(QString &error);
    bool writeRoute(QString &error);
    QString migrationFile() const;
    bool migrateSharedData(QString &error);
    bool migrateItem(const QString &key, const QString &source, const QString &target, const QString &kind,
                     bool directory, QJsonObject &record);
    void addWarning(const QString &message);
};

} // namespace vusiif::ztgui::storage
