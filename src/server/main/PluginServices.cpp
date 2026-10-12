#include "PluginServices.h"
#include "ServerHost.h"
#include "AppLog.h"
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QDateTime>
#include <limits>
namespace vusiif::ztgui::server {
using util::FileResult;
using util::LogResult;
using util::LogLevel;
using util::logLevelName;
namespace log = util::log;
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
PluginServices::PluginServices(ServerHost &host, const QString &id, const QString &name, const QString &root)
    : m_host(host), m_id(id), m_sharedDataRoot(QDir(root).filePath("plugins-data")),
      m_pluginName(name), m_dataRoot(QDir(m_sharedDataRoot).filePath(name)), m_queries(this) {}
quint16 PluginServices::port() const { return m_host.port(); }
QHostAddress PluginServices::peerAddress(api::ConnectionId id) const { return m_host.peerAddress(id); }
bool PluginServices::sendTcp(api::ConnectionId id, const QByteArray &data) { return m_host.sendTcp(m_id,id,data); }
bool PluginServices::sendUdp(const api::Endpoint &endpoint, const QByteArray &data) { return m_host.sendUdp(m_id,endpoint,data); }
void PluginServices::disconnect(api::ConnectionId id) { m_host.disconnect(id); }
qint64 PluginServices::queuedTcpBytes(api::ConnectionId id) const { return m_host.queuedTcpBytes(id); }
LogResult PluginServices::write(LogLevel level, const QString &message)
{
    const QMutexLocker lock(&fileMutex);
    const QString text = logLevelName(level) + " [" + m_id + "] " + message.trimmed();
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

QString PluginServices::filePath(const QString &relativePath, bool writing, QString &error) const
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
void PluginServices::sendDataTo(const QString &network, const QString &member, const QByteArray &data,
                               QObject *context, util::SendCallback completed)
{
    m_queries.query("controller.members", {{"networkId", network}}, context,
        [this, member, data, completed](const api::JsonResult &result) {
            if (!completed) return;
            if (!result.ok()) { completed({0, result.error}); return; }
            if (member.isEmpty()) { completed({0, QStringLiteral("成员 ID 不能为空")}); return; }
            completed(util::sendNetworkData(result.document.array(), member, m_id.toLatin1(), data));
        });
}
void PluginServices::broadcastDataTo(const QString &network, const QByteArray &data,
                                    QObject *context, util::SendCallback completed)
{
    m_queries.query("controller.members", {{"networkId", network}}, context,
        [this, data, completed](const api::JsonResult &result) {
            if (!completed) return;
            if (!result.ok()) { completed({0, result.error}); return; }
            completed(util::sendNetworkData(result.document.array(), {}, m_id.toLatin1(), data));
        });
}
FileResult PluginServices::readFile(const QString &relativePath)
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
FileResult PluginServices::writeFile(const QString &relativePath, const QByteArray &data)
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
}
