#include "ServerHost.h"
#include "AppLog.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QSocketNotifier>
#include <QDebug>
#include <sys/signalfd.h>
#include <unistd.h>
#include <signal.h>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("ZTOrbitHub"); app.setApplicationVersion(ZTO_VERSION);
    QCommandLineParser parser;
    parser.setApplicationDescription("ZeroTier Linux plugin server (root, Ubuntu/Debian + systemd)");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({{{"i","install"},"Install ZeroTier/ztncui (optional target: zerotier, ztncui, all)"},
        {{"u","uninstall"},"Uninstall ZeroTier/ztncui (optional target)"}, {{"m","moon"},"Build/export Moon"},
        {{"s","server"},"Run server in foreground"}, {{"p","port"},"TCP/UDP port","port","18100"},
        {"listen","Listen IPv4 address","address","0.0.0.0"}, {"ip","Moon public IPv4 address","address"},
        {"force","Rebuild existing Moon/container"}, {"yes","Confirm destructive operations"},
        {"dry-run","Validate and print installer arguments without making changes"}});
    parser.addPositionalArgument("target", "zerotier, ztncui or all; omitted for selection menu", "[target]");
    QStringList arguments = app.arguments();
    for (int i = 1; i < arguments.size(); ++i) {
        if (arguments[i].startsWith("--install=") || arguments[i].startsWith("--uninstall=")) {
            const auto pieces = arguments[i].split('=');
            if (pieces.size() != 2 || pieces[1].isEmpty()) { std::cerr << "Invalid target\n"; return 2; }
            arguments[i] = pieces[0]; arguments.insert(++i, pieces[1]);
        }
    }
    if (!parser.parse(arguments)) { std::cerr << parser.errorText().toStdString() << '\n'; return 2; }
    if (parser.isSet("help")) parser.showHelp();
    if (parser.isSet("version")) parser.showVersion();
    const int modes = int(parser.isSet("install")) + int(parser.isSet("uninstall")) + int(parser.isSet("moon")) + int(parser.isSet("server"));
    if (modes == 0 && arguments.size() == 1) parser.showHelp();
    if (modes != 1) { std::cerr << "Choose exactly one operation\n"; return 2; }
    const auto targets = parser.positionalArguments();
    if (targets.size() > 1 || ((!parser.isSet("install") && !parser.isSet("uninstall")) && !targets.isEmpty())) {
        std::cerr << "Unexpected target\n"; return 2;
    }
    bool validPort = false; const int port = parser.value("port").toInt(&validPort);
    const QHostAddress address(parser.value("listen"));
    if (!validPort || port < 1 || port > 65535 || address.protocol() != QAbstractSocket::IPv4Protocol) { std::cerr << "Invalid port/address\n"; return 2; }
    if (parser.isSet("ip") && QHostAddress(parser.value("ip")).protocol() != QAbstractSocket::IPv4Protocol) { std::cerr << "Invalid public IPv4\n"; return 2; }
    if (parser.isSet("server") && (parser.isSet("ip") || parser.isSet("force") || parser.isSet("yes") || parser.isSet("dry-run"))) {
        std::cerr << "Installer options cannot be used with --server\n"; return 2;
    }
    if (!parser.isSet("server") && (parser.isSet("port") || parser.isSet("listen"))) { std::cerr << "Listen/port require --server\n"; return 2; }
    if (geteuid() != 0) { std::cerr << "Run as root\n"; return 1; }
    if (parser.isSet("server")) {
        sigset_t stopSignals; sigemptyset(&stopSignals); sigaddset(&stopSignals, SIGTERM); sigaddset(&stopSignals, SIGINT);
        if (sigprocmask(SIG_BLOCK, &stopSignals, nullptr) != 0) return 1;
        const int fd = signalfd(-1, &stopSignals, SFD_NONBLOCK | SFD_CLOEXEC);
        if (fd < 0) return 1;
        QSocketNotifier notifier(fd, QSocketNotifier::Read);
        QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
            signalfd_siginfo info; if (::read(fd, &info, sizeof(info)) == sizeof(info)) app.quit();
        });
        vusiif::zto::util::log::initialize();
        vusiif::ztohub::ServerHost host(QDir::currentPath());
        if (!host.start(address, quint16(port))) { ::close(fd); return 1; }
        const int result = app.exec(); host.stop(); notifier.setEnabled(false); ::close(fd); return result;
    }
    QString target = targets.value(0);
    if (target.isEmpty() && !parser.isSet("moon")) {
        if (!isatty(STDIN_FILENO)) { std::cerr << "Specify target for non-interactive use\n"; return 2; }
        std::cout << "1) ZeroTier\n2) ztncui\n3) All\n0) Cancel\nSelect: " << std::flush;
        std::string input; std::getline(std::cin, input);
        if (input == "0") return 0;
        target = input == "1" ? "zerotier" : input == "2" ? "ztncui" : input == "3" ? "all" : "";
    }
    int action = 4;
    if (!parser.isSet("moon")) {
        if (target != "zerotier" && target != "ztncui" && target != "all") { std::cerr << "Invalid target\n"; return 2; }
        if (parser.isSet("install")) action = target == "all" ? 1 : target == "zerotier" ? 2 : 3;
        else action = target == "all" ? 6 : target == "ztncui" ? 5 : 7;
    }
    if (parser.isSet("uninstall") && !parser.isSet("yes") && !parser.isSet("dry-run")) {
        if (!isatty(STDIN_FILENO)) { std::cerr << "Uninstall requires --yes in non-interactive use\n"; return 2; }
        std::cout << "Delete " << target.toStdString() << " and its data? ZeroTier identity deletion changes the node ID. Type yes: " << std::flush;
        std::string input; std::getline(std::cin, input); if (input != "yes") return 0;
    }
    QStringList scriptArguments{"--action",QString::number(action)};
    if (parser.isSet("ip")) scriptArguments << "--ip" << parser.value("ip");
    if (parser.isSet("force")) scriptArguments << "--force";
    if (parser.isSet("dry-run")) { std::cout << "bash <embedded-installer> " << scriptArguments.join(' ').toStdString() << '\n'; return 0; }
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    const auto script = temporary.filePath("zt-installer.sh");
    if (!QFile::copy(":/installer/zt-installer.sh",script)) return 1;
    QProcess process; process.setProcessChannelMode(QProcess::ForwardedChannels); process.setInputChannelMode(QProcess::ForwardedInputChannel);
    process.start("/bin/bash",QStringList{script} + scriptArguments);
    if (!process.waitForStarted()) return 1;
    process.waitForFinished(-1);
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 1;
}
