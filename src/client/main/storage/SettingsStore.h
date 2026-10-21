#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <memory>

class QSettings;

namespace vusiif::ztgui::storage {

class StorageContext;

struct SettingsMigrationResult {
    bool attempted = false;
    bool alreadyCompleted = false;
    bool complete = true;
    int imported = 0;
    QString error;
    QStringList warnings;
};

// Host settings live in one explicitly named INI file. Keeping the file path
// explicit avoids depending on QSettings' default format/path (which plugins
// and historical sources may still use).
class SettingsStore {
public:
    explicit SettingsStore(const QString &fileName = {});
    ~SettingsStore();
    SettingsStore(const SettingsStore &) = delete;
    SettingsStore &operator=(const SettingsStore &) = delete;

    QString fileName() const;
    bool valid() const;
    bool writable() const;
    QString error() const { return m_error; }

    QVariant value(const QString &key, const QVariant &fallback = {}) const;
    void setValue(const QString &key, const QVariant &value);
    void remove(const QString &key);
    bool contains(const QString &key) const;
    QStringList allKeys() const;
    bool sync(QString &error);

    // Refuses every persistent write, including creating the file. Used while
    // the shared-root migration still has to restore this INI: a target seeded
    // with defaults would make the retry skip the original file. Reads keep
    // returning whatever is on disk (normally nothing).
    void protectFromWrites(const QString &reason);
    QString writeProtection() const { return m_writeProtection; }

    // Applies that gate for a context whose "settings" migration item is still
    // unsettled. Returns the protection reason, or an empty string.
    static QString guardStorageMigration(SettingsStore &target, const StorageContext &storage);

    // A malformed INI file is reported instead of being silently replaced by
    // defaults. QSettings only exposes this through status() after a read.
    static bool validIniFile(const QString &path, QString &error);

    // Fills missing whitelisted host settings from the current and historical
    // application names. Values present in the target always win. The source
    // key is removed only after the target write was synced and read back.
    // The completion marker lives in the host metadata directory and is owned
    // by the storage root, so later users do not re-import into a shared root.
    // Reading the marker, importing, removing source keys and writing the
    // marker all happen under the metadata root coordination lock. A busy lock
    // returns an incomplete result, keeps every source key and writes no marker.
    static SettingsMigrationResult migrateLegacy(SettingsStore &target,
                                                 const QString &metadataDirectory,
                                                 const QString &userKey);

private:
    std::unique_ptr<QSettings> m_settings;
    QString m_fileName;
    QString m_writeProtection;
    mutable QString m_error;
};

} // namespace vusiif::ztgui::storage
