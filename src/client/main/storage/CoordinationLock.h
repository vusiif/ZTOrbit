#pragma once

#include <QLockFile>
#include <QString>

namespace vusiif::ztgui::storage {

// Short-lived cross-process lock below <active root>/.ztgui. It serializes
// lease checks/registration, migration metadata, file transactions and cache
// cleanup. Lock order is always the install-directory package change lock
// first, then this coordination lock; never acquire the package lock while
// holding this one.
class CoordinationLock {
public:
    explicit CoordinationLock(const QString &metadataRoot);
    ~CoordinationLock();
    CoordinationLock(const CoordinationLock &) = delete;
    CoordinationLock &operator=(const CoordinationLock &) = delete;
    bool acquire(QString &error, int timeoutMs = 5000);
    void release();
    bool held() const { return m_held; }
    QString fileName() const { return m_lock.fileName(); }

private:
    QLockFile m_lock;
    bool m_held = false;
};

// Per-file activity lock below <metadata root>/read-locks. Business-cache
// readers hold it for the duration of the read; cleanup tries it while holding
// the short coordination lock and skips a file that is still in use instead of
// blocking every writer for the whole read. The path is stable across
// processes for the same absolute file.
QString cacheReadLockPath(const QString &metadataRoot, const QString &filePath);

} // namespace vusiif::ztgui::storage
