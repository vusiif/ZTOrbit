#include "PluginDataStore.h"

#include "StorageContext.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace vusiif::ztgui::storage {
namespace {
bool linked(const QFileInfo &info)
{
    return info.isSymLink() || info.isJunction();
}

bool readObject(const QString &path, QJsonObject &object, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError) { error = parse.errorString(); return false; }
    if (!document.isObject()) { error = QStringLiteral("元信息不是 JSON 对象"); return false; }
    object = document.object();
    return true;
}

bool writeObject(const QString &path, const QJsonObject &object, QString &error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = QStringLiteral("无法创建目录：%1").arg(QFileInfo(path).absolutePath());
        return false;
    }
    QSaveFile file(path);
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

QJsonObject baseOwnership(const QString &pluginId)
{
    return QJsonObject{{"format", 1}, {"id", pluginId},
                       {"createdAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
}

bool samePath(const QString &first, const QString &second)
{
    const auto a = QDir::cleanPath(QFileInfo(first).absoluteFilePath());
    const auto b = QDir::cleanPath(QFileInfo(second).absoluteFilePath());
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

// A legacy display-name directory is exactly one ordinary path segment below
// plugins-data. Display names remain unrestricted UI metadata; only the
// automatic historical migration is limited to a safe name.
bool safeLegacyDirectoryName(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QLatin1Char(':')) || name.contains(QChar('\0')))
        return false;
    if (QDir::isAbsolutePath(name)) return false;
    if (QFileInfo(name).fileName() != name) return false;
#ifdef Q_OS_WIN
    // A trailing dot or space cannot name a real directory on Windows; a
    // candidate that only matches after trimming must not be migrated.
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) return false;
#endif
    return true;
}

bool settledMigrationItem(const QJsonObject &item)
{
    const auto status = item.value("status").toString();
    return status == QLatin1String("copied") || status == QLatin1String("existing")
        || status == QLatin1String("missing");
}
} // namespace

PluginDataStore::PluginDataStore(QString pluginsDataRoot, QString metadataRoot, QString runId, QString userKey)
    : m_pluginsDataRoot(QDir::cleanPath(QFileInfo(pluginsDataRoot.isEmpty()
          ? QDir(QCoreApplication::applicationDirPath()).filePath("plugins-data")
          : pluginsDataRoot).absoluteFilePath())),
      m_metadataRoot(QDir::cleanPath(QFileInfo(metadataRoot.isEmpty()
          ? QDir(QFileInfo(m_pluginsDataRoot).absolutePath()).filePath(".ztgui")
          : metadataRoot).absoluteFilePath())),
      m_runId(runId.isEmpty() ? QUuid::createUuid().toString(QUuid::Id128) : std::move(runId)),
      m_userKey(std::move(userKey))
{
}

QString PluginDataStore::directoryName(const QString &pluginId)
{
    return StorageContext::pluginDataDirectoryName(pluginId);
}

QString PluginDataStore::dataDirectory(const QString &pluginId) const
{
    const auto name = directoryName(pluginId);
    return name.isEmpty() ? QString() : QDir(m_pluginsDataRoot).filePath(name);
}

QString PluginDataStore::ownershipFile(const QString &pluginId) const
{
    return QDir(QDir(m_metadataRoot).filePath("plugins")).filePath(pluginId.toLower() + ".json");
}

QString PluginDataStore::leaseFile(const QString &pluginId) const
{
    const auto name = directoryName(pluginId);
    return QDir(QDir(QDir(m_metadataRoot).filePath("leases")).filePath(name))
        .filePath(m_runId + ".json");
}

void PluginDataStore::addWarning(const QString &warning) const
{
    if (!m_warnings.contains(warning)) m_warnings.append(warning);
}

bool PluginDataStore::acquireCoordination(CoordinationLock &lock, QString &error) const
{
    return lock.acquire(error);
}

QJsonObject PluginDataStore::readOwnership(const QString &pluginId, QString &error) const
{
    const auto path = ownershipFile(pluginId);
    if (!QFileInfo::exists(path)) return {};
    QJsonObject object;
    if (!readObject(path, object, error)) return {};
    if (object.value("format").toInt() != 1) {
        error = QStringLiteral("插件元信息格式版本不受支持：%1").arg(path);
        return {};
    }
    if (object.value("id").toString().compare(pluginId, Qt::CaseInsensitive) != 0) {
        error = QStringLiteral("插件元信息 ID 不匹配：%1").arg(path);
        return {};
    }
    return object;
}

QJsonObject PluginDataStore::ownershipRecord(const QString &pluginId, QString &error) const
{
    return readOwnership(pluginId, error);
}

QStringList PluginDataStore::legacyCandidates(const QJsonObject &ownership, const QString &displayName) const
{
    QStringList names;
    const auto append = [&names, this](const QString &name) {
        const auto trimmed = name.trimmed();
        if (trimmed.isEmpty()) return;
        if (!safeLegacyDirectoryName(trimmed)) {
            // Never turn an arbitrary display name into a path. Warn once so the
            // skipped migration is visible instead of silently doing nothing.
            addWarning(QStringLiteral("显示名不是单一目录名，未参与旧数据目录迁移：%1").arg(trimmed));
            return;
        }
        if (!names.contains(trimmed, Qt::CaseInsensitive)) names.append(trimmed);
    };
    append(displayName);
    for (const auto &value : ownership.value("names").toArray()) append(value.toString());
    return names;
}

bool PluginDataStore::nameClaimedByOther(const QString &pluginId, const QString &name) const
{
    const QDir directory(QDir(m_metadataRoot).filePath("plugins"));
    for (const auto &entry : directory.entryInfoList({"*.json"}, QDir::Files | QDir::NoSymLinks)) {
        QJsonObject object;
        QString error;
        if (!readObject(entry.absoluteFilePath(), object, error)) continue;
        if (object.value("id").toString().compare(pluginId, Qt::CaseInsensitive) == 0) continue;
        for (const auto &value : object.value("names").toArray())
            if (value.toString().compare(name, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

bool PluginDataStore::nameClaimedByInstalled(const QString &pluginId, const QString &name) const
{
    for (const auto &entry : m_nameClaims.installed) {
        if (entry.first.compare(pluginId, Qt::CaseInsensitive) == 0) continue;
        if (entry.second.trimmed().compare(name, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

QString PluginDataStore::ownershipUncertainty() const
{
    // Registration itself failed (busy lock, unreadable record, unwritable
    // metadata): the claims below are still usable for a name conflict, but the
    // stored ownership cannot be trusted, so nothing may be renamed by guess.
    if (!m_nameClaims.issues.isEmpty())
        return QStringLiteral("插件显示名归属未能完整登记：%1").arg(m_nameClaims.issues.first());
    const QDir directory(QDir(m_metadataRoot).filePath("plugins"));
    if (!directory.exists()) return {};
    QSet<QString> claimed;
    if (m_nameClaims.authoritative)
        for (const auto &entry : m_nameClaims.installed) claimed.insert(entry.first.toLower());
    for (const auto &entry : directory.entryInfoList({"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Name)) {
        const auto recordId = entry.completeBaseName();
        // With a complete install list, a leftover record of an uninstalled ID
        // cannot claim new data; without one, every record is relevant.
        if (m_nameClaims.authoritative && !claimed.contains(recordId.toLower())) continue;
        QJsonObject object;
        QString error;
        if (!readObject(entry.absoluteFilePath(), object, error))
            return QStringLiteral("插件归属记录无法读取，无法确认旧目录归属：%1（%2）")
                .arg(entry.fileName(), error);
        if (object.value("format").toInt() != 1)
            return QStringLiteral("插件归属记录格式版本不受支持，无法确认旧目录归属：%1").arg(entry.fileName());
        if (object.value("id").toString().compare(recordId, Qt::CaseInsensitive) != 0)
            return QStringLiteral("插件归属记录 ID 不匹配，无法确认旧目录归属：%1").arg(entry.fileName());
    }
    return {};
}

bool PluginDataStore::writeOwnership(const QString &pluginId, const QString &displayName, QString &error) const
{
    auto object = readOwnership(pluginId, error);
    if (!error.isEmpty()) return false;
    if (object.isEmpty()) object = baseOwnership(pluginId);
    object["id"] = pluginId;
    object["format"] = 1;
    object["dataDirectory"] = directoryName(pluginId);
    object["updatedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QStringList names;
    for (const auto &value : object.value("names").toArray())
        if (!value.toString().trimmed().isEmpty() && !names.contains(value.toString(), Qt::CaseInsensitive))
            names.append(value.toString());
    if (!displayName.trimmed().isEmpty() && !names.contains(displayName.trimmed(), Qt::CaseInsensitive))
        names.append(displayName.trimmed());
    object["names"] = QJsonArray::fromStringList(names);
    return writeObject(ownershipFile(pluginId), object, error);
}

PluginDataStore::InstalledNameClaims PluginDataStore::registerInstalledNames(
    const QList<QPair<QString, QString>> &installed) const
{
    InstalledNameClaims claims;
    claims.authoritative = true;
    for (const auto &entry : installed) {
        const auto &id = entry.first;
        const auto name = entry.second.trimmed();
        if (!StorageContext::validPluginId(id) || name.isEmpty()) continue;
        claims.installed.append({id, name});
    }
    if (claims.installed.isEmpty()) return claims;
    // Runs before any plugin instance can call prepare(), so a display name
    // shared by two installed IDs is already visible to nameClaimedByOther().
    CoordinationLock lock(m_metadataRoot);
    QString lockError;
    if (!acquireCoordination(lock, lockError)) {
        claims.issues.append(QStringLiteral("无法登记插件显示名归属：%1").arg(lockError));
        return claims;
    }
    for (const auto &entry : claims.installed) {
        const auto &id = entry.first;
        const auto &name = entry.second;
        QString error;
        const auto ownership = readOwnership(id, error);
        if (!error.isEmpty()) {
            // Never overwrite a corrupt ownership record: the loader/reporting
            // already treats it as an error, and the in-memory claim still
            // protects the legacy directory of a same-name twin.
            claims.issues.append(QStringLiteral("插件 %1 的显示名归属无法登记：%2").arg(id, error));
            continue;
        }
        bool recorded = false;
        for (const auto &value : ownership.value("names").toArray())
            if (value.toString().compare(name, Qt::CaseInsensitive) == 0) recorded = true;
        if (recorded) continue;
        if (!writeOwnership(id, name, error))
            claims.issues.append(QStringLiteral("插件 %1 的显示名归属无法写入：%2").arg(id, error));
    }
    return claims;
}

QString PluginDataStore::pendingMigration(const QString &pluginId, const QString &displayName) const
{
    if (!StorageContext::validPluginId(pluginId)) return {};
    const auto path = QDir(m_metadataRoot).filePath(QStringLiteral("migration.json"));
    if (!QFileInfo::exists(path)) return {};
    QJsonObject record;
    QString error;
    if (!readObject(path, record, error) || record.value("format").toInt() != 1) {
        // Without an authoritative record a retry could later copy the original
        // data onto a target that defaults already occupy. Keep writes off.
        return QStringLiteral("共享数据迁移记录不可读，已暂停插件持久写入：%1").arg(path);
    }
    if (record.value("completed").toBool()) return {};
    QStringList owned{directoryName(pluginId)};
    QString ownershipError;
    const auto ownership = readOwnership(pluginId, ownershipError);
    if (ownershipError.isEmpty()) {
        for (const auto &name : legacyCandidates(ownership, displayName))
            if (!owned.contains(name, Qt::CaseInsensitive)) owned.append(name);
    } else {
        const auto trimmed = displayName.trimmed();
        if (safeLegacyDirectoryName(trimmed)) owned.append(trimmed);
    }
    for (const auto &value : record.value("items").toArray()) {
        const auto item = value.toObject();
        if (!item.value("key").toString().startsWith(QLatin1String("plugin-data:"))) continue;
        if (settledMigrationItem(item)) continue;
        const auto targetName = QFileInfo(item.value("target").toString()).fileName();
        const auto sourceName = QFileInfo(item.value("source").toString()).fileName();
        for (const auto &name : owned) {
            if (targetName.compare(name, Qt::CaseInsensitive) == 0
                || sourceName.compare(name, Qt::CaseInsensitive) == 0)
                return QStringLiteral("共享数据尚未恢复到个人目录，已暂停插件持久写入：%1（下次启动会自动重试）")
                    .arg(name);
        }
    }
    return {};
}

QString PluginDataStore::prepare(const QString &pluginId, const QString &displayName, QString &error)
{
    if (!StorageContext::validPluginId(pluginId)) {
        error = QStringLiteral("插件 ID 无效，无法建立数据目录");
        return {};
    }
    if (!QDir().mkpath(m_pluginsDataRoot)) {
        error = QStringLiteral("无法创建插件数据根目录：%1").arg(m_pluginsDataRoot);
        return {};
    }
    CoordinationLock lock(m_metadataRoot);
    if (!acquireCoordination(lock, error)) return {};
    auto ownership = readOwnership(pluginId, error);
    if (!error.isEmpty()) return {};
    const auto target = dataDirectory(pluginId);
    const auto candidates = legacyCandidates(ownership, displayName);
    // Legacy directory renaming is only safe while no other process/instance
    // uses this ID and while every same-name owner can be identified. An
    // unreadable ownership record or a failed registration means the target of
    // an automatic migration is unknown, so the directory is kept for a later
    // attempt (and for manual review) instead of being merged.
    const auto unresolved = ownershipUncertainty();
    // The check runs before this process registers its lease, so our own
    // registration is never mistaken for a conflict.
    bool migrationBlocked = false;
    if (!QFileInfo::exists(target)) {
        const bool candidateExists = std::any_of(candidates.cbegin(), candidates.cend(),
            [&](const QString &name) {
                if (name == directoryName(pluginId)) return false;
                const QFileInfo legacy(QDir(m_pluginsDataRoot).filePath(name));
                return legacy.exists() && !linked(legacy) && legacy.isDir();
            });
        if (candidateExists) {
            if (!unresolved.isEmpty()) {
                migrationBlocked = true;
                addWarning(unresolved
                    + QStringLiteral("；已保留旧显示名数据目录，待归属可确认后再迁移"));
            } else {
                QString leaseError;
                if (hasOtherLeases(pluginId, leaseError)) {
                    migrationBlocked = true;
                    addWarning(leaseError.isEmpty()
                        ? QStringLiteral("插件 %1 存在其他进程或实例的租约，已保留旧显示名数据目录，租约释放后下次加载再迁移").arg(pluginId)
                        : QStringLiteral("插件 %1 的租约无法确认，已保留旧显示名数据目录，下次加载再迁移：%2")
                              .arg(pluginId, leaseError));
                }
            }
        }
    }
    for (const auto &name : candidates) {
        if (migrationBlocked) break;
        if (name == directoryName(pluginId)) continue;
        const QFileInfo legacy(QDir(m_pluginsDataRoot).filePath(name));
        if (!legacy.exists()) continue;
        if (linked(legacy) || !legacy.isDir()) {
            addWarning(QStringLiteral("旧显示名数据目录不是普通目录，已保留：%1").arg(name));
            continue;
        }
        // Re-validate the resolved source, not just the declared name: the
        // directory must be a direct, ordinary child of plugins-data. This is
        // the second line of defence behind the single-segment name check.
        const auto legacyPath = QDir::cleanPath(legacy.absoluteFilePath());
        if (!samePath(QFileInfo(legacyPath).absolutePath(), m_pluginsDataRoot)
            || QFileInfo(legacyPath).fileName() != name) {
            addWarning(QStringLiteral("旧显示名数据目录越界，已保留：%1").arg(name));
            continue;
        }
        if (QFileInfo::exists(target)) {
            addWarning(QStringLiteral("旧显示名数据目录与新 ID 目录同时存在，已保留两者：%1").arg(name));
            continue;
        }
        if (nameClaimedByOther(pluginId, name) || nameClaimedByInstalled(pluginId, name)) {
            addWarning(QStringLiteral("显示名 %1 还对应其他插件 ID，未自动迁移").arg(name));
            continue;
        }
        if (!QDir().rename(legacy.absoluteFilePath(), target)) {
            addWarning(QStringLiteral("无法将旧数据目录 %1 迁移为稳定 ID 目录").arg(name));
            continue;
        }
    }
    // A blocked migration must not pre-create the target directory, otherwise
    // the next launch would see both directories and never migrate. The caller
    // still receives the path; the first actual file write creates it.
    if (!migrationBlocked && !QDir().mkpath(target)) {
        error = QStringLiteral("无法创建插件数据目录：%1").arg(target);
        return {};
    }
    if (!writeOwnership(pluginId, displayName, error)) return {};
    QJsonObject lease{{"format", 1}, {"pluginId", pluginId}, {"runId", m_runId},
                      {"userKey", m_userKey}, {"pid", QCoreApplication::applicationPid()},
                      {"startedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (!writeObject(leaseFile(pluginId), lease, error)) return {};
    return target;
}

void PluginDataStore::release(const QString &pluginId)
{
    const auto path = leaseFile(pluginId);
    QFile::remove(path);
    const auto directory = QFileInfo(path).absolutePath();
    QDir().rmdir(directory); // Only removes an empty directory.
}

LegacyImportState PluginDataStore::legacyImportState(const QString &pluginId) const
{
    LegacyImportState state;
    if (!StorageContext::validPluginId(pluginId)) {
        state.error = QStringLiteral("插件 ID 无效");
        return state;
    }
    QString error;
    const auto ownership = readOwnership(pluginId, error);
    if (!error.isEmpty()) {
        state.error = error;
        return state;
    }
    if (ownership.isEmpty()) {
        state.allowed = true;
        return state;
    }
    state.allowed = !ownership.value("legacyImport").toObject().value("completed").toBool();
    return state;
}

StorageResult PluginDataStore::completeLegacyImport(const QString &pluginId, CoordinationLock *held)
{
    StorageResult result;
    if (!StorageContext::validPluginId(pluginId)) {
        result.error = QStringLiteral("插件 ID 无效");
        return result;
    }
    CoordinationLock lock(m_metadataRoot);
    CoordinationLock *coordination = held;
    if (!coordination) {
        if (!acquireCoordination(lock, result.error)) return result;
        coordination = &lock;
    } else if (!coordination->held()) {
        result.error = QStringLiteral("调用方未持有存储协调锁");
        return result;
    }
    Q_UNUSED(coordination);
    auto object = readOwnership(pluginId, result.error);
    if (!result.error.isEmpty()) return result;
    if (object.isEmpty()) object = baseOwnership(pluginId);
    auto legacy = object.value("legacyImport").toObject();
    if (legacy.value("completed").toBool()) return result;
    legacy["completed"] = true;
    legacy["userKey"] = m_userKey;
    legacy["at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    object["legacyImport"] = legacy;
    object["id"] = pluginId;
    object["format"] = 1;
    writeObject(ownershipFile(pluginId), object, result.error);
    return result;
}

bool PluginDataStore::hasOtherLeases(const QString &pluginId, QString &error) const
{
    if (!StorageContext::validPluginId(pluginId)) {
        error = QStringLiteral("插件 ID 无效");
        return true;
    }
    const QDir directory(QDir(QDir(m_metadataRoot).filePath("leases")).filePath(directoryName(pluginId)));
    if (!directory.exists()) return false;
    if (linked(QFileInfo(directory.absolutePath()))) {
        error = QStringLiteral("插件租约目录是链接，按正在使用处理");
        return true;
    }
    for (const auto &entry : directory.entryInfoList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name)) {
        if (entry.fileName() == m_runId + ".json") continue;
        if (!entry.isFile() || linked(entry)) {
            error = QStringLiteral("存在无法识别的插件租约：%1").arg(entry.absoluteFilePath());
            return true;
        }
        QJsonObject lease;
        QString leaseError;
        if (!readObject(entry.absoluteFilePath(), lease, leaseError)) {
            error = QStringLiteral("插件租约无法读取，按正在使用处理：%1").arg(leaseError);
            return true;
        }
        return true; // Unknown or other-user leases are conservatively in use.
    }
    return false;
}

bool PluginDataStore::deleteDataDirectory(const QString &pluginId, QString &error, CoordinationLock *held)
{
    if (!StorageContext::validPluginId(pluginId)) {
        error = QStringLiteral("插件 ID 无效");
        return false;
    }
    CoordinationLock lock(m_metadataRoot);
    CoordinationLock *coordination = held;
    if (!coordination) {
        if (!acquireCoordination(lock, error)) return false;
        coordination = &lock;
    } else if (!coordination->held()) {
        error = QStringLiteral("调用方未持有存储协调锁");
        return false;
    }
    Q_UNUSED(coordination);
    if (hasOtherLeases(pluginId, error)) return false;
    const auto target = dataDirectory(pluginId);
    const QFileInfo info(target);
    if (!info.exists()) return true;
    if (linked(info) || !info.isDir()) {
        error = QStringLiteral("插件数据目录不是普通目录，已取消删除：%1").arg(target);
        return false;
    }
    if (!QDir(target).removeRecursively()) {
        error = QStringLiteral("无法完整删除插件数据，请检查文件占用后重试。");
        return false;
    }
    return true;
}

} // namespace vusiif::ztgui::storage
