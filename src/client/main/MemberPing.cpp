#include "MemberPing.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace vusiif::ztgui::zerotier {
namespace {
class PingProcess final : public QProcess {
public:
    using QProcess::QProcess;
    ~PingProcess() override {
        disconnect();
        if (state() != NotRunning) { kill(); waitForFinished(1000); }
    }
};
}
QString parsePingReply(const QByteArray &output)
{
    // Match an echo reply, never the summary or an ICMP destination-unreachable message.
    // Only ASCII tokens are needed, including with Chinese Windows ping output.
    const auto lines = QString::fromLocal8Bit(output).split('\n');
    static const QRegularExpression ttl("\\bttl[= ]", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression time("([=<])\\s*(\\d+(?:[.,]\\d+)?)\\s*ms\\b", QRegularExpression::CaseInsensitiveOption);
    for (const auto &line : lines) {
        if (!ttl.match(line).hasMatch()) continue;
        const auto match = time.match(line);
        if (match.hasMatch()) return (match.captured(1) == "<" ? "<" : "") + match.captured(2).replace(',', '.') + " ms";
    }
    return {};
}
void pingMember(QObject *context, const QHostAddress &address, std::function<void(PingResult)> completed,
                const QString &program)
{
    auto *process = new PingProcess(context);
    auto *timeout = new QTimer(process); timeout->setSingleShot(true);
    auto finished = std::make_shared<bool>(false);
    auto done = [process, timeout, finished, completed = std::move(completed)](PingResult result) {
        if (*finished) return;
        *finished = true; timeout->stop();
        if (process->state() != QProcess::NotRunning) process->kill();
        process->deleteLater();
        if (completed) completed(std::move(result));
    };
    QObject::connect(timeout, &QTimer::timeout, process, [done] {
        done({QStringLiteral("不可达"), QStringLiteral("ICMP ping 超时（4 秒）"), PingState::Unreachable});
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            done({QStringLiteral("检测失败"), QStringLiteral("无法启动系统 ping 命令"), PingState::ToolFailure});
    });
    QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process,
        [process, done](int code, QProcess::ExitStatus status) {
            const auto latency = parsePingReply(process->readAllStandardOutput());
            if (status == QProcess::NormalExit && code == 0 && !latency.isEmpty())
                done({latency, QStringLiteral("虚拟 IP ICMP ping RTT"), PingState::Success});
            else done({QStringLiteral("不可达"), QStringLiteral("ICMP ping 未收到有效回显应答（超时或目标不可达）"), PingState::Unreachable});
        });
    QTimer::singleShot(0, process, [process, timeout, address, program, done] {
        if (address.protocol() != QAbstractSocket::IPv4Protocol) {
            done({QStringLiteral("—"), QStringLiteral("成员没有可探测的虚拟 IPv4 地址"), PingState::NoAddress}); return;
        }
        const auto executable = program.isEmpty() ? QStandardPaths::findExecutable("ping") : program;
        if (executable.isEmpty()) { done({QStringLiteral("检测失败"), QStringLiteral("未找到系统 ping 命令"), PingState::ToolFailure}); return; }
#ifdef Q_OS_WIN
        const QStringList arguments{"-n", "1", "-w", "4000", address.toString()};
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#elif defined(Q_OS_MACOS)
        const QStringList arguments{"-n", "-c", "1", "-W", "4000", address.toString()};
#else
        const QStringList arguments{"-n", "-c", "1", "-W", "4", address.toString()};
#endif
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("LC_ALL", "C"); process->setProcessEnvironment(environment);
        process->start(executable, arguments);
        timeout->start(4000);
    });
}
}
