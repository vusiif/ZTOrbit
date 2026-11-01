#pragma once

#include <QFlags>
#include <QString>

namespace vusiif::zto::util {
class HostServices;
}

namespace vusiif::zto::desktop_share {

// Persists only the explicitly chosen server address and video profile through
// the public host file service. Room codes, media tokens and any "share on
// next start" flag are never written.
//
// Writes go through util::PluginStorage::updateFile, which reads the newest
// file under the host's cross-process coordination lock. Only the fields a
// caller marks dirty are merged; every other field keeps whatever the
// transaction just read, so a change made by another process is never rolled
// back to this process's stale snapshot. Every failure is reported to the
// caller so the page can show it instead of silently dropping the preference.
class Preferences {
public:
    struct Values {
        QString serverAddress;
        QString profile;
        QString error;
        bool loaded = false;
    };

    enum Field {
        ServerAddressField = 0x1,
        ProfileField = 0x2,
    };
    Q_DECLARE_FLAGS(Fields, Field)

    // One transactional write. A field whose dirty flag is not set is left
    // untouched at the transaction's newest value; an empty dirty address or
    // profile removes that key.
    struct Change {
        QString serverAddress;
        QString profile;
        bool serverAddressDirty = false;
        bool profileDirty = false;
    };

    explicit Preferences(util::HostServices *services);

    // Reads the current file. A missing file is not an error. A malformed file
    // returns an error and leaves the defaults untouched; it is never
    // overwritten implicitly.
    Values load();

    // Merges the dirty fields into the newest file. *merged (optional) receives
    // the resulting values. Returns false and fills *error when the host has no
    // field-merging storage, the existing file is malformed or the write fails;
    // the caller keeps its dirty flags so the write can be retried.
    bool save(const Change &change, Values *merged, QString *error);

    QString lastError() const { return m_error; }

    static QString relativePath();

private:
    util::HostServices *m_services = nullptr;
    QString m_error;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(Preferences::Fields)

} // namespace vusiif::zto::desktop_share
