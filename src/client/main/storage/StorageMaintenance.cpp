#include "StorageMaintenance.h"

#include "PluginDataStore.h"
#include "AppLog.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

namespace vusiif::ztgui::storage {
namespace {
constexpr qint64 LogRetentionDays = 30;
constexpr qint64 LogOwnerBytes = 100LL * 1024 * 1024;
constexpr qint64 CacheIdleDays = 7;
constexpr qint64 PluginCacheBytes = 256LL * 1024 * 1024;

bool linked(const QFileInfo &info)
{
    return info.isSymLink() || info.isJunction();
}

struct Entry {
    QString path;
    qint64 bytes = 0;
    QDateTime modified;
};

QList<Entry> logFiles(const QString &directory)
{
    QList<Entry> entries;
    const QDir dir(directory);
    if (!dir.exists() || linked(QFileInfo(directory))) return entries;
    for (const auto &info : dir.entryInfoList({"*.log"}, QDir::Files | QDir::Hidden | QDir::System)) {
        if (linked(info) || !info.isFile()) continue;
        entries.append({info.absoluteFilePath(), info.size(), info.lastModified()});
    }
    return entries;
}

QList<Entry> cacheFiles(const QString &directory, QStringList &failures)
{
    QList<Entry> entries;
    if (!QFileInfo::exists(directory)) return entries;
    if (linked(QFileInfo(directory)) || !QFileInfo(directory).isDir()) {
        failures.append(directory);
        return entries;
    }
    QDirIterator iterator(directory, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const auto info = iterator.fileInfo();
        if (linked(info) || !info.isFile()) { failures.append(info.absoluteFilePath()); continue; }
        entries.append({info.absoluteFilePath(), info.size(), info.lastModified()});
    }
    return entries;
}

qint64 sum(const QList<Entry> &entries)
{
    qint64 bytes = 0;
    for (const auto &entry : entries) bytes += entry.bytes;
    return bytes;
}

struct RemovalPlan {
    QList<Entry> removals;
    QStringList skipped;
    QStringList failures;
};

// Same ordering and limits as cleanupLogs(): age first, then the per-owner cap
// from oldest to newest. Active files are never removal candidates.
RemovalPlan planLogs(const QString &directory)
{
    RemovalPlan plan;
    auto entries = logFiles(directory);
    if (entries.isEmpty()) return plan;
    const auto now = QDateTime::currentDateTimeUtc();
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.modified < b.modified;
    });
    QList<Entry> remaining;
    for (const auto &entry : entries) {
        if (util::log::isActiveFile(entry.path)) {
            plan.skipped.append(entry.path);
            remaining.append(entry);
            continue;
        }
        if (entry.modified.secsTo(now) > LogRetentionDays * 24 * 60 * 60) {
            plan.removals.append(entry);
            continue;
        }
        remaining.append(entry);
    }
    qint64 total = sum(remaining);
    for (const auto &entry : remaining) {
        if (total <= LogOwnerBytes) break;
        if (util::log::isActiveFile(entry.path)) continue;
        plan.removals.append(entry);
        total -= entry.bytes;
    }
    return plan;
}

// Same ordering and limits as cleanupCaches(). Failures (links, non-files) are
// reported but never followed recursively.
RemovalPlan planCaches(const QString &cacheDirectory)
{
    RemovalPlan plan;
    auto entries = cacheFiles(cacheDirectory, plan.failures);
    if (entries.isEmpty()) return plan;
    const auto now = QDateTime::currentDateTimeUtc();
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.modified < b.modified;
    });
    QList<Entry> remaining;
    for (const auto &entry : entries) {
        if (entry.modified.secsTo(now) > CacheIdleDays * 24 * 60 * 60) {
            plan.removals.append(entry);
            continue;
        }
        remaining.append(entry);
    }
    qint64 total = sum(remaining);
    for (const auto &entry : remaining) {
        if (total <= PluginCacheBytes) break;
        plan.removals.append(entry);
        total -= entry.bytes;
    }
    return plan;
}

QStringList logDirectories(const StorageContext &context)
{
    QStringList directories{context.logsDirectory()};
    const QDir dataRoot(context.pluginDataDirectory());
    for (const auto &info : dataRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QDir logs(QDir(info.absoluteFilePath()).filePath("logs"));
        if (logs.exists()) directories.append(logs.absolutePath());
    }
    return directories;
}

QString modeText(StorageContext::Mode mode)
{
    switch (mode) {
    case StorageContext::Mode::Personal: return QStringLiteral("personal");
    case StorageContext::Mode::Ephemeral: return QStringLiteral("ephemeral");
    case StorageContext::Mode::Shared: break;
    }
    return QStringLiteral("shared");
}
} // namespace

StorageMaintenance::StorageMaintenance(const StorageContext &context) : m_context(context) {}

qint64 StorageMaintenance::directoryBytes(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) return 0;
    if (linked(QFileInfo(path)) || !QFileInfo(path).isDir()) return 0;
    qint64 bytes = 0;
    QDirIterator iterator(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const auto info = iterator.fileInfo();
        if (!linked(info) && info.isFile()) bytes += info.size();
    }
    return bytes;
}

StorageUsage StorageMaintenance::usage() const
{
    StorageUsage usage;
    usage.root = m_context.activeRoot();
    usage.settings = m_context.settingsFile();
    usage.logs = m_context.logsDirectory();
    usage.pluginData = m_context.pluginDataDirectory();
    usage.cache = m_context.cacheDirectory();
    usage.mode = modeText(m_context.mode());
    usage.warnings = m_context.warnings();
    usage.logsBytes = directoryBytes(usage.logs);
    usage.pluginDataBytes = directoryBytes(usage.pluginData);
    usage.cacheBytes = directoryBytes(usage.cache);
    usage.logFiles = int(logFiles(usage.logs).size());
    return usage;
}

QList<OrphanItem> StorageMaintenance::orphans(const QStringList &installedIds, bool uncertainPackages) const
{
    QList<OrphanItem> result;
    QSet<QString> installed;
    for (const auto &id : installedIds) installed.insert(id.toCaseFolded());
    const QDir root(m_context.pluginDataDirectory());
    if (!root.exists() || linked(QFileInfo(root.absolutePath()))) return result;
    const QDir ownership(QDir(m_context.metadataDirectory()).filePath("plugins"));
    for (const auto &info : root.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                               QDir::Name)) {
        const auto name = info.fileName();
        if (name.startsWith('.')) continue;
        OrphanItem item;
        item.directory = name;
        item.path = info.absoluteFilePath();
        item.bytes = directoryBytes(item.path);
        if (linked(info)) {
            item.reason = QStringLiteral("目录是链接，按人工处理保留");
            result.append(item);
            continue;
        }
        if (name.startsWith("id-")) {
            const auto id = name.mid(3);
            if (!StorageContext::validPluginId(id)) {
                item.reason = QStringLiteral("稳定 ID 目录名无效，按人工处理保留");
                result.append(item);
                continue;
            }
            if (installed.contains(id.toCaseFolded())) continue;
            item.id = id;
            if (uncertainPackages) {
                // An unparsable/unauthenticated installed package could own this
                // directory under any ID. Keep it for explicit manual handling.
                item.deletable = false;
                item.reason = QStringLiteral("存在无法解析或未通过认证的安装包，数据归属不确定，按人工处理保留");
                result.append(item);
                continue;
            }
            item.deletable = true;
            item.reason = QStringLiteral("已无安装包使用该插件 ID");
            result.append(item);
            continue;
        }
        // Legacy display-name directories are never auto-deleted. They may be
        // ambiguous or still owned by a package that has not migrated.
        item.id = QStringLiteral("legacy:");
        item.reason = QStringLiteral("旧显示名目录未经稳定 ID 登记，需人工确认");
        for (const auto &recordInfo : ownership.entryInfoList({"*.json"}, QDir::Files | QDir::NoSymLinks)) {
            QFile record(recordInfo.absoluteFilePath());
            if (!record.open(QIODevice::ReadOnly)) continue;
            const auto object = QJsonDocument::fromJson(record.readAll()).object();
            for (const auto &value : object.value("names").toArray()) {
                if (value.toString().compare(name, Qt::CaseInsensitive) != 0) continue;
                item.reason = QStringLiteral("声称归属 %1 的旧显示名目录，需人工确认")
                                  .arg(object.value("id").toString());
            }
        }
        result.append(item);
    }
    return result;
}

RetentionPreview StorageMaintenance::previewRetention() const
{
    RetentionPreview preview;
    for (const auto &directory : logDirectories(m_context)) {
        const auto plan = planLogs(directory);
        preview.logFiles += int(plan.removals.size());
        for (const auto &entry : plan.removals) preview.logBytes += entry.bytes;
    }
    const QDir dataRoot(m_context.pluginDataDirectory());
    for (const auto &info : dataRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (linked(info)) continue;
        const auto cacheDirectory = QDir(info.absoluteFilePath()).filePath("cache");
        if (!QFileInfo::exists(cacheDirectory)) continue;
        const auto plan = planCaches(cacheDirectory);
        preview.cacheFiles += int(plan.removals.size());
        for (const auto &entry : plan.removals) preview.cacheBytes += entry.bytes;
        if (!plan.failures.isEmpty() && preview.error.isEmpty())
            preview.error = QStringLiteral("部分缓存路径无法识别，将保守跳过：%1").arg(plan.failures.first());
    }
    return preview;
}

CleanupReport StorageMaintenance::cleanupLogs()
{
    CleanupReport report;
    bool hostHadFailure = false;
    for (const auto &directory : logDirectories(m_context)) {
        const auto plan = planLogs(directory);
        report.skipped.append(plan.skipped);
        for (const auto &entry : plan.removals) {
            // An active file may appear between planning and removal.
            if (util::log::isActiveFile(entry.path)) {
                report.skipped.append(entry.path);
                continue;
            }
            if (QFile::remove(entry.path)) {
                report.released.append(entry.path);
                report.releasedBytes += entry.bytes;
            } else {
                report.failed.append(entry.path);
                hostHadFailure = true;
            }
        }
    }
    if (hostHadFailure) report.error = QStringLiteral("部分日志无法删除，请检查文件占用与权限。");
    return report;
}

CleanupReport StorageMaintenance::cleanupCaches()
{
    CleanupReport report;
    CoordinationLock lock(m_context.metadataDirectory());
    if (!lock.acquire(report.error)) return report;
    const QDir dataRoot(m_context.pluginDataDirectory());
    for (const auto &info : dataRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (linked(info)) continue;
        const auto cacheDirectory = QDir(info.absoluteFilePath()).filePath("cache");
        if (!QFileInfo::exists(cacheDirectory)) continue;
        const auto plan = planCaches(cacheDirectory);
        report.failed.append(plan.failures);
        for (const auto &entry : plan.removals) {
            if (!QFile::exists(entry.path)) continue;
            // A reader holds this file's activity lock for the duration of its
            // read; wait briefly while holding the coordination lock, then
            // report it as skipped instead of forcing removal or blocking every
            // unrelated transaction for the whole read.
            const auto lockPath = cacheReadLockPath(m_context.metadataDirectory(), entry.path);
            if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
                report.failed.append(entry.path);
                continue;
            }
            QLockFile activity(lockPath);
            activity.setStaleLockTime(0);
            if (!activity.tryLock(1000)) {
                report.skipped.append(entry.path);
                continue;
            }
            if (QFile::remove(entry.path)) {
                report.released.append(entry.path);
                report.releasedBytes += entry.bytes;
            } else {
                // The coordination lock excludes cooperating host processes.
                // A remaining open handle (another program, antivirus scanner)
                // makes the file in-use rather than an actual failure.
                QFile probe(entry.path);
                if (!probe.open(QIODevice::ReadWrite)) {
                    report.skipped.append(entry.path);
                } else {
                    probe.close();
                    report.failed.append(entry.path);
                }
            }
        }
        QDir().rmdir(cacheDirectory); // Only empty directories are removed.
    }
    if (!report.failed.isEmpty())
        report.error = QStringLiteral("部分缓存文件正在使用或无法删除，已保留。");
    return report;
}

CleanupReport StorageMaintenance::cleanupOrphanData(const QStringList &ids)
{
    CleanupReport report;
    PluginDataStore store(m_context.pluginDataDirectory(), m_context.metadataDirectory(),
                          m_context.runId(), m_context.userKey());
    for (const auto &id : ids) {
        if (!StorageContext::validPluginId(id)) {
            report.failed.append(id);
            continue;
        }
        const auto directory = store.dataDirectory(id);
        if (!QFileInfo::exists(directory)) {
            report.skipped.append(id);
            continue;
        }
        const auto bytes = directoryBytes(directory);
        // Close the one-way legacy import before deleting unowned data.
        const auto tombstone = store.completeLegacyImport(id);
        if (!tombstone.ok()) {
            report.failed.append(QStringLiteral("%1：%2").arg(id, tombstone.error));
            continue;
        }
        QString error;
        if (!store.deleteDataDirectory(id, error)) {
            report.failed.append(QStringLiteral("%1：%2").arg(id, error));
            continue;
        }
        report.released.append(id);
        report.releasedBytes += bytes;
    }
    if (!report.failed.isEmpty()) report.error = QStringLiteral("部分无主数据未能删除，结果已保留。");
    return report;
}

} // namespace vusiif::ztgui::storage
