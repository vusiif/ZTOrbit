#include "CoordinationLock.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

namespace vusiif::ztgui::storage {

CoordinationLock::CoordinationLock(const QString &metadataRoot)
    : m_lock(QDir(metadataRoot).filePath(QStringLiteral("coord.lock")))
{
    m_lock.setStaleLockTime(30000);
}

CoordinationLock::~CoordinationLock() { release(); }

bool CoordinationLock::acquire(QString &error, int timeoutMs)
{
    if (m_held) return true;
    if (!QDir().mkpath(QFileInfo(m_lock.fileName()).absolutePath())) {
        error = QStringLiteral("无法创建协调锁目录");
        return false;
    }
    if (!m_lock.tryLock(timeoutMs)) {
        error = QStringLiteral("无法获取存储协调锁：%1").arg(m_lock.fileName());
        return false;
    }
    m_held = true;
    return true;
}

void CoordinationLock::release()
{
    if (!m_held) return;
    m_lock.unlock();
    m_held = false;
}

QString cacheReadLockPath(const QString &metadataRoot, const QString &filePath)
{
    auto normalized = QDir::cleanPath(QFileInfo(filePath).absoluteFilePath());
#ifdef Q_OS_WIN
    normalized = normalized.toCaseFolded();
#endif
    const auto digest = QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(QDir(metadataRoot).filePath(QStringLiteral("read-locks")))
        .filePath(QString::fromLatin1(digest) + QStringLiteral(".lock"));
}

} // namespace vusiif::ztgui::storage
