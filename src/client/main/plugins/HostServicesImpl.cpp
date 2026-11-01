#include "HostServicesImpl.h"
#include "ClientNetworkService.h"
#include "NetworkTransport.h"
#include "../ZeroTierClient.h"
#include "../NetworkMembers.h"
#include "../AppLog.h"
#include "../storage/StorageContext.h"
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QDebug>
#include <exception>
#include <limits>
#include <memory>

namespace vusiif::zto::util {
namespace {
QMutex fileMutex; // Serializes requests, including plugins using the same name.
bool linked(const QFileInfo &info) { return info.isSymLink() || info.isJunction(); }
bool isCachePath(const QString &relativePath)
{
    auto normalized = relativePath;
    normalized.replace('\\', '/');
    return normalized == QLatin1String("cache") || normalized.startsWith(QLatin1String("cache/"));
}
bool isLogPath(const QString &relativePath)
{
    auto normalized = relativePath;
    normalized.replace('\\', '/');
    return normalized == QLatin1String("logs") || normalized.startsWith(QLatin1String("logs/"));
}
bool disposableView(const QString &relativePath)
{
    return isCachePath(relativePath) || isLogPath(relativePath);
}
bool reservedWindowsName(const QString &name)
{
    // Windows reserves these device names in every directory, even with
    // extensions. The stable-ID root itself is prefixed with "id-" and cannot
    // collide, but relative path segments still must not resolve to a device.
    static const QRegularExpression device(QStringLiteral("\\A(?:con|prn|aux|nul|com[1-9¹²³]|lpt[1-9¹²³])(?:\\.|\\z)"),
                                           QRegularExpression::CaseInsensitiveOption);
    return device.match(name).hasMatch();
}
} // namespace
void HostServicesImpl::notifyCacheWrite(const QString &relativePath)
{
    if (m_cacheWriteNotifier && isCachePath(relativePath)) m_cacheWriteNotifier(relativePath);
}
bool HostServicesImpl::acquireFileActivityLock(const QString &path, std::unique_ptr<QLockFile> &lock,
                                               QString &error, int timeoutMs) const
{
    const auto lockPath = storage::cacheReadLockPath(m_store.metadataDirectory(), path);
    if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
        error = QStringLiteral("无法创建缓存文件活动锁目录");
        return false;
    }
    auto candidate = std::make_unique<QLockFile>(lockPath);
    // A crashed reader leaves a lock file; its PID is checked, never its age.
    candidate->setStaleLockTime(0);
    if (!candidate->tryLock(timeoutMs)) {
        error = QStringLiteral("缓存文件正在被清理或被其他进程使用，请稍后重试");
        return false;
    }
    lock = std::move(candidate);
    return true;
}
HostServicesImpl::HostServicesImpl(const QString &pluginName, const QString &dataRoot,
                                   const QString &pluginId, const QString &metadataRoot,
                                   const QString &runId, const QString &userKey,
                                   std::function<void(const QString &)> cacheWriteNotifier,
                                   const storage::PluginDataStore::InstalledNameClaims &nameClaims)
    : m_sharedDataRoot(QDir::cleanPath(dataRoot.isEmpty()
          ? QDir(QCoreApplication::applicationDirPath()).filePath("plugins-data")
          : QFileInfo(dataRoot).absoluteFilePath())),
      m_pluginName(pluginName),
      m_pluginId(pluginId),
      m_cacheWriteNotifier(std::move(cacheWriteNotifier)),
      m_store(m_sharedDataRoot, metadataRoot, runId, userKey)
{
    // The stable ID is mandatory: the display name is UI metadata only and must
    // never select or create a data directory.
    if (!storage::StorageContext::validPluginId(m_pluginId)) {
        m_storageError = QStringLiteral("插件 ID 无效，无法建立数据目录");
        return;
    }
    // Same-name ownership of the whole install list decides whether a legacy
    // display-name directory may be renamed into this ID's directory.
    m_store.setInstalledNameClaims(nameClaims);
    m_dataRoot = m_store.prepare(m_pluginId, m_pluginName, m_storageError);
    if (m_storageError.isEmpty())
        m_migrationBlock = m_store.pendingMigration(m_pluginId, m_pluginName);
}
HostServicesImpl::~HostServicesImpl()
{
    if (m_logFile.isOpen()) {
        const auto path = m_logFile.fileName();
        m_logFile.close();
        log::unregisterActiveFile(path);
    }
    if (storage::StorageContext::validPluginId(m_pluginId)) m_store.release(m_pluginId);
    stopRequests();
}

LogResult HostServicesImpl::write(LogLevel level, const QString &message)
{
    const QMutexLocker lock(&fileMutex);
    const QString text = logLevelName(level) + " [" + m_pluginId + "] " + message.trimmed();
    LogResult result;
    result.hostError = log::writeChecked(text);
    if (m_logFile.isOpen() && m_logFile.size() >= log::maximumFileBytes()) {
        const auto path = m_logFile.fileName();
        m_logFile.close();
        log::unregisterActiveFile(path);
    }
    if (!m_logFile.isOpen()) {
        const QString date = QDateTime::currentDateTime().toString("yyMMdd");
        quint64 sequence = 0;
        const QString first = filePath("logs/" + date + "-1.log", true, result.pluginError);
        if (!result.pluginError.isEmpty()) return result;
        const QDir directory(QFileInfo(first).absolutePath());
        const QRegularExpression pattern("^" + date + "-([1-9][0-9]*)\\.log$");
        for (const auto &name : directory.entryList(QDir::Files)) {
            const auto match = pattern.match(name);
            if (match.hasMatch()) sequence = qMax(sequence, match.captured(1).toULongLong());
        }
        for (;;) {
            if (sequence == std::numeric_limits<quint64>::max()) { result.pluginError = "Log sequence exhausted"; return result; }
            const auto path = filePath("logs/" + date + "-" + QString::number(++sequence) + ".log", true, result.pluginError);
            if (!result.pluginError.isEmpty()) return result;
            m_logFile.setFileName(path);
            if (m_logFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                log::registerActiveFile(path);
                break;
            }
            if (!QFile::exists(path)) { result.pluginError = m_logFile.errorString(); return result; }
        }
    }
    const QByteArray bytes = (QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + " " + text + '\n').toUtf8();
    if (m_logFile.write(bytes) != bytes.size() || !m_logFile.flush()) result.pluginError = m_logFile.errorString();
    return result;
}

QString HostServicesImpl::filePath(const QString &relativePath, bool writing, QString &error) const
{
    // Paths are rooted at the stable-ID directory. The display name has no
    // influence on the location or on whether a legal file may be accessed.
    if (!m_storageError.isEmpty()) {
        error = m_storageError; return {};
    }
    QString normalized = relativePath;
    normalized.replace('\\', '/');
    const auto parts = normalized.split('/');
    if (normalized.isEmpty() || QDir::isAbsolutePath(normalized) || normalized.contains(':')
        || normalized.contains(QChar('\0'))) {
        error = QStringLiteral("文件路径必须相对于当前插件的数据目录"); return {};
    }
    for (const auto &part : parts) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')
            || reservedWindowsName(part)) {
            error = QStringLiteral("文件路径包含无效分段"); return {};
        }
    }
    const QFileInfo sharedRoot(m_sharedDataRoot);
    if (linked(sharedRoot) || (sharedRoot.exists() && !sharedRoot.isDir())) {
        error = QStringLiteral("plugins-data 必须是普通目录"); return {};
    }
    const QFileInfo root(m_dataRoot);
    if (linked(root) || (root.exists() && !root.isDir())) {
        error = QStringLiteral("插件数据目录必须是普通目录"); return {};
    }
    if (writing && !QDir().mkpath(m_dataRoot)) {
        error = QStringLiteral("无法创建插件数据目录"); return {};
    }
    QString path = m_dataRoot;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        path = QDir(path).filePath(parts[i]);
        const QFileInfo info(path);
        if (linked(info) || (info.exists() && (i + 1 < parts.size() ? !info.isDir() : !info.isFile()))) {
            error = QStringLiteral("文件路径包含链接或非普通文件/目录"); return {};
        }
        if (writing && i + 1 < parts.size() && !QDir().mkpath(path)) {
            error = QStringLiteral("无法创建文件父目录"); return {};
        }
    }
    return path;
}
FileResult HostServicesImpl::readFile(const QString &relativePath)
{
    const QMutexLocker lock(&fileMutex);
    FileResult result;
    if (!m_storageError.isEmpty()) { result.error = m_storageError; return result; }
    const auto path = filePath(relativePath, false, result.error);
    if (!result.ok()) return result;
    // Business-cache files are disposable and may be reclaimed by storage
    // maintenance. The short coordination lock serializes the decision/open
    // window; the per-file activity lock is held for the actual read so a large
    // file does not block every other transaction in this process or others.
    // Other paths keep the cheaper in-process-only path.
    std::unique_ptr<storage::CoordinationLock> coordination;
    std::unique_ptr<QLockFile> activity;
    if (isCachePath(relativePath)) {
        coordination = std::make_unique<storage::CoordinationLock>(m_store.metadataDirectory());
        if (!m_store.acquireCoordination(*coordination, result.error)) return result;
        if (!acquireFileActivityLock(path, activity, result.error, 5000)) return result;
        coordination->release();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
    result.data = file.readAll();
    if (file.error() != QFileDevice::NoError) { result.error = file.errorString(); result.data.clear(); }
    return result;
}
FileResult HostServicesImpl::writeFile(const QString &relativePath, const QByteArray &data)
{
    const QMutexLocker lock(&fileMutex);
    FileResult result;
    if (!m_storageError.isEmpty()) { result.error = m_storageError; return result; }
    if (!m_migrationBlock.isEmpty() && !disposableView(relativePath)) {
        result.error = m_migrationBlock; return result;
    }
    storage::CoordinationLock coordination(m_store.metadataDirectory());
    if (!m_store.acquireCoordination(coordination, result.error)) return result;
    const auto path = filePath(relativePath, true, result.error);
    if (!result.ok()) return result;
    std::unique_ptr<QLockFile> activity;
    if (isCachePath(relativePath) && !acquireFileActivityLock(path, activity, result.error, 5000))
        return result;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        result.error = file.errorString();
    if (result.ok()) notifyCacheWrite(relativePath);
    return result;
}
PluginFileTransaction HostServicesImpl::updateFile(const QString &relativePath,
                                                   const PluginFileUpdater &updater)
{
    const QMutexLocker lock(&fileMutex);
    PluginFileTransaction result;
    if (!m_storageError.isEmpty()) { result.error = m_storageError; return result; }
    if (!m_migrationBlock.isEmpty() && !disposableView(relativePath)) {
        result.error = m_migrationBlock; return result;
    }
    if (!updater) { result.error = QStringLiteral("缺少文件更新回调"); return result; }
    storage::CoordinationLock coordination(m_store.metadataDirectory());
    if (!m_store.acquireCoordination(coordination, result.error)) return result;

    PluginFileSnapshot snapshot;
    const auto readPath = filePath(relativePath, false, result.error);
    if (!result.error.isEmpty()) return result;
    std::unique_ptr<QLockFile> activity;
    if (isCachePath(relativePath) && !acquireFileActivityLock(readPath, activity, result.error, 5000))
        return result;
    if (QFileInfo::exists(readPath)) {
        QFile file(readPath);
        if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
        snapshot.exists = true;
        snapshot.data = file.readAll();
        if (file.error() != QFileDevice::NoError) { result.error = file.errorString(); return result; }
    }
    PluginFileUpdate update;
    try {
        update = updater(snapshot);
    } catch (const std::exception &exception) {
        result.error = QStringLiteral("文件更新回调抛出异常：%1").arg(QString::fromUtf8(exception.what()));
        return result;
    } catch (...) {
        result.error = QStringLiteral("文件更新回调抛出未知异常，原文件已保留");
        return result;
    }
    if (update.action == PluginFileUpdate::Action::Keep) {
        result.ok = true;
        result.data = snapshot.data;
        return result;
    }
    if (update.action == PluginFileUpdate::Action::Fail) {
        result.error = update.error.isEmpty() ? QStringLiteral("文件更新被拒绝") : update.error;
        return result;
    }
    if (snapshot.exists && snapshot.data == update.data) {
        result.ok = true;
        result.data = update.data;
        return result;
    }
    const auto writePath = filePath(relativePath, true, result.error);
    if (!result.error.isEmpty()) return result;
    QSaveFile file(writePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(update.data) != update.data.size() || !file.commit()) {
        result.error = file.errorString();
        return result;
    }
    result.ok = true;
    result.changed = true;
    result.data = update.data;
    notifyCacheWrite(relativePath);
    return result;
}
LegacyImportState HostServicesImpl::legacyImportAllowed()
{
    LegacyImportState state;
    if (!m_storageError.isEmpty()) { state.error = m_storageError; return state; }
    if (!storage::StorageContext::validPluginId(m_pluginId)) {
        state.error = QStringLiteral("插件没有稳定的合法 ID，无法判断旧配置导入资格");
        return state;
    }
    return m_store.legacyImportState(m_pluginId);
}
StorageResult HostServicesImpl::completeLegacyImport()
{
    if (!m_storageError.isEmpty()) return {m_storageError};
    if (!storage::StorageContext::validPluginId(m_pluginId))
        return {QStringLiteral("插件没有稳定的合法 ID")};
    return m_store.completeLegacyImport(m_pluginId);
}
void HostServicesImpl::stopRequests()
{
    m_stopped = true;
    const auto requests = m_requests.children();
    for (auto *request : requests) delete request;
}
void HostServicesImpl::query(const QString &command, bool object, QObject *context, JsonCallback completed)
{
    if (QThread::currentThread() != thread() || !context || context->thread() != thread()) {
        qWarning() << "[插件] ZeroTier 查询需要 GUI 线程和有效的 QObject 上下文";
        return;
    }
    if (m_stopped || !completed) return;
    auto *request = new QObject(&m_requests);
    connect(context, &QObject::destroyed, request, [request] { delete request; });
    const QPointer<QObject> target(context);
    // Even missing CLI/authorization errors must complete asynchronously.
    QTimer::singleShot(0, request, [request, target, command, object, completed = std::move(completed),lease=m_callbacks] {
        vusiif::zto::zerotier::command(request, {"-j", command},
            [request, target, object, completed,lease](bool success, const QString &text) {
                JsonResult result;
                if (!success) result.error = text;
                else {
                    QJsonParseError parse;
                    result.document = QJsonDocument::fromJson(text.toUtf8(), &parse);
                    if (parse.error != QJsonParseError::NoError)
                        result.error = QStringLiteral("ZeroTier 返回无效 JSON：%1").arg(parse.errorString());
                    else if (object ? !result.document.isObject() : !result.document.isArray())
                        result.error = QStringLiteral("ZeroTier 返回的 JSON 类型不符合查询要求");
                    if (!result.error.isEmpty()) result.document = {};
                }
                request->deleteLater();
                if (target) completed(result);
            });
    });
}
void HostServicesImpl::getZTInfo(QObject *context, JsonCallback completed)
{ query("info", true, context, std::move(completed)); }
void HostServicesImpl::getZTPeers(QObject *context, JsonCallback completed)
{ query("listpeers", false, context, std::move(completed)); }
void HostServicesImpl::getZTNetworks(QObject *context, JsonCallback completed)
{ query("listnetworks", false, context, std::move(completed)); }
void HostServicesImpl::getZTMoons(QObject *context, JsonCallback completed)
{ query("listmoons", false, context, std::move(completed)); }
void HostServicesImpl::sendDataTo(const QString &network, const QString &member, const QByteArray &data,
                                  QObject *context, SendCallback completed)
{ sendNetwork(network, {member}, data, false, context, std::move(completed)); }
void HostServicesImpl::broadcastDataTo(const QString &network, const QStringList &members, const QByteArray &data,
                                       QObject *context, SendCallback completed)
{ sendNetwork(network, members, data, false, context, std::move(completed)); }
void HostServicesImpl::broadcastDataBalancedTo(const QString &network, const QStringList &members, const QByteArray &data,
                                               QObject *context, SendCallback completed)
{ sendNetwork(network, members, data, true, context, std::move(completed)); }
void HostServicesImpl::sendNetwork(const QString &network, const QStringList &members, const QByteArray &data, bool balanced,
                                    QObject *context, SendCallback completed)
{
    if (m_stopped || !context || !completed || QThread::currentThread()!=thread() || context->thread()!=thread()) return;
    auto *job=new QObject(&m_requests);
    connect(context,&QObject::destroyed,job,[job] { delete job; });
    network::ClientNetworkService::instance().withTransport(job,
        [id=m_pluginId,network,members,data,balanced,job,completed=std::move(completed),lease=m_callbacks](network::NetworkTransport &transport) {
            transport.send(id,network,members,data,balanced,job,
                [job,completed,lease](const SendResult &result) { job->deleteLater(); completed(result); });
        });
}
void HostServicesImpl::getNetworkRelays(const QString &networkId,QObject *context,RelayCallback completed) {
    if(m_stopped || !context || !completed || QThread::currentThread()!=thread() || context->thread()!=thread()) return;
    auto *job=new QObject(&m_requests); connect(context,&QObject::destroyed,job,[job] { delete job; });
    network::ClientNetworkService::instance().withTransport(job,
        [networkId,job,completed=std::move(completed),lease=m_callbacks](network::NetworkTransport &transport) {
            transport.relays(networkId,job,[job,completed,lease](const RelayResult &r) { job->deleteLater(); completed(r); });
        });
}
NetworkSubscription *HostServicesImpl::subscribeNetworkData(const QString &networkId,QObject *context,NetworkCallback callback) {
    if(m_stopped || !context || QThread::currentThread()!=thread() || context->thread()!=thread()) return nullptr;
    auto *sub=network::ClientNetworkService::instance().subscribe(m_pluginId,networkId,context,[callback=std::move(callback),lease=m_callbacks](const NetworkMessage &message) { if(callback) callback(message); });
    if(sub) { auto *guard=new QObject(&m_requests); connect(guard,&QObject::destroyed,sub,[sub] { sub->cancel(); }); }
    return sub;
}
}
