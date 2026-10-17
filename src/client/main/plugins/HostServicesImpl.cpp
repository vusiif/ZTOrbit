#include "HostServicesImpl.h"
#include "ClientNetworkService.h"
#include "NetworkTransport.h"
#include "../ZeroTierClient.h"
#include "../NetworkMembers.h"
#include "../AppLog.h"
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
#include <limits>

namespace vusiif::ztgui::util {
namespace {
QMutex fileMutex; // Serializes requests, including plugins using the same name.
bool linked(const QFileInfo &info) { return info.isSymLink() || info.isJunction(); }
bool validPluginName(const QString &name)
{
    if (name.isEmpty() || name != name.trimmed() || name == "." || name == ".." || name.endsWith('.'))
        return false;
    for (const auto character : name) {
        if (character.unicode() < 32 || QStringLiteral("/\\:*?\"<>|").contains(character)) return false;
    }
    // Windows reserves these device names, even with extensions.
    static const QRegularExpression device(QStringLiteral("\\A(?:con|prn|aux|nul|com[1-9¹²³]|lpt[1-9¹²³])(?:\\.|\\z)"),
                                           QRegularExpression::CaseInsensitiveOption);
    return !device.match(name).hasMatch();
}
}
HostServicesImpl::HostServicesImpl(const QString &pluginName, const QString &dataRoot, const QString &pluginId)
    : m_sharedDataRoot(QDir::cleanPath(dataRoot.isEmpty()
          ? QDir(QCoreApplication::applicationDirPath()).filePath("plugins-data")
          : QFileInfo(dataRoot).absoluteFilePath())),
      m_pluginName(pluginName), m_dataRoot(QDir(m_sharedDataRoot).filePath(pluginName)),
      m_pluginId(pluginId.isEmpty() ? pluginName : pluginId) {}
HostServicesImpl::~HostServicesImpl() { stopRequests(); }

LogResult HostServicesImpl::write(LogLevel level, const QString &message)
{
    const QMutexLocker lock(&fileMutex);
    const QString text = logLevelName(level) + " [" + m_pluginId + "] " + message.trimmed();
    LogResult result;
    result.hostError = log::writeChecked(text);
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
            if (m_logFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) break;
            if (!QFile::exists(path)) { result.pluginError = m_logFile.errorString(); return result; }
        }
    }
    const QByteArray bytes = (QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + " " + text + '\n').toUtf8();
    if (m_logFile.write(bytes) != bytes.size() || !m_logFile.flush()) result.pluginError = m_logFile.errorString();
    return result;
}

QString HostServicesImpl::filePath(const QString &relativePath, bool writing, QString &error) const
{
    if (!validPluginName(m_pluginName)) {
        error = QStringLiteral("插件名称不能用作数据目录名"); return {};
    }
    QString normalized = relativePath;
    normalized.replace('\\', '/');
    const auto parts = normalized.split('/');
    if (normalized.isEmpty() || QDir::isAbsolutePath(normalized) || normalized.contains(':')
        || normalized.contains(QChar('\0'))) {
        error = QStringLiteral("文件路径必须相对于当前插件的数据目录"); return {};
    }
    for (const auto &part : parts) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) {
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
    const auto path = filePath(relativePath, false, result.error);
    if (!result.ok()) return result;
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
    const auto path = filePath(relativePath, true, result.error);
    if (!result.ok()) return result;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        result.error = file.errorString();
    return result;
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
        vusiif::ztgui::zerotier::command(request, {"-j", command},
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
