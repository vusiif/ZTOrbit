#include "ZeroTierClient.h"
#include "AppLog.h"
#include "MacPlatform.h"
#include "WindowsPortScript.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <memory>
#include <QRegularExpression>
#if defined(Q_OS_MACOS)
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#include <winsvc.h>
#endif

namespace {
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
vusiif::ztgui::zerotier::ServiceStatus macService;
bool macApiAvailable = false;
std::shared_ptr<QTemporaryDir> macSession()
{
    static auto directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/ztgui-session-XXXXXX");
    return directory;
}
bool sessionReady() { return QFileInfo::exists(macSession()->filePath("authtoken.secret")); }
#endif
}

std::function<void(bool)> vusiif::ztgui::zerotier::createServiceMonitor(QObject *owner)
{
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
    auto *process = new QProcess(owner);
    auto *timer = new QTimer(process);
    timer->setInterval(1000);
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    timeout->setInterval(5000);
    auto pending = std::make_shared<bool>(false);
    const auto refresh = [process, timeout, pending] {
        if (process->state() != QProcess::NotRunning) return;
        *pending = true;
        process->start("/bin/launchctl", {"print", "system/com.zerotier.one"});
        timeout->start();
    };
    QObject::connect(timeout, &QTimer::timeout, process, [process] { process->kill(); });
    QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process,
        [process, timer, timeout, pending, refresh](int code, QProcess::ExitStatus status) {
            timeout->stop();
            if (!*pending) {
                process->readAllStandardOutput();
                if (timer->isActive()) QTimer::singleShot(0, process, refresh);
                return;
            }
            *pending = false;
            if (!timer->isActive()) return;
            macService.exists = QFileInfo::exists("/Library/LaunchDaemons/com.zerotier.one.plist");
            macService.running = macApiAvailable || (code == 0 && status == QProcess::NormalExit
                && QRegularExpression("state\\s*=\\s*running").match(QString::fromUtf8(process->readAllStandardOutput())).hasMatch());
            macService.known = status == QProcess::NormalExit;
        });
    QObject::connect(timer, &QTimer::timeout, process, refresh);
    return [process, timer, timeout, pending, refresh](bool active) {
        if (active == timer->isActive()) return;
        if (active) { timer->start(); refresh(); }
        else { timer->stop(); timeout->stop(); *pending = false; if (process->state() != QProcess::NotRunning) process->kill(); }
    };
#else
    Q_UNUSED(owner)
    return {};
#endif
}

vusiif::ztgui::zerotier::ServiceStatus vusiif::ztgui::zerotier::serviceStatus()
{
#ifdef Q_OS_WIN
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return {false, false, false};
    SC_HANDLE service = OpenServiceW(manager, L"ZeroTierOneService", SERVICE_QUERY_STATUS);
    if (!service) {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager);
        return {false, false, error == ERROR_SERVICE_DOES_NOT_EXIST};
    }
    SERVICE_STATUS_PROCESS status = {};
    DWORD bytes = 0;
    const bool ok = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return {true, ok && status.dwCurrentState == SERVICE_RUNNING, ok};
#elif defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
    return macService;
#else
    return {!executable().isEmpty(), false, true};
#endif
}

QString vusiif::ztgui::zerotier::matchingMoonFile(const QString &networkId)
{
    if (!QRegularExpression("^[0-9a-fA-F]{16}$").match(networkId).hasMatch()) return {};
    const QDir directory(moonDirectory());
    for (const auto &file : directory.entryInfoList(QDir::Files, QDir::Name)) {
        const QString stem = file.completeBaseName();
        if (file.suffix().compare("moon", Qt::CaseInsensitive) == 0
            && QRegularExpression("^[0-9a-fA-F]{16}$").match(stem).hasMatch()
            && stem.right(10).compare(networkId.left(10), Qt::CaseInsensitive) == 0)
            return file.absoluteFilePath();
    }
    return {};
}

QString vusiif::ztgui::zerotier::dataDirectory()
{
#ifdef ZEROTIER_TESTING
    const QString testing = qEnvironmentVariable("ZT_TEST_DATA_DIRECTORY");
    if (!testing.isEmpty()) return testing;
#endif
#ifdef Q_OS_WIN
    return QDir(qEnvironmentVariable("ProgramData", "C:/ProgramData")).filePath("ZeroTier");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("/Library/Application Support/ZeroTier");
#else
    return QStringLiteral("/var/lib/zerotier-one");
#endif
}

QString vusiif::ztgui::zerotier::homeDirectory()
{
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
    return macSession()->path();
#else
    return systemHomeDirectory();
#endif
}

QString vusiif::ztgui::zerotier::systemHomeDirectory()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return QDir(dataDirectory()).filePath("One");
#else
    return dataDirectory();
#endif
}

QString vusiif::ztgui::zerotier::moonDirectory() { return QDir(homeDirectory()).filePath("moons.d"); }
QString vusiif::ztgui::zerotier::moonWriteDirectory() { return QDir(systemHomeDirectory()).filePath("moons.d"); }

QString vusiif::ztgui::zerotier::sessionRefreshScript()
{
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
    return vusiif::ztgui::platform::synchronizeSession(systemHomeDirectory(), homeDirectory(), getuid());
#else
    return {};
#endif
}

QString vusiif::ztgui::zerotier::executable()
{
    const auto cli = QStandardPaths::findExecutable("zerotier-cli");
    if (!cli.isEmpty()) {
        const auto info = QFileInfo(cli);
        // Windows ships a .bat wrapper. QProcess runs the sibling binary directly.
        if (info.suffix().compare("bat", Qt::CaseInsensitive) != 0 &&
            info.suffix().compare("cmd", Qt::CaseInsensitive) != 0) return cli;
        for (const auto &binary : {"zerotier-one_x64.exe", "zerotier-one_x86.exe"}) {
            const auto path = info.dir().filePath(binary);
            if (QFileInfo::exists(path)) return path;
        }
    }
    const QStringList candidates = {
#ifdef Q_OS_MACOS
        QStringLiteral("/usr/local/bin/zerotier-cli"),
        QDir(systemHomeDirectory()).filePath("zerotier-cli"),
        QDir(systemHomeDirectory()).filePath("zerotier-one")
#else
        QDir(homeDirectory()).filePath("zerotier-cli_x64.exe"),
        QDir(homeDirectory()).filePath("zerotier-one_x64.exe"),
        QDir(homeDirectory()).filePath("zerotier-one_x86.exe"),
        QDir(qEnvironmentVariable("ProgramFiles(x86)", "C:/Program Files (x86)"))
            .filePath("ZeroTier/One/zerotier-one_x64.exe"),
        QDir(qEnvironmentVariable("ProgramFiles", "C:/Program Files"))
            .filePath("ZeroTier/One/zerotier-one_x64.exe")
#endif
    };
    for (const auto &candidate : candidates)
        if (QFileInfo::exists(candidate)) return candidate;
    return {};
}

static QStringList cliArguments(const QString &program, const QStringList &arguments)
{
    auto result = arguments;
    if (QFileInfo(program).fileName().startsWith("zerotier-one")) result.prepend("-q");
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
    result.prepend("-D" + vusiif::ztgui::zerotier::homeDirectory());
#endif
    return result;
}

QString vusiif::ztgui::zerotier::translate(const QString &value)
{
    static const QHash<QString, QString> labels = {
        {"PLANET", "根节点"}, {"MOON", "中转站"}, {"LEAF", "成员"},
        {"DIRECT", "直链"}, {"RELAY", "中转"}, {"OK", "已连接"},
        {"ONLINE", "在线"}, {"OFFLINE", "离线"}, {"PRIVATE", "私有"},
        {"PUBLIC", "公开"}, {"ACCESS_DENIED", "等待授权"},
        {"REQUESTING_CONFIGURATION", "获取配置中"}, {"NOT_FOUND", "网络不存在"},
        {"PORT_ERROR", "端口错误"}, {"CLIENT_TOO_OLD", "客户端版本过旧"},
        {"AUTHENTICATION_REQUIRED", "需要身份验证"},
        {"address", "节点 ID"}, {"id", "ID"}, {"nwid", "网络 ID"},
        {"name", "名称"}, {"role", "角色"}, {"latency", "延迟（ms）"},
        {"paths", "连接路径"}, {"version", "版本"}, {"status", "状态"},
        {"type", "类型"}, {"assignedAddresses", "分配的 IP"},
        {"online", "在线"}, {"tcpFallbackActive", "TCP 回退"},
        {"roots", "根节点"}, {"stableEndpoints", "固定地址"},
        {"identity", "身份"}, {"timestamp", "时间戳"}, {"signature", "签名"},
        {"active", "有效"}, {"preferred", "首选"}, {"trustedPathId", "可信路径 ID"},
        {"lastReceive", "最近接收"}, {"lastSend", "最近发送"},
        {"portDeviceName", "虚拟网卡"}, {"mac", "MAC 地址"}, {"mtu", "MTU"},
        {"allowManaged", "允许托管地址"}, {"allowGlobal", "允许全局地址"},
        {"allowDefault", "允许默认路由"}, {"allowDNS", "允许 DNS"},
        {"routes", "路由"}, {"target", "目标"}, {"via", "网关"},
        {"dns", "DNS"}, {"servers", "服务器"}, {"domain", "域名"},
        {"worldId", "世界 ID"}, {"worldType", "世界类型"},
        {"config", "配置"}, {"settings", "设置"}, {"homeDir", "数据目录"},
        {"moonFile", "Moon 文件"}, {"localName", "本机名称"}
    };
    return labels.value(value, value);
}

QString vusiif::ztgui::zerotier::jsonText(const QJsonValue &value)
{
    if (value.isBool()) return value.toBool() ? QStringLiteral("是") : QStringLiteral("否");
    if (value.isNull() || value.isUndefined()) return QStringLiteral("—");
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    return translate(value.toVariant().toString());
}

QString vusiif::ztgui::zerotier::quotePowerShell(const QString &value)
{
    QString escaped = value;
    escaped.replace('\\', '/');
    escaped.replace("'", "''");
    return "'" + escaped + "'";
}

QString vusiif::ztgui::zerotier::serviceCommands(const QString &action)
{
#ifdef Q_OS_MACOS
    return vusiif::ztgui::platform::serviceCommands(action, executable());
#else
    const QString stop = "try { Stop-Service -Name ZeroTierOneService -ErrorAction Stop } catch {}\n"
        "(Get-Service -Name ZeroTierOneService -ErrorAction Stop).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))\n";
    const QString start = windowsPortPreflight() + "Start-Service -Name ZeroTierOneService -ErrorAction Stop\n"
        "(Get-Service -Name ZeroTierOneService -ErrorAction Stop).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))\n"
        + windowsServiceReady();
    if (action == "stop") return stop;
    if (action == "start") return start;
    if (action == "restart") return stop + start;
    return {};
#endif
}

QString vusiif::ztgui::zerotier::wrapScript(const QString &commands)
{
#ifdef Q_OS_MACOS
    return "set -eu\nPATH=/usr/bin:/bin:/usr/sbin:/sbin\nexport PATH\n" + commands;
#else
    return "$ErrorActionPreference = 'Stop'\ntry {\n" + commands + "exit 0\n} catch { Write-Output $_; exit 1 }";
#endif
}

QString vusiif::ztgui::zerotier::moonImportScript(const QList<QPair<QString, QString>> &files)
{
#ifdef Q_OS_MACOS
    return wrapScript(vusiif::ztgui::platform::copyMoons(files, moonWriteDirectory()) + serviceCommands("restart"));
#else
    QString copies = "New-Item -ItemType Directory -Path " + quotePowerShell(moonWriteDirectory()) + " -Force | Out-Null\n";
    for (const auto &file : files) copies += "Copy-Item -LiteralPath " + quotePowerShell(file.first)
        + " -Destination " + quotePowerShell(file.second) + " -Force -ErrorAction Stop\n";
    return wrapScript(copies + serviceCommands("restart"));
#endif
}

QString vusiif::ztgui::zerotier::moonDeleteScript(const QString &path)
{
#ifdef Q_OS_MACOS
    if (QFileInfo(path).absolutePath() != QDir(moonDirectory()).absolutePath()) return "exit 1\n";
    const QString actual = QDir(moonWriteDirectory()).filePath(QFileInfo(path).fileName());
    return wrapScript(vusiif::ztgui::platform::deleteMoon(actual, moonWriteDirectory()) + serviceCommands("restart"));
#else
    return wrapScript("$target = [IO.Path]::GetFullPath(" + quotePowerShell(path)
        + ")\n$directory = [IO.Path]::GetFullPath(" + quotePowerShell(moonWriteDirectory()) + ")\n"
        "if ([IO.Path]::GetDirectoryName($target) -ne $directory -or [IO.Path]::GetExtension($target) -ne '.moon') { throw 'Invalid Moon path' }\n"
        "Remove-Item -LiteralPath $target -Force -ErrorAction Stop\n" + serviceCommands("restart"));
#endif
}

vusiif::ztgui::zerotier::PowerShellInvocation vusiif::ztgui::zerotier::preparePowerShell(const QString &script)
{
    PowerShellInvocation invocation;
    invocation.directory = std::make_shared<QTemporaryDir>(
        QDir::tempPath() + "/zerotier-command-XXXXXX");
    if (!invocation.directory->isValid()) return invocation;
    // The child creates this file. QTemporaryFile::close() would retain an
    // exclusive Windows file handle and prevent PowerShell from opening it.
    invocation.logPath = invocation.directory->filePath("output.log");
    const QString wrapped = "$ProgressPreference = 'SilentlyContinue'\n"
        "$PSDefaultParameterValues['Out-File:Encoding'] = 'utf8'\n& {\n"
        + script + "\n} *> " + quotePowerShell(invocation.logPath);
    const auto encoded = QByteArray(reinterpret_cast<const char *>(wrapped.utf16()),
                                    wrapped.size() * 2).toBase64();
    invocation.arguments = {"-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden",
                            "-EncodedCommand", QString::fromLatin1(encoded)};
    return invocation;
}

QString vusiif::ztgui::zerotier::packageScript(bool uninstall, const QString &winget)
{
#ifdef Q_OS_MACOS
    Q_UNUSED(winget)
    return vusiif::ztgui::platform::packageScript(uninstall, dataDirectory());
#else
    QString script = "$ErrorActionPreference = 'Stop'\n& " + quotePowerShell(winget);
    script += uninstall
        ? " uninstall --id ZeroTier.ZeroTierOne --exact --source winget --silent --disable-interactivity\n"
        : " install --id ZeroTier.ZeroTierOne --exact --source winget --silent --accept-package-agreements --accept-source-agreements --disable-interactivity\n";
    script += "$code = $LASTEXITCODE\nif ($code -notin @(0, 3010, 1641)) { exit $code }\n";
    if (uninstall) {
        // This target is fixed, never supplied by a text field or by CLI JSON.
        // Resolve and validate it in the same shell that performs the deletion.
        script += R"PS($expected = [IO.Path]::GetFullPath((Join-Path $env:ProgramData 'ZeroTier'))
$target = [IO.Path]::GetFullPath()PS" + quotePowerShell(dataDirectory()) + R"PS()
if ($target -ne $expected -or [IO.Path]::GetFileName($target) -ne 'ZeroTier') { exit 1 }
if (Test-Path -LiteralPath $target) {
    $item = Get-Item -LiteralPath $target -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { exit 1 }
    $links = Get-ChildItem -LiteralPath $target -Recurse -Force -ErrorAction Stop |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }
    if ($links) { exit 1 }
    Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop
    if (Test-Path -LiteralPath $target) { exit 1 }
}
)PS";
    } else {
        script += serviceCommands("start");
    }
    return "try {\n" + script + "\nexit 0\n} catch { Write-Output $_; exit 1 }";
#endif
}

void vusiif::ztgui::zerotier::command(QObject *owner, const QStringList &arguments,
                       std::function<void(bool, const QString &)> finished)
{
    const QString program = executable();
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
    if (!sessionReady()) { finished(false, QStringLiteral("尚未授权读取 ZeroTier，请操作服务后重试。")); return; }
#endif
    if (program.isEmpty()) {
        finished(false, QStringLiteral("未找到 ZeroTier，请先安装。"));
        return;
    }
    auto *process = new QProcess(owner);
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    timeout->setInterval(15000);
    auto completed = std::make_shared<bool>(false);
    auto complete = [process, finished, completed, arguments](bool ok, const QString &text) {
        if (*completed) return;
        *completed = true;
        vusiif::ztgui::util::log::write(arguments.join(' ') + (ok ? "：完成 " : "：失败 ") + text);
        process->deleteLater();
        finished(ok, text);
    };
    QObject::connect(timeout, &QTimer::timeout, process, [process, complete] {
        process->kill();
        complete(false, QStringLiteral("操作超时，请检查 ZeroTier 服务。"));
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete(false, QStringLiteral("无法启动 ZeroTier 命令。"));
    });
    QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     process, [process, complete](int code, QProcess::ExitStatus status) {
        const QString text = QString::fromLocal8Bit(process->readAllStandardOutput())
                           + QString::fromLocal8Bit(process->readAllStandardError());
        complete(code == 0 && status == QProcess::NormalExit, text.trimmed());
    });
    process->start(program, cliArguments(program, arguments));
    timeout->start();
}

vusiif::ztgui::zerotier::JsonPoller::JsonPoller(QObject *owner, const QString &command, bool object)
    : QObject(owner), m_command(command), m_object(object)
{
    m_interval.setInterval(1000);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(5000);
    connect(&m_interval, &QTimer::timeout, this, &vusiif::ztgui::zerotier::JsonPoller::refresh);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (!m_requestPending) return;
        m_requestPending = false;
        m_process.kill();
        failed(QStringLiteral("ZeroTier 响应超时，正在自动重试"));
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_requestPending) return;
        m_requestPending = false;
        m_timeout.stop();
        failed(QStringLiteral("无法读取 ZeroTier，请检查安装或管理员权限"));
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
        if (!m_requestPending) {
            m_process.readAllStandardOutput(); m_process.readAllStandardError();
            const bool restart = m_restartWhenFinished && m_active;
            m_restartWhenFinished = false;
            if (restart) QTimer::singleShot(0, this, &JsonPoller::refresh);
            return;
        }
        m_requestPending = false;
        m_timeout.stop();
        auto document = QJsonDocument::fromJson(m_process.readAllStandardOutput());
        if (code != 0 || status != QProcess::NormalExit ||
            (m_object ? !document.isObject() : !document.isArray())) {
            failed(QStringLiteral("服务未响应，请检查服务状态或以管理员身份运行"));
            return;
        }
        if (!m_lastFailure.isEmpty()) vusiif::ztgui::util::log::write(m_command + QStringLiteral("：恢复连接"));
        m_lastFailure.clear();
        m_failures = 0;
        m_failurePeriod.invalidate();
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
        if (m_command == "info") macApiAvailable = true;
#endif
        emit availabilityChanged(true, QStringLiteral("每 1 秒自动更新"));
        emit updated(document);
    });

}

void vusiif::ztgui::zerotier::JsonPoller::failed(const QString &message)
{
    if (!m_failurePeriod.isValid()) m_failurePeriod.start();
    ++m_failures;
    // Keep the last good snapshot through transient CLI/API failures. Both a
    // retry budget and a grace period must expire before reporting offline.
    if (m_failures < 3 || m_failurePeriod.elapsed() < 5000) return;
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
    if (m_command == "info") macApiAvailable = false;
#endif
    if (m_lastFailure != message) vusiif::ztgui::util::log::write(m_command + "：" + message);
    m_lastFailure = message;
    emit availabilityChanged(false, message);
}

vusiif::ztgui::zerotier::JsonPoller::~JsonPoller()
{
    m_interval.stop();
    m_timeout.stop();
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

void vusiif::ztgui::zerotier::JsonPoller::refresh()
{
    if (!m_active || m_process.state() != QProcess::NotRunning) return;
    m_restartWhenFinished = false;
#if defined(Q_OS_MACOS) && !defined(ZEROTIER_TESTING)
    if (!sessionReady()) { failed(QStringLiteral("等待授权读取 ZeroTier")); return; }
#endif
    const QString program = vusiif::ztgui::zerotier::executable();
    if (program.isEmpty()) {
        failed(QStringLiteral("未安装 ZeroTier，请在主页安装"));
        return;
    }
    m_requestPending = true;
    m_process.start(program, cliArguments(program, {"-j", m_command}));
    m_timeout.start();
}

void vusiif::ztgui::zerotier::JsonPoller::setActive(bool active)
{
    if (m_active == active) return;
    m_active = active;
    if (active) { m_interval.start(); refresh(); }
    else {
        m_interval.stop(); m_timeout.stop(); m_requestPending = false;
        m_failures = 0; m_failurePeriod.invalidate();
        m_restartWhenFinished = m_process.state() != QProcess::NotRunning;
        if (m_process.state() != QProcess::NotRunning) m_process.kill();
    }
}
