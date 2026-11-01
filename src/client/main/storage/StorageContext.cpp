#include "StorageContext.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <sddl.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

namespace vusiif::zto::storage {
namespace {
QMutex installKeyMutex;
QString cachedInstallKey;
QString cachedInstallSource;

QString normalizedForHash(const QString &path)
{
    auto normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    normalized.replace('/', '\\');
    return normalized.toCaseFolded();
#else
    return normalized;
#endif
}

QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QString modeName(StorageContext::Mode mode)
{
    switch (mode) {
    case StorageContext::Mode::Personal: return QStringLiteral("personal");
    case StorageContext::Mode::Ephemeral: return QStringLiteral("ephemeral");
    case StorageContext::Mode::Shared: break;
    }
    return QStringLiteral("shared");
}

bool readObject(const QString &path, QJsonObject &object)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
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
        error = file.errorString().isEmpty() ? QStringLiteral("无法写入 %1").arg(path) : file.errorString();
        return false;
    }
    return true;
}

bool linked(const QFileInfo &info)
{
    return info.isSymLink() || info.isJunction();
}

QString hashFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(sha256(file.readAll()));
}

bool copyFileAtomic(const QString &source, const QString &target, QString &error)
{
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) { error = input.errorString(); return false; }
    const auto data = input.readAll();
    if (input.error() != QFileDevice::NoError) { error = input.errorString(); return false; }
    QSaveFile output(target);
    if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit()) {
        error = output.errorString(); return false;
    }
    if (hashFile(source) != hashFile(target)) {
        error = QStringLiteral("复制校验和不一致"); return false;
    }
    return true;
}

QString stableUserIdentifier()
{
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (size > 0) {
            QByteArray buffer(static_cast<int>(size), Qt::Uninitialized);
            if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
                const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.constData());
                LPWSTR text = nullptr;
                if (ConvertSidToStringSidW(user->User.Sid, &text)) {
                    const QString sid = QString::fromWCharArray(text);
                    LocalFree(text);
                    CloseHandle(token);
                    return sid;
                }
            }
        }
        CloseHandle(token);
    }
    return QString::fromLocal8Bit(qgetenv("USERDOMAIN")) + '\\' + QString::fromLocal8Bit(qgetenv("USERNAME"));
#else
    const uid_t uid = getuid();
    QString name = QString::number(uid);
    if (const auto *entry = getpwuid(uid)) name = QString::fromLocal8Bit(entry->pw_name) + ':' + name;
    return name;
#endif
}

bool hasPluginIdCharacters(const QString &id)
{
    if (id.isEmpty() || id.size() > 128 || id == QLatin1String("builtin")) return false;
    for (const auto character : id) {
        if (!(character.isLetterOrNumber() && character.unicode() < 128) && character != '_' && character != '-') return false;
    }
    return true;
}
} // namespace

QString StorageContext::normalizedPath(const QString &path)
{
    return normalizedForHash(path);
}

QString StorageContext::installKey(const QString &applicationDirectory)
{
    const QString source = normalizedForHash(applicationDirectory.isEmpty()
        ? QCoreApplication::applicationDirPath() : applicationDirectory);
    const QMutexLocker lock(&installKeyMutex);
    if (cachedInstallSource == source && !cachedInstallKey.isEmpty()) return cachedInstallKey;
    cachedInstallSource = source;
    cachedInstallKey = QString::fromLatin1(sha256(source.toUtf8()));
    return cachedInstallKey;
}

QString StorageContext::userKey()
{
    return QString::fromLatin1(sha256(stableUserIdentifier().toUtf8()));
}

bool StorageContext::validPluginId(const QString &id)
{
    return hasPluginIdCharacters(id);
}

QString StorageContext::pluginDataDirectoryName(const QString &pluginId)
{
    // Verify before normalizing: lowercase conversion must never turn an
    // invalid public ID into a valid directory name.
    return validPluginId(pluginId) ? QStringLiteral("id-") + pluginId.toLower() : QString();
}

StorageContext StorageContext::forTesting(const QString &root)
{
    StorageContext context;
    context.m_activeRoot = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    context.m_sharedRoot = context.m_activeRoot;
    context.m_personalRoot = context.m_activeRoot;
    context.m_installKey = installKey(context.m_activeRoot);
    context.m_userKey = userKey();
    context.m_runId = QUuid::createUuid().toString(QUuid::Id128);
    context.m_mode = Mode::Shared;
    QDir().mkpath(context.m_activeRoot);
    return context;
}

QString StorageContext::settingsFile() const
{
    return m_activeRoot.isEmpty() ? QString() : QDir(m_activeRoot).filePath("settings.ini");
}

QString StorageContext::logsDirectory() const
{
    return m_activeRoot.isEmpty() ? QString() : QDir(m_activeRoot).filePath("logs");
}

QString StorageContext::pluginDataDirectory() const
{
    return m_activeRoot.isEmpty() ? QString() : QDir(m_activeRoot).filePath("plugins-data");
}

QString StorageContext::pluginDataDirectory(const QString &pluginId) const
{
    const auto name = pluginDataDirectoryName(pluginId);
    if (name.isEmpty() || activeRoot().isEmpty()) return {};
    return QDir(pluginDataDirectory()).filePath(name);
}

QString StorageContext::cacheDirectory() const
{
    return m_activeRoot.isEmpty() ? QString() : QDir(m_activeRoot).filePath("cache");
}

QString StorageContext::pluginPackageCacheRoot() const
{
    if (m_activeRoot.isEmpty()) return {};
    return QDir(cacheDirectory()).filePath(QStringLiteral("plugin-packages/%1/%2").arg(m_userKey, m_runId));
}

QString StorageContext::metadataDirectory() const
{
    return m_activeRoot.isEmpty() ? QString() : QDir(m_activeRoot).filePath(".zto");
}

void StorageContext::addWarning(const QString &message)
{
    if (!m_warnings.contains(message)) m_warnings.append(message);
}

StorageContext StorageContext::resolve(const QString &applicationDirectory)
{
    return resolveInternal(applicationDirectory, {});
}

StorageContext StorageContext::resolveForTesting(const QString &applicationDirectory, const QString &personalBase)
{
    return resolveInternal(applicationDirectory, personalBase);
}

StorageContext StorageContext::resolveInternal(const QString &applicationDirectory, const QString &personalBase)
{
    StorageContext context;
    context.m_sharedRoot = QDir::cleanPath(QFileInfo(applicationDirectory.isEmpty()
        ? QCoreApplication::applicationDirPath() : applicationDirectory).absoluteFilePath());
    context.m_installKey = installKey(context.m_sharedRoot);
    context.m_userKey = userKey();
    context.m_runId = QUuid::createUuid().toString(QUuid::Id128);

    auto base = personalBase;
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) {
        context.addWarning(QStringLiteral("无法确定当前用户的本地数据目录，个人回退目录使用临时位置。"));
        base = QDir(QDir::tempPath()).filePath(QStringLiteral("ZTOrbit"));
    }
    const QDir installations(QDir(base).filePath(QStringLiteral("installations")));
    const QDir installDirectory(installations.filePath(context.m_installKey));
    context.m_personalRoot = installDirectory.filePath(QStringLiteral("profile"));
    context.m_routeFile = installDirectory.filePath(QStringLiteral("route.json"));
    context.m_hadRoute = QFileInfo::exists(context.m_routeFile);

    if (context.m_hadRoute) {
        QJsonObject route;
        if (!readObject(context.m_routeFile, route)) {
            context.addWarning(QStringLiteral("安装存储路由文件损坏，将重新探测程序目录。"));
        } else if (route.value("mode").toString() == QLatin1String("personal")) {
            context.m_mode = Mode::Personal;
            context.m_selectedPersonal = true;
        }
    }

    if (!context.m_selectedPersonal) {
        QString probeError;
        if (context.probeWritable(context.m_sharedRoot, probeError)) {
            context.m_mode = Mode::Shared;
            context.m_activeRoot = context.m_sharedRoot;
        } else {
            context.m_mode = Mode::Personal;
            context.addWarning(QStringLiteral("程序目录不可写，已改用当前用户的个人存储目录。"));
        }
    }

    if (context.m_mode == Mode::Personal) {
        QString probeError;
        if (context.probeWritable(context.m_personalRoot, probeError)) {
            context.m_activeRoot = context.m_personalRoot;
        } else {
            context.m_mode = Mode::Ephemeral;
            context.addWarning(QStringLiteral("个人存储目录不可写，本次运行无法持久保存设置。"));
            const auto ephemeral = QDir(QDir::tempPath()).filePath(
                QStringLiteral("ZTOrbit-ephemeral-%1-%2").arg(context.m_installKey.left(12),
                    QUuid::createUuid().toString(QUuid::Id128).left(12)));
            if (QDir().mkpath(ephemeral)) context.m_activeRoot = QDir::cleanPath(ephemeral);
        }
    }
    return context;
}

bool StorageContext::probeWritable(const QString &root, QString &error)
{
    if (root.isEmpty()) { error = QStringLiteral("存储目录为空"); return false; }
    if (!QDir().mkpath(root)) { error = QStringLiteral("无法创建目录：%1").arg(root); return false; }
    const QFileInfo rootInfo(root);
    if (rootInfo.exists() && !rootInfo.isDir()) { error = QStringLiteral("存储路径不是目录：%1").arg(root); return false; }
    const auto token = QUuid::createUuid().toString(QUuid::Id128);
    const QStringList subdirectories{QString(), QStringLiteral("logs"), QStringLiteral("plugins-data"),
                                     QStringLiteral("cache"), QStringLiteral(".zto")};
    for (const auto &subdirectory : subdirectories) {
        const QDir directory(subdirectory.isEmpty() ? root : QDir(root).filePath(subdirectory));
        if (!QDir().mkpath(directory.absolutePath())) {
            error = QStringLiteral("无法创建 %1").arg(directory.absolutePath()); return false;
        }
        const QString probe = directory.filePath(QStringLiteral(".zto-probe-") + token);
        QSaveFile file(probe);
        const QByteArray payload("zto-storage-probe");
        if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() || !file.commit()) {
            error = file.errorString(); return false;
        }
        QFile check(probe);
        if (!check.open(QIODevice::ReadOnly) || check.readAll() != payload) {
            error = QStringLiteral("无法读回探测文件"); return false;
        }
        check.close();
        if (!QFile::remove(probe)) { error = QStringLiteral("无法删除探测文件"); return false; }
    }
    // Another process holding the coordination lock does not make the root
    // read-only. Permission and unknown errors do.
    QLockFile lock(QDir(root).filePath(QStringLiteral(".zto/coord.lock")));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(500) && lock.error() != QLockFile::LockFailedError) {
        error = QStringLiteral("无法创建存储协调锁"); return false;
    }
    if (lock.isLocked()) lock.unlock();
    return true;
}

bool StorageContext::selectRoot(QString &error)
{
    if (m_activeRoot.isEmpty()) { error = QStringLiteral("没有可写的存储目录"); return false; }
    return true;
}

bool StorageContext::prepareLayout(QString &error)
{
    if (!selectRoot(error)) return false;
    const QStringList directories{m_activeRoot, logsDirectory(), pluginDataDirectory(), cacheDirectory(),
                                  metadataDirectory(), QDir(metadataDirectory()).filePath(QStringLiteral("plugins")),
                                  QDir(metadataDirectory()).filePath(QStringLiteral("leases")),
                                  QDir(metadataDirectory()).filePath(QStringLiteral("pending-deletions"))};
    for (const auto &directory : directories) {
        if (!QDir().mkpath(directory)) {
            error = QStringLiteral("无法创建存储目录：%1").arg(directory); return false;
        }
        if (linked(QFileInfo(directory))) {
            error = QStringLiteral("存储目录不能是链接：%1").arg(directory); return false;
        }
    }
    return true;
}

QByteArray StorageContext::ownershipRecord() const
{
    return QJsonDocument(QJsonObject{
        {"format", 1},
        {"kind", QStringLiteral("zto-storage")},
        {"installKey", m_installKey},
        {"userKey", m_userKey},
        {"mode", modeName(m_mode)},
        {"applicationDirectory", m_sharedRoot},
        {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
    }).toJson(QJsonDocument::Indented);
}

bool StorageContext::writeOwnership(QString &error)
{
    const auto path = QDir(metadataDirectory()).filePath(QStringLiteral("profile.json"));
    QJsonObject existing;
    if (QFileInfo::exists(path)) {
        if (!readObject(path, existing)) {
            error = QStringLiteral("存储所有权记录损坏：%1").arg(path);
            return false;
        }
        if (existing.value("format").toInt() != 1) {
            error = QStringLiteral("不支持的存储格式版本：%1").arg(existing.value("format").toInt());
            return false;
        }
        if (existing.value("installKey").toString() != m_installKey) {
            error = QStringLiteral("存储目录属于另一个安装位置，已停止写入：%1").arg(path);
            return false;
        }
        return true;
    }
    return writeObject(path, QJsonDocument::fromJson(ownershipRecord()).object(), error);
}

bool StorageContext::writeRoute(QString &error)
{
    if (m_mode != Mode::Personal) return true;
    QJsonObject route{{"format", 1}, {"mode", QStringLiteral("personal")},
                      {"installKey", m_installKey}, {"userKey", m_userKey},
                      {"root", QDir::cleanPath(m_personalRoot)},
                      {"selectedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    return writeObject(m_routeFile, route, error);
}

QString StorageContext::migrationFile() const
{
    return QDir(metadataDirectory()).filePath(QStringLiteral("migration.json"));
}

bool StorageContext::migrateItem(const QString &key, const QString &source, const QString &target,
                                 const QString &kind, bool directory, QJsonObject &record)
{
    const QFileInfo sourceInfo(source);
    record = QJsonObject{{"key", key}, {"kind", kind}, {"source", source}, {"target", target}};
    if (linked(sourceInfo)) {
        record["status"] = QStringLiteral("failed");
        record["message"] = QStringLiteral("来源是链接，已跳过");
        addWarning(QStringLiteral("迁移 %1 时跳过了链接来源：%2").arg(kind, source));
        return false;
    }
    if (!sourceInfo.exists()) {
        record["status"] = QStringLiteral("missing");
        return true;
    }
    QString error;
    if (!directory) {
        const QFileInfo targetInfo(target);
        if (linked(targetInfo) || (targetInfo.exists() && !targetInfo.isFile())) {
            record["status"] = QStringLiteral("failed");
            record["message"] = QStringLiteral("目标已存在且不是普通文件，已保留");
            return false;
        }
        if (targetInfo.exists()) {
            record["status"] = QStringLiteral("existing");
            if (hashFile(source) != hashFile(target))
                record["message"] = QStringLiteral("目标已存在且内容不同，保留目标");
            return true;
        }
        if (!copyFileAtomic(source, target, error)) {
            record["status"] = QStringLiteral("failed");
            record["message"] = error;
            addWarning(QStringLiteral("无法迁移 %1：%2").arg(kind, error));
            return false;
        }
        record["status"] = QStringLiteral("copied");
        return true;
    }
    const QFileInfo targetInfo(target);
    if (linked(targetInfo) || (targetInfo.exists() && !targetInfo.isDir())) {
        record["status"] = QStringLiteral("failed");
        record["message"] = QStringLiteral("目标已存在且不是普通目录，已保留");
        return false;
    }
    if (!QDir().mkpath(target)) {
        record["status"] = QStringLiteral("failed");
        record["message"] = QStringLiteral("无法创建目标目录");
        return false;
    }
    int copied = 0;
    int existing = 0;
    int excluded = 0;
    bool failed = false;
    // Logs stay at the shared root and plugin caches are disposable: only
    // persistent settings, business data and ownership cross the profile
    // boundary. The rule is applied per migration category so a nested
    // directory that merely happens to be named like one is not touched.
    const auto excludedPath = [&kind](const QString &relative) {
        if (kind != QLatin1String("plugin-data")) return false;
        const auto first = relative.section(QLatin1Char('/'), 0, 0);
        return first == QLatin1String("cache") || first == QLatin1String("logs");
    };
    QDirIterator iterator(source, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const auto info = iterator.fileInfo();
        const auto relative = QDir(source).relativeFilePath(info.absoluteFilePath());
        if (excludedPath(relative)) {
            if (!info.isDir()) ++excluded;
            continue;
        }
        const auto destination = QDir(target).filePath(relative);
        if (linked(info)) {
            failed = true;
            addWarning(QStringLiteral("迁移 %1 时跳过链接：%2").arg(kind, relative));
            continue;
        }
        if (info.isDir()) {
            if (!QDir().mkpath(destination)) {
                failed = true;
                addWarning(QStringLiteral("无法创建迁移目标目录：%1").arg(relative));
            }
            continue;
        }
        const QFileInfo destinationInfo(destination);
        if (destinationInfo.exists()) {
            if (linked(destinationInfo) || !destinationInfo.isFile()) {
                failed = true;
                addWarning(QStringLiteral("迁移 %1 的目标不是普通文件：%2").arg(kind, relative));
            } else {
                ++existing;
            }
            continue;
        }
        if (!copyFileAtomic(info.absoluteFilePath(), destination, error)) {
            failed = true;
            addWarning(QStringLiteral("无法迁移 %1：%2").arg(relative, error));
        } else {
            ++copied;
        }
    }
    if (failed) {
        record["status"] = QStringLiteral("partial");
        record["message"] = QStringLiteral("部分文件未能复制，已保留来源以便重试");
        return false;
    }
    record["status"] = copied > 0 ? QStringLiteral("copied") : QStringLiteral("existing");
    if (excluded > 0) record["excluded"] = excluded;
    if (existing > 0 && copied > 0)
        record["message"] = QStringLiteral("已复制 %1 个文件，保留 %2 个已存在的目标").arg(copied).arg(existing);
    return true;
}

bool StorageContext::migrateSharedData(QString &error)
{
    Q_UNUSED(error);
    m_settingsMigrationPending = false;
    if (m_mode != Mode::Personal || m_sharedRoot.isEmpty() || m_sharedRoot == m_activeRoot) {
        m_migrationComplete = true;
        return true;
    }
    const QFileInfo shared(m_sharedRoot);
    if (!shared.exists() || !shared.isDir()) {
        m_migrationComplete = true;
        return true;
    }
    const auto recordPath = migrationFile();
    QJsonObject previous;
    if (QFileInfo::exists(recordPath)) {
        if (!readObject(recordPath, previous) || previous.value("format").toInt() != 1) {
            // A damaged progress record is authoritative for nothing: keep the
            // file and skip migration instead of silently replacing it. The
            // host INI is write-protected because no item can be proven settled.
            m_migrationCorrupt = true;
            m_migrationComplete = false;
            m_settingsMigrationPending = true;
            addWarning(QStringLiteral("共享数据迁移记录损坏，已保留原文件，本次不执行迁移：%1").arg(recordPath));
            return false;
        }
        if (previous.value("completed").toBool()) {
            m_migrationComplete = true;
            return true;
        }
    }
    QMap<QString, QJsonObject> previousItems;
    for (const auto &value : previous.value("items").toArray()) {
        const auto object = value.toObject();
        const auto key = object.value("key").toString();
        if (!key.isEmpty()) previousItems.insert(key, object);
    }
    const auto settled = [](const QJsonObject &item) {
        const auto status = item.value("status").toString();
        return status == QLatin1String("copied") || status == QLatin1String("existing")
            || status == QLatin1String("missing");
    };

    QJsonArray items;
    QSet<QString> seen;
    bool complete = true;
    const auto process = [&](const QString &key, const QString &source, const QString &target,
                             const QString &kind, bool directory) {
        seen.insert(key);
        const auto previousItem = previousItems.value(key);
        if (!previousItem.isEmpty() && settled(previousItem)) {
            items.append(previousItem);
            return;
        }
        QJsonObject item;
        if (!migrateItem(key, source, target, kind, directory, item)) complete = false;
        if (!settled(item)) complete = false;
        items.append(item);
    };

    process(QStringLiteral("settings"), QDir(m_sharedRoot).filePath(QStringLiteral("settings.ini")),
            settingsFile(), QStringLiteral("settings"), false);

    const auto dataSource = QDir(m_sharedRoot).filePath(QStringLiteral("plugins-data"));
    if (QFileInfo(dataSource).isDir() && !linked(QFileInfo(dataSource))) {
        const QDir directories(dataSource);
        for (const auto &info : directories.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden
                                                              | QDir::System, QDir::Name)) {
            if (info.fileName().startsWith('.')) continue;
            process(QStringLiteral("plugin-data:") + info.fileName(), info.absoluteFilePath(),
                    QDir(pluginDataDirectory()).filePath(info.fileName()), QStringLiteral("plugin-data"), true);
        }
    } else if (linked(QFileInfo(dataSource))) {
        complete = false;
        addWarning(QStringLiteral("共享插件数据目录是链接，未迁移：%1").arg(dataSource));
    }

    process(QStringLiteral("plugin-ownership"), QDir(m_sharedRoot).filePath(QStringLiteral(".zto/plugins")),
            QDir(metadataDirectory()).filePath(QStringLiteral("plugins")),
            QStringLiteral("plugin-ownership"), true);
    process(QStringLiteral("settings-migration"),
            QDir(m_sharedRoot).filePath(QStringLiteral(".zto/settings-migration.json")),
            QDir(metadataDirectory()).filePath(QStringLiteral("settings-migration.json")),
            QStringLiteral("settings-migration"), false);

    for (auto it = previousItems.cbegin(); it != previousItems.cend(); ++it) {
        if (!seen.contains(it.key())) items.append(it.value());
    }

    QJsonObject migration{{"format", 1}, {"source", m_sharedRoot}, {"completed", complete},
                          {"updatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                          {"items", items}};
    // Only the host settings item gates host INI writes. A still unsettled
    // plugin-data item must not stop the user from saving window, theme or
    // plugin enable state; a failed settings copy must, or the retry would
    // find an existing target and never restore the original values.
    bool settingsSeen = false;
    bool settingsSettled = false;
    for (const auto &value : items) {
        const auto item = value.toObject();
        if (item.value("key").toString() != QLatin1String("settings")) continue;
        settingsSeen = true;
        settingsSettled = settled(item);
        break;
    }
    m_settingsMigrationPending = !settingsSeen || !settingsSettled;
    QString writeError;
    if (!writeObject(recordPath, migration, writeError)) {
        addWarning(QStringLiteral("无法记录存储迁移状态：%1").arg(writeError));
    }
    m_migrationComplete = complete;
    if (!complete) addWarning(QStringLiteral("部分共享数据未能复制到个人目录；下次启动会继续尝试。"));
    return complete;
}

bool StorageContext::prepare(QString &error)
{
    if (!prepareLayout(error)) { m_error = error; return false; }
    // A partial or failed migration leaves the context usable (settings and
    // already copied items are valid). The per-item result is exposed through
    // migrationComplete(); PluginDataStore refuses persistent plugin writes
    // for directories that are still unsettled instead of seeding defaults.
    if (m_mode == Mode::Personal && !m_sharedRoot.isEmpty() && m_sharedRoot != m_activeRoot)
        migrateSharedData(error);
    else
        m_migrationComplete = true;
    if (!writeOwnership(error)) { m_error = error; return false; }
    if (!writeRoute(error)) { m_error = error; return false; }
    if (m_hadRoute && m_mode == Mode::Personal && !QFileInfo::exists(m_routeFile)) {
        addWarning(QStringLiteral("安装存储路由丢失，已重新记录个人目录。"));
        if (!writeRoute(error)) { m_error = error; return false; }
    }
    return true;
}

} // namespace vusiif::zto::storage
