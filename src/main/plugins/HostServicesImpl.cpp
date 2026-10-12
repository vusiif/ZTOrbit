#include "HostServicesImpl.h"
#include "../ZeroTierClient.h"
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
HostServicesImpl::HostServicesImpl(const QString &pluginName, const QString &dataRoot)
    : m_sharedDataRoot(QDir::cleanPath(dataRoot.isEmpty()
          ? QDir(QCoreApplication::applicationDirPath()).filePath("plugins-data")
          : QFileInfo(dataRoot).absoluteFilePath())),
      m_pluginName(pluginName), m_dataRoot(QDir(m_sharedDataRoot).filePath(pluginName)) {}
HostServicesImpl::~HostServicesImpl() { stopRequests(); }

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
    QTimer::singleShot(0, request, [request, target, command, object, completed = std::move(completed)] {
        vusiif::ztgui::zerotier::command(request, {"-j", command},
            [request, target, object, completed](bool success, const QString &text) {
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
}
