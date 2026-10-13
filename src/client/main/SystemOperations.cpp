#include "SystemOperations.h"
#include "ZeroTierClient.h"
#include "MacPlatform.h"
#include "AppLog.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QProcess>
#include <QTimer>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif
namespace vusiif::ztgui::ui {
void SystemOperations::runService(const QString &action, std::function<void(bool)> finished)
{
    const QString operation = vusiif::ztgui::zerotier::serviceCommands(action);
    if (operation.isEmpty()) { if (finished) finished(false); return; }
    const QString script = vusiif::ztgui::zerotier::wrapScript(operation);
    runElevatedScript(QStringLiteral("ZeroTier 服务：%1").arg(action), script, finished);
}

void SystemOperations::runElevatedScript(const QString &label, const QString &script,
                                   std::function<void(bool)> finished)
{
#ifdef Q_OS_WIN
    const auto invocation = vusiif::ztgui::zerotier::preparePowerShell(script);
    if (!invocation.directory->isValid()) {
        report(QStringLiteral("无法创建操作日志\n"));
        if (finished) finished(false);
        return;
    }
    const QString parameters = invocation.arguments.join(' ');
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = L"powershell.exe";
    info.lpParameters = reinterpret_cast<LPCWSTR>(parameters.utf16());
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        report(label + QStringLiteral("：未执行，提权被取消或启动失败\n"));
        if (finished) finished(false);
        return;
    }
    report(label + QStringLiteral("：执行中…\n"));
    auto handle = std::shared_ptr<void>(info.hProcess, [](void *value) { CloseHandle(value); });
    auto *timer = new QTimer(this);
    timer->setInterval(200);
    connect(timer, &QTimer::timeout, this, [this, timer, handle, label, invocation, finished] {
        if (WaitForSingleObject(handle.get(), 0) != WAIT_OBJECT_0) return;
        timer->stop();
        DWORD code = 1;
        GetExitCodeProcess(handle.get(), &code);
        QFile output(invocation.logPath);
        if (output.open(QIODevice::ReadOnly)) {
            // Detect UTF-8 and UTF-16 logs from supported PowerShell versions.
            QTextStream stream(&output);
            const QString text = stream.readAll().trimmed();
            if (!text.isEmpty()) report(text + '\n');
        }
        report(label + (code == 0 ? QStringLiteral("：完成\n") : QStringLiteral("：失败（退出码 %1）\n").arg(code)));
        if (finished) finished(code == 0);
        timer->deleteLater();
    });
    timer->start();
#elif defined(Q_OS_MACOS)
    auto directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/ztgui-operation-XXXXXX");
    if (!directory->isValid()) { if (finished) finished(false); return; }
    const QString path = directory->filePath("operation.sh");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { if (finished) finished(false); return; }
    // Preserve the operation result while refreshing the private session after partial failures.
    const QString payload = "/bin/sh -c " + vusiif::ztgui::platform::quote(script) + "\nstatus=$?\n"
        "/bin/sh -c " + vusiif::ztgui::platform::quote("set -eu\n" + vusiif::ztgui::zerotier::sessionRefreshScript())
        + "\nsession_status=$?\n[ \"$status\" -ne 0 ] || status=$session_status\n"
        "exit \"$status\"\n";
    const QByteArray bytes = payload.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.flush()) { if (finished) finished(false); return; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.close();
    auto *process = new QProcess(this);
    const QString command = "/bin/sh " + vusiif::ztgui::platform::quote(path) + " 2>&1";
    const QString appleScript = "do shell script " + vusiif::ztgui::platform::appleScriptString(command) + " with administrator privileges";
    auto completed = std::make_shared<bool>(false);
    const auto complete = [this, process, directory, finished, completed, label](bool ok, const QString &output) {
        if (*completed) return;
        *completed = true;
        if (!output.trimmed().isEmpty()) report(output.trimmed());
        report(label + (ok ? QStringLiteral("：完成") : QStringLiteral("：失败或授权被取消")));
        if (finished) finished(ok);
        process->deleteLater();
    };
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [process, complete](int code, QProcess::ExitStatus status) {
            complete(code == 0 && status == QProcess::NormalExit,
                QString::fromUtf8(process->readAllStandardOutput() + process->readAllStandardError()));
        });
    connect(process, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete(false, QStringLiteral("无法启动 macOS 授权程序。"));
    });
    report(label + QStringLiteral("：等待系统授权"));
    process->start("/usr/bin/osascript", {"-e", appleScript});
#else
    Q_UNUSED(script)
    report(label + QStringLiteral("：此操作仅支持 Windows\n"));
    if (finished) finished(false);
#endif
}

void SystemOperations::report(const QString &text) {
    util::log::write(text);
    emit output(text);
}
}
