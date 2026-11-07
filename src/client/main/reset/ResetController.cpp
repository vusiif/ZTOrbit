#include "ResetController.h"

#include "AppLog.h"
#include "LocalConfig.h"
#include "ResetPlan.h"
#include "ResetScript.h"
#include "ZeroTierClient.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace vusiif::zto::reset {
namespace {

const QString kLabel = QStringLiteral("重置 ZeroTier 本地状态");
const QString kConfirmationTitle = QStringLiteral("重置 ZeroTier 本地状态");
const QString kConfirmationAccept = QStringLiteral("退出网络并重置");
const QString kConfirmationMessage = QStringLiteral(
    "将退出所有 ZeroTier 网络，并清除本机保存的本地状态：已加入网络的配置、本机发现的成员状态与缓存、"
    "Moon 文件、自定义 planet 根节点文件，以及过期的 TCP 回退中转设置。\n\n"
    "重置后本机将暂时无法连接这些网络；成员状态会由 ZeroTier 重新发现，不保留原来的数量或连接。"
    "节点身份、访问令牌和 ZeroTier 端口文件会保留。\n\n"
    "此操作只重启 ZeroTier 服务，不会重启 Z2。确定继续？");

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString sanitize(const QString &text)
{
    QString value = text.trimmed();
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    while (value.contains(QStringLiteral("  "))) value.replace(QStringLiteral("  "), QStringLiteral(" "));
    return value;
}

} // namespace

ResetController::ResetController(ui::SystemOperations &operations, QObject *parent)
    : QObject(parent), m_operations(operations)
{
}

ResetController::~ResetController() = default;

void ResetController::setBlockedProvider(std::function<bool()> blocked)
{
    m_blocked = std::move(blocked);
    emit canResetChanged();
}

bool ResetController::canReset() const
{
    return !m_busy && (!m_blocked || !m_blocked());
}

void ResetController::setStatus(const QString &status)
{
    if (m_status == status) return;
    m_status = status;
    emit statusChanged();
}

void ResetController::notify(const QString &message, bool error)
{
    emit notificationRequested(message, error);
}

void ResetController::endBusy()
{
    if (!m_busy) return;
    m_busy = false;
    emit busyChanged();
    emit canResetChanged();
}

void ResetController::requestReset()
{
    if (m_busy || m_awaitingConfirmation) return;
    if (!canReset()) {
        notify(QStringLiteral("另一项系统操作正在进行，请稍后再重置 ZeroTier 本地状态。"), true);
        return;
    }
    m_awaitingConfirmation = true;
    emit confirmationRequested(kConfirmationTitle, kConfirmationMessage, kConfirmationAccept);
}

void ResetController::confirm(bool accepted)
{
    if (!m_awaitingConfirmation) return;
    m_awaitingConfirmation = false;
    if (!accepted) {
        m_lastError.clear();
        const auto message = QStringLiteral("已取消重置，未做任何更改。");
        setStatus(message);
        notify(message, false);
        return;
    }
    if (m_busy) return;
    if (!canReset()) {
        notify(QStringLiteral("另一项系统操作正在进行，未执行重置。"), true);
        return;
    }
    beginReset();
}

void ResetController::beginReset()
{
    m_lastError.clear();
    m_scriptOutput.clear();
    m_sawElevated = m_sawDone = m_failed = m_recoveryFailed = m_notInstalled = false;
    m_failedStage.clear();
    m_failureDetail.clear();
    m_removalCount = 0;
    m_localConfigChanged = false;
    m_busy = true;
    emit busyChanged();
    emit canResetChanged();
    m_interruptionActive = true;
    emit interruptionBegan();
    setStatus(QStringLiteral("正在准备重置 ZeroTier 本地状态…"));
    vusiif::zto::util::log::write(QStringLiteral("重置 ZeroTier 本地状态：开始"));

    // The plan is built from the real system data directory. On macOS this is
    // /Library/Application Support/ZeroTier/One, never the temporary session
    // directory that only holds local API credentials and Moon copies.
    const auto dataRoot = vusiif::zto::zerotier::systemHomeDirectory();
    const auto snapshot = scanDataDirectory(dataRoot);
    if (!snapshot.valid()) {
        failBeforeElevation(QStringLiteral("无法检查 ZeroTier 数据目录，未做任何更改：%1").arg(snapshot.error));
        return;
    }

    QByteArray localConfig;
    bool localConfigExists = false;
    QString readError;
    if (!readLocalConfigBytes(localConfig, localConfigExists, readError)) {
        failBeforeElevation(QStringLiteral("无法读取 local.conf，未做任何更改：%1").arg(readError));
        return;
    }
    const auto plan = buildPlan(snapshot, localConfig, localConfigExists);
    if (!plan.valid()) {
        failBeforeElevation(QStringLiteral("无法建立安全的清理计划，未做任何更改：%1").arg(plan.error));
        return;
    }
    m_removalCount = plan.removals.size();
    m_localConfigChanged = plan.localConfigChanged;

    if (plan.empty()) {
        completeRun(QStringLiteral(
            "没有发现需要清理的本地 ZeroTier 状态（网络配置、成员缓存、Moon、自定义 planet 或过期中转设置）。"
            "未停止 ZeroTier 服务。"), false);
        return;
    }

    m_workDirectory = std::make_shared<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/zto-reset-XXXXXX"));
    if (!m_workDirectory->isValid()) {
        failBeforeElevation(QStringLiteral("无法创建重置过程的临时目录，未做任何更改。"));
        return;
    }

    ScriptOptions options;
    options.dataRoot = dataRoot;
    options.removalListPath = m_workDirectory->filePath(QStringLiteral("removals.txt"));
    options.localConfigChanged = plan.localConfigChanged;
    options.localConfigPath = vusiif::zto::zerotier::localConfigPath();
    options.cliPath = vusiif::zto::zerotier::executable();
    options.manageService = true;
    {
        QFile list(options.removalListPath);
        const auto lines = plan.removals.join(QLatin1Char('\n')).toUtf8()
            + (plan.removals.isEmpty() ? QByteArray() : QByteArray("\n"));
        if (!list.open(QIODevice::WriteOnly) || list.write(lines) != lines.size() || !list.flush()) {
            failBeforeElevation(QStringLiteral("无法准备本地状态删除列表，未做任何更改。"));
            return;
        }
    }
    if (plan.localConfigChanged) {
        options.proposedLocalConfigPath = m_workDirectory->filePath(QStringLiteral("local.conf"));
        QFile proposed(options.proposedLocalConfigPath);
        if (!proposed.open(QIODevice::WriteOnly) || proposed.write(plan.mergedLocalConfig) != plan.mergedLocalConfig.size()
            || !proposed.flush()) {
            failBeforeElevation(QStringLiteral("无法准备 local.conf 的重置内容，未做任何更改。"));
            return;
        }
        proposed.close();
        options.expectedLocalConfigSha256 = sha256Hex(plan.originalLocalConfig);
    }

    const auto script = resetScript(options);
    if (script.isEmpty()) {
        failBeforeElevation(QStringLiteral("当前平台不支持自动提权重置 ZeroTier 本地状态，未做任何更改。"));
        return;
    }

    vusiif::zto::util::log::write(
        QStringLiteral("重置 ZeroTier 本地状态：计划删除 %1 个文件%2")
            .arg(plan.removals.size())
            .arg(plan.localConfigChanged ? QStringLiteral("，并更新 local.conf") : QString()));
    m_outputConnection = connect(&m_operations, &ui::SystemOperations::output, this,
                                 [this](const QString &text) { m_scriptOutput.append(text); });
    auto work = m_workDirectory; // Keep the proposed local.conf alive for the elevated process.
    QPointer<ResetController> guard(this);
    m_operations.runElevatedScript(kLabel, script, [guard, work](bool ok) {
        if (guard) guard->finishElevation(ok);
    });
}

void ResetController::finishElevation(bool ok)
{
    if (m_outputConnection) {
        disconnect(m_outputConnection);
        m_outputConnection = {};
    }

    static const QRegularExpression marker(
        QStringLiteral("^ZTO-RESET stage=([A-Za-z]+) status=(ok|failed)(?: message=(.*))?$"));
    for (const auto &chunk : m_scriptOutput) {
        for (const auto &raw : chunk.split(QLatin1Char('\n'))) {
            const auto line = raw.trimmed();
            if (line.isEmpty()) continue;
            const auto match = marker.match(line);
            if (!match.hasMatch()) continue;
            const auto stage = match.captured(1);
            const auto status = match.captured(2);
            const auto detail = sanitize(match.captured(3));
            if (status == QStringLiteral("ok")) {
                if (stage == QStringLiteral("elevated")) m_sawElevated = true;
                else if (stage == QStringLiteral("done")) m_sawDone = true;
                else if (stage == QStringLiteral("recovery")) m_recoveryFailed = false;
            } else {
                m_failed = true;
                if (stage == QStringLiteral("recovery")) {
                    m_recoveryFailed = true;
                } else {
                    m_failedStage = stage;
                    m_failureDetail = detail;
                }
                if (stage == QStringLiteral("stop") && detail == QStringLiteral("not-installed"))
                    m_notInstalled = true;
            }
        }
    }

    const auto message = outcomeMessage(ok);
    const bool error = !(ok && m_sawDone);
    if (error) m_lastError = message;
    else m_lastError.clear();
    vusiif::zto::util::log::write(
        QStringLiteral("重置 ZeroTier 本地状态：%1").arg(message));
    setStatus(message);
    m_workDirectory.reset();
    endBusy();
    if (m_interruptionActive) {
        m_interruptionActive = false;
        emit interruptionEnded();
    }
    notify(message, error);
}

void ResetController::failBeforeElevation(const QString &message)
{
    m_lastError = message;
    vusiif::zto::util::log::write(QStringLiteral("重置 ZeroTier 本地状态：%1").arg(message));
    setStatus(message);
    m_workDirectory.reset();
    endBusy();
    if (m_interruptionActive) {
        m_interruptionActive = false;
        emit interruptionEnded();
    }
    notify(message, true);
}

void ResetController::completeRun(const QString &message, bool error)
{
    if (error) m_lastError = message;
    else m_lastError.clear();
    vusiif::zto::util::log::write(QStringLiteral("重置 ZeroTier 本地状态：%1").arg(message));
    setStatus(message);
    endBusy();
    if (m_interruptionActive) {
        m_interruptionActive = false;
        emit interruptionEnded();
    }
    notify(message, error);
}

QString ResetController::outcomeMessage(bool ok) const
{
    if (ok && m_sawDone) {
        QStringList parts;
        if (m_removalCount > 0)
            parts << QStringLiteral("已清除 %1 个本地状态文件").arg(m_removalCount);
        else
            parts << QStringLiteral("本地状态文件已清理");
        if (m_localConfigChanged) parts << QStringLiteral("已移除过期的 TCP 回退中转设置");
        return parts.join(QStringLiteral("，"))
            + QStringLiteral("；ZeroTier 服务已重启，节点身份和本地 API 令牌已保留。");
    }
    if (m_notInstalled)
        return QStringLiteral("未检测到已安装的 ZeroTier 服务，未做任何更改。");

    QString message;
    if (!m_failed) {
        if (!m_sawElevated)
            return QStringLiteral("授权被取消或提权脚本未执行，未做任何更改。");
        return QStringLiteral("重置未完成：清理脚本未报告完成状态。");
    }

    if (m_failedStage == QStringLiteral("begin") || m_failedStage == QStringLiteral("elevated")) {
        message = QStringLiteral("未通过数据目录安全检查，未做任何更改。");
    } else if (m_failedStage == QStringLiteral("stop")) {
        message = QStringLiteral("停止 ZeroTier 服务失败，未删除任何文件。");
    } else if (m_failedStage == QStringLiteral("cleanup")) {
        message = QStringLiteral("停止服务后清理本地状态失败，部分文件可能已删除。");
    } else if (m_failedStage == QStringLiteral("localconf")) {
        message = QStringLiteral("本地状态已清理，但更新 local.conf 失败。");
    } else if (m_failedStage == QStringLiteral("start")) {
        message = QStringLiteral("本地状态已清理，但 ZeroTier 服务启动或本地 API 就绪验证失败。");
    } else {
        message = QStringLiteral("重置未完成（阶段 %1）。").arg(m_failedStage);
    }

    if (m_recoveryFailed)
        message += QStringLiteral("恢复 ZeroTier 服务失败，请手动启动 ZeroTier 服务。");
    else if (m_failedStage != QStringLiteral("begin"))
        message += QStringLiteral("已尝试恢复 ZeroTier 服务。");

    if (!m_failureDetail.isEmpty())
        message += QStringLiteral("（%1）").arg(m_failureDetail);
    return message;
}

bool ResetController::readLocalConfigBytes(QByteArray &bytes, bool &exists, QString &error)
{
    // Reuse the existing local.conf reader (size limit, BOM/JSON validation and
    // permission classification) instead of duplicating its rules.
    const auto result = vusiif::zto::zerotier::localconfig::read(
        vusiif::zto::zerotier::localConfigPath());
    if (result.ok) {
        exists = result.state.exists;
        bytes = result.state.raw;
        return true;
    }
    // ZeroTier accepts an empty local.conf and keeps its defaults, while the
    // settings-page parser reports empty content as invalid JSON. Treat that
    // one case as an empty valid document for the reset merge.
    if (result.state.exists && result.state.raw.trimmed().isEmpty()) {
        exists = true;
        bytes = result.state.raw;
        return true;
    }
    error = result.error;
    return false;
}

} // namespace vusiif::zto::reset
