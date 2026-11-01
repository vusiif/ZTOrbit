#pragma once

#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QString>

namespace vusiif::zto::plugins {

// Install-directory-level mutation lock shared by package import and delete.
// It is independent of the active data root, so two profiles importing or
// deleting into the same plugins/ directory cannot interleave. The lock is
// always acquired before the data-root coordination lock.
class PackageChangeLock {
public:
    explicit PackageChangeLock(const QString &pluginDirectory)
        : m_lock(QDir(pluginDirectory).filePath(QStringLiteral(".import.lock")))
    {
        m_lock.setStaleLockTime(30000);
    }
    ~PackageChangeLock() { release(); }
    PackageChangeLock(const PackageChangeLock &) = delete;
    PackageChangeLock &operator=(const PackageChangeLock &) = delete;

    bool acquire(QString &error, int timeoutMs = 0)
    {
        if (m_held) return true;
        if (!QDir().mkpath(QFileInfo(m_lock.fileName()).absolutePath())) {
            error = QStringLiteral("无法创建插件目录。");
            return false;
        }
        if (!m_lock.tryLock(timeoutMs)) {
            error = QStringLiteral("另一个插件导入或删除操作正在进行，请稍后重试。");
            return false;
        }
        m_held = true;
        return true;
    }
    void release()
    {
        if (!m_held) return;
        m_lock.unlock();
        m_held = false;
    }
    bool held() const { return m_held; }

private:
    QLockFile m_lock;
    bool m_held = false;
};

} // namespace vusiif::zto::plugins
