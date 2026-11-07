#include "UpdateController.h"

#include "AppLog.h"
#include "UpdateConfig.h"
#include "UpdateCrypto.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QProcess>
#include <QCryptographicHash>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>

namespace vusiif::zto::update {
namespace {

QString builtVersion()
{
#ifdef ZTO_VERSION
    return QStringLiteral(ZTO_VERSION);
#else
    return {};
#endif
}

void logLine(const QString &message)
{
    vusiif::zto::util::log::write(QStringLiteral("更新：") + message);
}

QString failureMessage(const QString &error)
{
    return QStringLiteral("更新失败：%1").arg(error);
}

struct StageOutcome {
    bool ok = false;
    QString error;
    QList<ExtractedFile> files;
};

} // namespace

UpdateController::UpdateController(const QString &executableDirectory, const QString &storageRoot,
                                   const QString &settingsFile, QObject *parent)
    : QObject(parent)
    , m_executableDirectory(executableDirectory)
    , m_storageRoot(storageRoot)
    , m_settingsFile(settingsFile)
    , m_paths(UpdatePaths::forStorageRoot(storageRoot))
{
    const auto applicationVersion = QCoreApplication::applicationVersion();
    m_currentVersion = applicationVersion.isEmpty() ? builtVersion() : applicationVersion;
    m_status = QStringLiteral("尚未检查更新");
    m_http = new UpdateHttp(this);
    connect(m_http, &UpdateHttp::manifestFinished, this, &UpdateController::handleManifest);
    m_startupTimer = new QTimer(this);
    m_startupTimer->setSingleShot(true);
    connect(m_startupTimer, &QTimer::timeout, this, [this] {
        if (m_startupHandled && m_phase == Phase::Idle && !busy()) {
            if (!readyForCheck(false)) return;
            startCheck(false);
        }
    });
    if (!m_storageRoot.isEmpty()) {
        QString prepareError;
        m_paths.prepare(prepareError);
        if (!prepareError.isEmpty()) logLine(prepareError);
    }
    // A helper result from the previous run is surfaced exactly once, on the
    // next startup, and then removed from the storage root.
    const auto record = UpdateResultRecord::take(m_paths.resultFile);
    if (record.present) {
        m_pendingResultNotify = true;
        if (record.ok) {
            m_installedMessage = record.version.isEmpty()
                ? QStringLiteral("更新已完成")
                : QStringLiteral("已更新到 %1").arg(record.version);
            logLine(m_installedMessage);
        } else {
            m_pendingResultError = true;
            m_lastError = record.message.isEmpty()
                ? QStringLiteral("上次更新未完成（%1）").arg(record.step)
                : QStringLiteral("上次更新未完成（%1）：%2").arg(record.step, record.message);
            m_state = QStringLiteral("failed");
            m_status = m_lastError;
            logLine(m_lastError);
        }
    }
    refreshConfiguration();
}

UpdateController::~UpdateController()
{
    if (m_downloader) { m_downloader->kill(); m_downloader->waitForFinished(3000); }
    if (m_http) m_http->abort();
}

QString UpdateController::serverIp() const { return QUrl(m_serverAddress).host(); }
int UpdateController::serverPort() const { return QUrl(m_serverAddress).port(UpdateConfig::DefaultPort); }

void UpdateController::setHostBusyProvider(std::function<bool()> provider)
{
    m_hostBusyProvider = std::move(provider);
}

bool UpdateController::busy() const
{
    return m_phase != Phase::Idle;
}

void UpdateController::setPhase(Phase phase)
{
    if (m_phase == phase) return;
    m_phase = phase;
    emit stateChanged();
}

void UpdateController::setState(const QString &state, const QString &status)
{
    m_state = state;
    m_status = status;
    emit stateChanged();
}

void UpdateController::notify(const QString &message, bool error)
{
    if (message.trimmed().isEmpty()) return;
    logLine(message);
    emit notificationRequested(message, error);
}

void UpdateController::refreshConfiguration()
{
    QString error;
    m_serverAddress = UpdateConfig::serverAddress(m_settingsFile, error);
    const auto url = m_serverAddress.isEmpty() ? QString() : m_serverAddress + QStringLiteral("/updater");
    m_feedUrl = url;
    m_publicKey.clear();
    QString reason;
    if (!error.isEmpty()) {
        reason = error;
    } else if (url.isEmpty()) {
        reason = QStringLiteral("未配置更新服务器");
    } else if (UpdateConfig::trustedPublicKeyBase64().isEmpty()) {
        reason = QStringLiteral("未配置更新服务器：缺少受信任的签名公钥");
    } else if (!UpdateCrypto::decodePublicKey(UpdateConfig::trustedPublicKeyBase64(), m_publicKey, error)) {
        reason = QStringLiteral("受信任的签名公钥无效：%1").arg(error);
    }
    m_configured = reason.isEmpty();
    if (!m_configured && (m_state == QLatin1String("idle") || m_state == QLatin1String("unconfigured"))) {
        m_state = QStringLiteral("unconfigured");
        m_status = reason;
    } else if (m_configured && m_state == QLatin1String("unconfigured")) {
        m_state = QStringLiteral("idle");
        m_status = QStringLiteral("尚未检查更新");
    }
    emit stateChanged();
}

bool UpdateController::readyForCheck(bool manual)
{
    refreshConfiguration();
    if (!m_configured) {
        const auto reason = m_status.isEmpty() ? QStringLiteral("未配置更新服务器") : m_status;
        // A failed transaction result is more important than the configuration
        // state, so it stays visible until the next check.
        if (m_state != QLatin1String("failed")) setState(QStringLiteral("unconfigured"), reason);
        if (manual) notify(reason, true);
        return false;
    }
    if (busy()) {
        if (manual) notify(QStringLiteral("正在检查更新，请稍候"), false);
        return false;
    }
    if (m_hostBusyProvider && m_hostBusyProvider()) {
        if (manual) notify(QStringLiteral("其他操作正在进行，请稍后再检查更新"), false);
        else scheduleStartupRetry();
        return false;
    }
    return true;
}

void UpdateController::scheduleStartupRetry()
{
    if (m_startupAttempts >= 60) return;
    ++m_startupAttempts;
    m_startupTimer->start(2000);
}

void UpdateController::checkOnStartup()
{
    if (m_startupHandled) return;
    m_startupHandled = true;
    if (!m_startupMaintenanceDone) {
        m_startupMaintenanceDone = true;
        UpdateStorage storage(m_paths);
        QString error;
        const auto notes = storage.recoverInterrupted(error);
        for (const auto &note : notes) logLine(note);
        if (!error.isEmpty()) {
            m_lastError = error;
            logLine(error);
        }
        // Surface the helper's result only after the recovery pass: a failed
        // transaction must be reported before another check starts.
        const auto record = UpdateResultRecord::take(m_paths.resultFile);
        if (record.present && !record.ok) {
            m_pendingResultNotify = true;
            m_pendingResultError = true;
            m_lastError = record.message.isEmpty()
                ? QStringLiteral("上次更新未完成（%1）").arg(record.step)
                : QStringLiteral("上次更新未完成（%1）：%2").arg(record.step, record.message);
            m_state = QStringLiteral("failed");
            m_status = m_lastError;
            emit stateChanged();
        }
    }
    if (m_pendingResultNotify) {
        notify(m_pendingResultError ? m_lastError : m_installedMessage, m_pendingResultError);
        m_pendingResultNotify = false;
        m_pendingResultError = false;
    }
    if (!readyForCheck(false)) return;
    startCheck(false);
}

void UpdateController::checkManually()
{
    if (m_startupHandled == false) m_startupHandled = true; // startup check is replaced by the manual one
    if (!readyForCheck(true)) return;
    startCheck(true);
}

void UpdateController::retry()
{
    if (busy()) return;
    if (m_state != QLatin1String("failed")) return;
    checkManually();
}

void UpdateController::startCheck(bool manual)
{
    m_manual = manual;
    m_lastError.clear();
    m_availableVersion.clear();
    m_updateAvailable = false;
    m_progress = 0;
    m_extractedFiles.clear();
    m_downloadPath.clear();
    m_stagingDirectory.clear();
    m_manifest = UpdateManifest{};
    m_artifact = UpdateArtifact{};
    setPhase(Phase::Checking);
    setState(QStringLiteral("checking"), QStringLiteral("检查中…"));
    m_http->checkManifest(m_feedUrl, m_publicKey);
}

bool UpdateController::setServer(const QString &ip, int port)
{
    if (busy()) {
        notify(QStringLiteral("正在更新，请稍后再修改更新服务器"), false);
        return false;
    }
    QString error;
    if (!UpdateConfig::setServer(m_settingsFile, ip, port, error)) {
        m_lastError = error;
        setState(QStringLiteral("failed"), failureMessage(error));
        notify(error, true);
        return false;
    }
    m_startupTimer->stop();
    m_state = QStringLiteral("idle");
    m_status = QStringLiteral("尚未检查更新");
    m_lastError.clear();
    m_availableVersion.clear();
    m_updateAvailable = false;
    m_progress = 0;
    refreshConfiguration();
    if (m_feedUrl.isEmpty()) {
        notify(QStringLiteral("已清空更新服务器，不再检查更新"), false);
    } else if (!m_configured) {
        notify(QStringLiteral("已保存更新服务器地址；%1").arg(m_status), true);
    } else {
        notify(QStringLiteral("已保存更新服务器地址"), false);
    }
    return true;
}

void UpdateController::handleManifest(bool ok, const QByteArray &bytes, const UpdateManifest &manifest,
                                      const UpdateArtifact &artifact, const QString &error)
{
    Q_UNUSED(bytes)
    if (!ok) {
        fail(error, m_manual);
        return;
    }
    m_manifest = manifest;
    m_artifact = artifact;
    const auto version = artifact.version;
    bool parseError = false;
    const bool newer = versionIsNewer(version, m_currentVersion, &parseError);
    if (parseError) {
        fail(QStringLiteral("当前版本或更新版本不是有效的 SemVer：%1 / %2")
                 .arg(m_currentVersion, version),
             m_manual);
        return;
    }
    if (!newer) {
        setPhase(Phase::Idle);
        m_updateAvailable = false;
        m_progress = 0;
        setState(QStringLiteral("uptodate"),
                 QStringLiteral("已是最新（当前 %1）").arg(m_currentVersion));
        if (m_manual) notify(QStringLiteral("已是最新（当前 %1）").arg(m_currentVersion), false);
        return;
    }
    const auto minimum = parseVersion(manifest.minUpdatableVersion);
    const auto current = parseVersion(m_currentVersion);
    if (!minimum || !current) {
        fail(QStringLiteral("更新清单的最低可升级版本无效"), m_manual);
        return;
    }
    if (compareVersions(*current, *minimum) < 0) {
        // A current version below the manifest's minimum must never install;
        // the user has to perform a full manual installation.
        fail(QStringLiteral("当前版本 %1 低于可直接升级的最低版本 %2，请手动安装完整安装包")
                 .arg(m_currentVersion, manifest.minUpdatableVersion),
             m_manual);
        return;
    }
    m_availableVersion = version;
    m_updateAvailable = true;
    m_progress = 0;
    emit stateChanged();
    notify(QStringLiteral("发现新版本 %1，正在下载…").arg(version), false);

    UpdateStorage storage(m_paths);
    QString downloadError;
    m_downloadPath = storage.downloadFilePath(version, downloadError);
    if (m_downloadPath.isEmpty()) {
        fail(downloadError, true);
        return;
    }
    // Fail honestly before downloading when the installation directory cannot be
    // written: the helper inherits the elevated token, but a read-only folder
    // would still make the transaction fail after the user already quit.
    QTemporaryFile probe(QDir(m_executableDirectory).filePath(QStringLiteral(".z2-update-probe-XXXXXX")));
    if (!probe.open()) {
        fail(QStringLiteral("安装目录不可写：%1").arg(QDir::toNativeSeparators(m_executableDirectory)),
             m_manual);
        return;
    }
    probe.remove();
    setPhase(Phase::Downloading);
    setState(QStringLiteral("downloading"),
             QStringLiteral("正在下载 %1… 0%").arg(version));
    startDownload();
}

void UpdateController::startDownload()
{
    const QUrl server(m_serverAddress), file(m_artifact.url);
    if (file.scheme() != server.scheme() || file.host() != server.host()
        || file.port(80) != server.port(80) || file.path() != QLatin1String("/file")
        || QUrlQuery(file).queryItemValue(QStringLiteral("version")) != m_availableVersion) {
        fail(QStringLiteral("下载接口无效"), true);
        return;
    }
    UpdateStorage storage(m_paths);
    const auto helper = storage.helperExecutable(m_executableDirectory);
    auto *process = new QProcess(this);
    m_downloader = process;
    process->setWorkingDirectory(m_executableDirectory);
    process->setProgram(helper);
    process->setArguments({QStringLiteral("--download"), m_artifact.url, QStringLiteral("--output"),
        m_downloadPath, QStringLiteral("--size"), QString::number(m_artifact.size),
        QStringLiteral("--sha256"), m_artifact.sha256});
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        while (process->canReadLine()) {
            const auto line = process->readLine().trimmed();
            if (!line.startsWith("progress=")) continue;
            bool ok = false;
            const auto value = line.mid(9).toInt(&ok);
            if (!ok || value < 0 || value > 100) continue;
            m_progress = value;
            m_status = QStringLiteral("正在下载 %1… %2%").arg(m_availableVersion).arg(value);
            emit stateChanged();
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_downloader != process) return;
        m_downloader = nullptr;
        process->deleteLater();
        fail(QStringLiteral("无法启动更新程序"), true);
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        if (m_downloader != process) return;
        m_downloader = nullptr;
        const auto error = QString::fromUtf8(process->readAllStandardError().left(4096)).trimmed();
        process->deleteLater();
        handleDownload(status == QProcess::NormalExit && code == 0, m_downloadPath,
                       error.isEmpty() ? QStringLiteral("更新下载失败") : error);
    });
    process->start();
}

void UpdateController::handleDownload(bool ok, const QString &path, const QString &error)
{
    if (!ok) {
        fail(error, m_manual || m_updateAvailable);
        return;
    }
    m_downloadPath = path;
    stagePackage();
}

void UpdateController::stagePackage()
{
    setPhase(Phase::Verifying);
    m_progress = 100;
    setState(QStringLiteral("verifying"), QStringLiteral("正在校验并解压更新包…"));
    UpdateStorage storage(m_paths);
    QString error;
    const auto staging = storage.createStagingDirectory(m_availableVersion, error);
    if (staging.isEmpty()) {
        fail(error, m_manual || m_updateAvailable);
        return;
    }
    m_stagingDirectory = staging;
    const auto package = m_downloadPath;
    const auto version = m_availableVersion;
    auto *watcher = new QFutureWatcher<StageOutcome>(this);
    connect(watcher, &QFutureWatcher<StageOutcome>::finished, this, [this, watcher, version] {
        const auto outcome = watcher->result();
        watcher->deleteLater();
        if (!outcome.ok) {
            fail(outcome.error, m_manual || m_updateAvailable);
            return;
        }
        m_extractedFiles = outcome.files;
        Q_UNUSED(version)
        handleStaged();
    });
    const auto expectedSize = m_artifact.size;
    const auto expectedHash = m_artifact.sha256;
    watcher->setFuture(QtConcurrent::run([package, staging, expectedSize, expectedHash] {
        StageOutcome outcome;
        QFile file(package);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || file.size() != expectedSize || !hash.addData(&file)
            || QString::fromLatin1(hash.result().toHex()) != expectedHash) {
            outcome.error = QStringLiteral("更新包大小或 SHA-256 校验失败");
            return outcome;
        }
        ArchiveLimits limits;
        QString extractError;
        outcome.ok = UpdateArchive::extract(package, staging, limits, outcome.files, extractError);
        outcome.error = extractError;
        return outcome;
    }));
}

void UpdateController::handleStaged()
{
    UpdateStorage storage(m_paths);
    const auto transactionId = QFileInfo(m_stagingDirectory).fileName();
    updater::Plan plan;
    plan.formatVersion = 1;
    plan.packageVersion = m_availableVersion.toStdString();
    plan.installDir = QDir::toNativeSeparators(m_executableDirectory).toStdString();
    plan.stagingDir = QDir::toNativeSeparators(m_stagingDirectory).toStdString();
    plan.transactionDir = QDir::toNativeSeparators(
        QDir(m_paths.transactions).filePath(transactionId)).toStdString();
    plan.runtimeDir = QDir::toNativeSeparators(m_paths.runtime).toStdString();
    plan.logFile = QDir::toNativeSeparators(m_paths.logFile).toStdString();
    plan.resultFile = QDir::toNativeSeparators(m_paths.resultFile).toStdString();
    plan.parentPid = static_cast<std::uint64_t>(QCoreApplication::applicationPid());
    auto restartName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    if (restartName.isEmpty()) restartName = QStringLiteral("Z2.exe");
    plan.restartRelative = restartName.toStdString();
    for (const auto &file : m_extractedFiles) {
        updater::PlanFile entry;
        entry.sha256 = file.sha256.toStdString();
        entry.size = static_cast<std::uint64_t>(file.size);
        entry.path = file.path.toStdString();
        plan.files.push_back(entry);
    }
    for (const auto &removal : m_artifact.removeFiles) {
        plan.removals.push_back(removal.toStdString());
    }
    std::string planError;
    if (!updater::validatePlan(plan, planError)) {
        fail(QStringLiteral("更新计划无效：%1").arg(QString::fromStdString(planError)),
             m_manual || m_updateAvailable);
        return;
    }
    QString error;
    const auto planPath = storage.writePlan(plan, error);
    if (planPath.isEmpty()) {
        fail(error, m_manual || m_updateAvailable);
        return;
    }
    setPhase(Phase::Staging);
    setState(QStringLiteral("staging"), QStringLiteral("正在准备安装…"));
    const auto helper = m_helperOverride.isEmpty()
        ? storage.helperExecutable(m_executableDirectory)
        : m_helperOverride;
    qint64 pid = 0;
    if (!storage.launchHelper(helper, planPath, m_executableDirectory, m_noRestartForTesting, &pid,
                              error)) {
        fail(error, true);
        return;
    }
    logLine(QStringLiteral("已启动更新程序（PID %1），等待主程序退出").arg(pid));
    QFile::remove(m_downloadPath);
    m_downloadPath.clear();
    setPhase(Phase::Preparing);
    m_progress = 100;
    setState(QStringLiteral("preparing"), QStringLiteral("更新已准备就绪，正在退出以完成安装…"));
    emit quitRequested();
}

void UpdateController::fail(const QString &error, bool notifyError)
{
    m_lastError = error;
    setPhase(Phase::Idle);
    setState(QStringLiteral("failed"), failureMessage(error));
    logLine(error);
    if (notifyError) notify(error, true);
}

} // namespace vusiif::zto::update
