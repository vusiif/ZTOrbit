#include "PluginImport.h"
#include "ClientNetworkService.h"
#include "NetworkTransport.h"
#include <ztprotocol/NetworkDelivery.h>
#include <QNetworkDatagram>
#include <future>
#include "ZeroTierClient.h"
#include "AppLog.h"
#include "NetworkBundle.h"
#include "MacPlatform.h"
#include "plugins/PluginRegistry.h"
#include "plugins/HostServicesImpl.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QUrl>
#include <QtTest>
#include <stdexcept>
#include <thread>
#include <QTcpServer>
#include <QTcpSocket>
#include <ztprotocol/WireProtocol.h>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "SystemOperations.h"
using namespace vusiif::ztgui::api;
using namespace vusiif::ztgui::util;
using namespace vusiif::ztgui::plugins;
using namespace vusiif::ztgui::ui;
static QString serviceMocks()
{
#ifdef Q_OS_MACOS
    return "launchctl() { if [ \"$1\" = bootout ]; then return 1; fi; "
        "if [ \"$1\" = kickstart ] && [ \"${ZT_TEST_START_FAILURE:-0}\" = 1 ]; then return 1; fi; return 0; }\n";
#else
    return "function Stop-Service { throw 'stop error to ignore' }\n"
        "function netsh { $global:LASTEXITCODE = 0 }\n"
        "function Get-Content { [CmdletBinding()] param([string]$LiteralPath,[switch]$Raw); "
        "if ($LiteralPath -like '*zerotier-one.port') { return '9993' }; "
        "if ($LiteralPath -like '*authtoken.secret') { return 'mock-token' }; "
        "Microsoft.PowerShell.Management\\Get-Content @PSBoundParameters }\n"
        "function Invoke-RestMethod { return @{address='0123456789'} }\n"
        "function Start-Service { if ($env:ZT_TEST_START_FAILURE -eq '1') { throw 'start failed' }; Write-Output 'start executed' }\n"
        "function Get-Service { $s = New-Object PSObject; Add-Member -InputObject $s -MemberType ScriptMethod -Name WaitForStatus -Value {}; return $s }\n";
#endif
}

class ScriptWindow : public SystemOperations {
public:
    QString lastScript;
    bool succeeded = false;
    int operations = 0;
    void runElevatedScript(const QString &, const QString &script, std::function<void(bool)> finished) override
    {
        lastScript = script;
        ++operations;
#ifdef Q_OS_MACOS
        QTemporaryDir directory;
        QFile file(directory.filePath("mock-operation.sh"));
        if (!file.open(QIODevice::WriteOnly)) { if (finished) finished(false); return; }
        file.write((serviceMocks() + script).toUtf8());
        file.close();
        QProcess process;
        process.start("/bin/sh", {file.fileName()});
#else
        const auto invocation = vusiif::ztgui::zerotier::preparePowerShell(serviceMocks() + script);
        QProcess process;
        process.start("powershell.exe", invocation.arguments);
#endif
        succeeded = process.waitForFinished(15000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
#ifdef Q_OS_WIN
        if (!succeeded) {
            QFile output(invocation.logPath);
            if (output.open(QIODevice::ReadOnly)) qWarning().noquote() << "Mock service script failed:" << output.readAll();
        }
#endif
        if (finished) finished(succeeded);
    }
};

class CoreTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings;
    QByteArray m_oldPath, m_oldProgramData;
private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName("ZeroTierGUI-Tests");
        QCoreApplication::setApplicationName("RegressionTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        m_oldPath = qgetenv("PATH");
        m_oldProgramData = qgetenv("ProgramData");
        qputenv("ProgramData", m_settings.path().toLocal8Bit());
#ifndef Q_OS_WIN
        qputenv("ZT_TEST_DATA_DIRECTORY", (m_settings.path() + "/ZeroTier").toLocal8Bit());
#endif
        qputenv("PATH", QCoreApplication::applicationDirPath().toLocal8Bit() + QDir::listSeparator().toLatin1() + m_oldPath);
        QVERIFY(vusiif::ztgui::zerotier::executable().startsWith(QCoreApplication::applicationDirPath()));
    }
    void cleanup() {
        qunsetenv("ZT_TEST_MODE");
        qunsetenv("ZT_TEST_WINGET_CODE");
        qunsetenv("ZT_TEST_START_FAILURE");
        qunsetenv("ZT_TEST_COMMAND_LOG");
        qunsetenv("ZT_TEST_QUERY_LOG");
        QSettings().clear();
    }
    void cleanupTestCase() {
        qputenv("PATH", m_oldPath); qputenv("ProgramData", m_oldProgramData);
        qunsetenv("ZT_TEST_DATA_DIRECTORY");
    }
    void pluginNetworkPollingFollowsRequestsAndSubscriptions() {
        QTemporaryDir directory; const auto path=directory.filePath("queries.log");
        qputenv("ZT_TEST_QUERY_LOG",path.toUtf8()); qputenv("ZT_TEST_MODE","empty");
        const auto queries=[&] { QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); };
        auto &service=vusiif::ztgui::network::ClientNetworkService::instance();
        QTest::qWait(1200); QVERIFY(queries().isEmpty());
        auto *context=new QObject; bool invoked=false;
        service.withTransport(context,[&](vusiif::ztgui::network::NetworkTransport &) { invoked=true; });
        QTRY_VERIFY(invoked); QVERIFY(queries().contains("info")); QVERIFY(queries().contains("listnetworks"));
        delete context; QTest::qWait(150); const auto idle=queries(); QTest::qWait(1300); QCOMPARE(queries(),idle);
        QObject parent;
        auto *first=service.subscribe("fixture","0123456789abcdef",&parent,[](const NetworkMessage &) {});
        auto *second=service.subscribe("fixture","0123456789abcdef",&parent,[](const NetworkMessage &) {});
        QVERIFY(first); QVERIFY(second); QTRY_VERIFY(queries().size()>idle.size());
        QTest::qWait(200); first->cancel(); const auto one=queries();
        QTRY_VERIFY(queries().size()>one.size()); // The second subscription retains background polling.
        second->cancel(); QTest::qWait(150); const auto stopped=queries(); QTest::qWait(1300); QCOMPARE(queries(),stopped);
        qputenv("ZT_TEST_MODE","slow"); bool cancelled=false; auto *pending=new QObject;
        service.withTransport(pending,[&](vusiif::ztgui::network::NetworkTransport &) { cancelled=true; });
        QTRY_VERIFY(queries().contains("slow|")); delete pending; QTest::qWait(150);
        QVERIFY(!cancelled);
        qputenv("ZT_TEST_MODE","empty"); auto *resumed=new QObject; bool ready=false;
        service.withTransport(resumed,[&](vusiif::ztgui::network::NetworkTransport &) { ready=true; });
        QTRY_VERIFY(ready); delete resumed; QTest::qWait(150);
    }
    void pluginNetworkRequestsReleaseDemandAfterCompletionAndUnload() {
        QTemporaryDir directory; const auto path=directory.filePath("queries.log");
        qputenv("ZT_TEST_QUERY_LOG",path.toUtf8()); qputenv("ZT_TEST_MODE","empty");
        const auto queries=[&] { QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); };
        HostServicesImpl host("IdleFixture",directory.path(),"idle-fixture"); QObject context;
        bool completed=false;
        host.getNetworkRelays("0123456789abcdef",&context,[&](const RelayResult &result) { QVERIFY(!result.ok()); completed=true; });
        QTRY_VERIFY(completed); QTRY_VERIFY(!host.hasCallbacks());
        QTest::qWait(150); const auto idle=queries(); QTest::qWait(1300); QCOMPARE(queries(),idle);
        auto *subscription=host.subscribeNetworkData("0123456789abcdef",&context,[](const NetworkMessage &) {}); QVERIFY(subscription);
        QTRY_VERIFY(queries().size()>idle.size()); host.stopRequests(); QTRY_VERIFY(!host.hasCallbacks());
        QTest::qWait(150); const auto unloaded=queries(); QTest::qWait(1300); QCOMPARE(queries(),unloaded);
    }
    void pluginSubscriptionReceivesDataAndClosesIdleSocket() {
        qputenv("ZT_TEST_MODE","members");
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,vusiif::ztgui::protocol::NetworkPort));
        connect(&server,&QTcpServer::newConnection,this,[&] {
            auto *socket=server.nextPendingConnection(); auto input=std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::readyRead,socket,[socket,input] {
                *input+=socket->readAll(); vusiif::ztgui::protocol::Packet packet;
                if(vusiif::ztgui::protocol::takeFrame(*input,packet)!=vusiif::ztgui::protocol::ReadResult::Packet) return;
                const auto request=QJsonDocument::fromJson(packet.payload).object();
                const QJsonArray members{QJsonObject{{"nodeId","0123456789"},{"authorized",true},{"ipAssignments",QJsonArray{"127.0.0.2"}}},
                    QJsonObject{{"nodeId","abcdef0123"},{"authorized",true},{"ipAssignments",QJsonArray{"127.0.0.3"}}}};
                const QJsonValue data=request["op"]=="controller.members"?QJsonValue(members):QJsonValue(QJsonObject{});
                socket->write(vusiif::ztgui::protocol::frame("builtin",QJsonDocument(QJsonObject{{"v",1},{"id",1},{"ok",true},{"data",data}}).toJson(QJsonDocument::Compact)));
            });
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        });
        QTemporaryDir directory; HostServicesImpl host("SubscriptionFixture",directory.path(),"idle-fixture"); QObject context;
        int received=0; auto *sub=host.subscribeNetworkData("0123456789abcdef",&context,[&](const NetworkMessage &message) { QCOMPARE(message.data,QByteArray("background")); ++received; }); QVERIFY(sub);
        bool ready=false; host.getNetworkRelays("0123456789abcdef",&context,[&](const RelayResult &result) { QVERIFY(result.ok()); ready=true; }); QTRY_VERIFY(ready);
        QUdpSocket sender; QVERIFY(sender.bind(QHostAddress("127.0.0.3"),0));
        const auto payload=vusiif::ztgui::protocol::encodeDelivery({"0123456789abcdef","abcdef0123","0123456789","idle-fixture",QByteArray(16,'a'),"background"});
        sender.writeDatagram(payload,QHostAddress("127.0.0.2"),vusiif::ztgui::protocol::ClientNetworkPort);
        QTRY_COMPARE(received,1); sub->cancel(); QTRY_VERIFY(!host.hasCallbacks()); QTest::qWait(100);
        QUdpSocket replacement;
        QTRY_VERIFY(replacement.state()==QAbstractSocket::BoundState || replacement.bind(QHostAddress("127.0.0.2"),vusiif::ztgui::protocol::ClientNetworkPort));
    }
    void pausedSlowPollerResumesWithoutReportingFailure() {
        QTemporaryDir directory; const auto path=directory.filePath("queries.log");
        qputenv("ZT_TEST_QUERY_LOG",path.toUtf8()); qputenv("ZT_TEST_MODE","slow");
        const auto queries=[&] { QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); };
        vusiif::ztgui::zerotier::JsonPoller poller(this,"info",true);
        QSignalSpy updated(&poller,&vusiif::ztgui::zerotier::JsonPoller::updated);
        QSignalSpy availability(&poller,&vusiif::ztgui::zerotier::JsonPoller::availabilityChanged);
        poller.setActive(true); QTRY_VERIFY(queries().contains("slow|"));
        poller.setActive(false); qputenv("ZT_TEST_MODE","empty"); poller.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!updated.isEmpty(),3000);
        for(const auto &event:availability) QVERIFY(event.first().toBool());
        poller.setActive(false); QTest::qWait(150); const auto idle=queries(); QTest::qWait(1200); QCOMPARE(queries(),idle);
    }
    void pluginImportIsAtomicAndDoesNotOverwrite() {
        QTemporaryDir directory;
const QString name = "example.ztplugin";
        QFile source(directory.filePath(name));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("plugin bytes"); source.close();
        const auto target = directory.filePath("plugins");
        QVERIFY(vusiif::ztgui::plugins::importPlugin(source.fileName(),target).isEmpty());
        QFile installed(QDir(target).filePath(name)); QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(),QByteArray("plugin bytes")); installed.close();
        QVERIFY(!vusiif::ztgui::plugins::importPlugin(source.fileName(),target).isEmpty());
        QVERIFY(!vusiif::ztgui::plugins::importPlugin(directory.filePath("missing.dll"),target).isEmpty());
        QFile blocker(directory.filePath("blocked")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        QVERIFY(!vusiif::ztgui::plugins::importPlugin(source.fileName(),blocker.fileName()).isEmpty());
    }

    void pluginLogsWriteBothSinksAndReportPartialFailure() {
        QTemporaryDir directory;
        vusiif::ztgui::util::HostServicesImpl host(QStringLiteral("测试插件"),directory.path(),"test-logger");
        std::vector<std::future<bool>> tasks;
        for (int i=0;i<4;++i) tasks.push_back(std::async(std::launch::async,[&host,i] {
            for (int j=0;j<20;++j) if (!host.logs().write(vusiif::ztgui::util::LogLevel::Info,QString("concurrent-%1-%2").arg(i).arg(j)).ok()) return false;
            return true;
        }));
        for (auto &task:tasks) QVERIFY(task.get());
        const QDir logs(directory.filePath(QStringLiteral("测试插件/logs")));
        QCOMPARE(logs.entryList({"*.log"},QDir::Files).size(),1);
        QFile plugin(logs.filePath(logs.entryList({"*.log"},QDir::Files).first()));
        QVERIFY(plugin.open(QIODevice::ReadOnly)); const auto entries = plugin.readAll();
        QCOMPARE(entries.count("[test-logger]"),80);
        QFile main(vusiif::ztgui::util::log::path()); QVERIFY(main.open(QIODevice::ReadOnly));
        QVERIFY(main.readAll().contains("[test-logger] concurrent-3-19"));
        vusiif::ztgui::util::HostServicesImpl invalid("../invalid",directory.path(),"test-partial");
        const auto result=invalid.logs().write(vusiif::ztgui::util::LogLevel::Warning,"partial-sink-probe");
        QVERIFY(result.hostError.isEmpty()); QVERIFY(!result.pluginError.isEmpty());
        QVERIFY(!QFileInfo::exists(directory.filePath("../invalid")));
    }

    void singleInstanceActivatesAndRecoversAfterCrash() {
        QString helper=QDir(QCoreApplication::applicationDirPath()).filePath("instance_probe");
#ifdef Q_OS_WIN
        helper += ".exe";
#endif
        QProcess first; first.start(helper,{}); QVERIFY(first.waitForReadyRead(5000));
        QCOMPARE(first.readAllStandardOutput().trimmed(),QByteArray("primary"));
        QProcess second; second.start(helper,{}); QVERIFY(second.waitForFinished(5000));
        QCOMPARE(second.exitCode(),0); QCOMPARE(second.readAllStandardOutput().trimmed(),QByteArray("secondary"));
        QVERIFY(first.waitForReadyRead(3000)); QVERIFY(first.readAllStandardOutput().contains("activated"));
        first.kill(); QVERIFY(first.waitForFinished(3000));
        QProcess recovered; recovered.start(helper,{}); QVERIFY(recovered.waitForReadyRead(5000));
        QCOMPARE(recovered.readAllStandardOutput().trimmed(),QByteArray("primary"));
        recovered.kill(); QVERIFY(recovered.waitForFinished(3000));
    }

    void logsAreReservedPerLaunchAndPreserveHistory() {
        QTemporaryDir directory;
        QTemporaryDir workingDirectory;
        QVERIFY(directory.isValid());
        QVERIFY(workingDirectory.isValid());
        QString helperName = "app_log_probe";
#ifdef Q_OS_WIN
        helperName += ".exe";
#endif
        const QString helper = directory.filePath(helperName);
        QVERIFY(QFile::copy(QDir(QCoreApplication::applicationDirPath()).filePath(helperName), helper));
        const QDir logs(directory.filePath("logs"));
        const QString date = QDateTime::currentDateTime().toString("yyMMdd");
        auto runProbe = [&] {
            QProcess process;
            process.setWorkingDirectory(workingDirectory.path());
            process.start(helper, {});
            if (!process.waitForFinished(10000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
                return QString();
            return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        };
        QCOMPARE(runProbe(), logs.filePath(date + "-1.log"));
        QFile first(logs.filePath(date + "-1.log"));
        QVERIFY(first.open(QIODevice::ReadOnly));
        const QByteArray original = first.readAll();
        first.close();
        QCOMPARE(original.count('\n'), 43);
        QCOMPARE(original.count(QStringLiteral("应用启动").toUtf8()), 1);
        QVERIFY(original.contains(QStringLiteral("日志测试：中文").toUtf8()));
        QVERIFY(original.contains("WARN captured warning"));
        QVERIFY(original.contains("ZTGUI v" ZEROTIER_GUI_VERSION));
        QCOMPARE(runProbe(), logs.filePath(date + "-2.log"));
        QVERIFY(first.open(QIODevice::ReadOnly));
        QCOMPARE(first.readAll(), original);
        first.close();
        QVERIFY(!QDir(workingDirectory.filePath("logs")).exists());

        // Gaps, unrelated filenames, and previous dates must not reuse a log.
        const QString yesterday = QDateTime::currentDateTime().addDays(-1).toString("yyMMdd");
        for (const QString &name : {date + "-9.log", date + "-11.log", date + "-999.log.bak", yesterday + "-80.log"}) {
            QFile file(logs.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("preserved");
        }
        QCOMPARE(runProbe(), logs.filePath(date + "-12.log"));

        // Simultaneous launches reserve distinct filenames without overwrites.
        QProcess processes[4];
        for (auto &process : processes) {
            process.setWorkingDirectory(workingDirectory.path());
            process.start(helper, {});
        }
        QSet<QString> paths;
        for (auto &process : processes) {
            QVERIFY(process.waitForFinished(10000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(process.exitCode(), 0);
            const QString path = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
            QVERIFY(!paths.contains(path));
            paths.insert(path);
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll().count('\n'), 43);
        }
        for (int sequence = 13; sequence <= 16; ++sequence)
            QVERIFY(paths.contains(logs.filePath(date + "-" + QString::number(sequence) + ".log")));
    }

    void pluginFilesUseDataRootAndRejectEscapes() {
        using namespace vusiif::ztgui::api;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        HostServicesImpl services("chat", directory.filePath("plugins-data"));
        const QByteArray bytes("binary\0data", 11);
        QVERIFY(services.files().writeFile("example/file1.txt", bytes).ok());
        QFile actual(directory.filePath("plugins-data/chat/example/file1.txt"));
        QVERIFY(actual.open(QIODevice::ReadOnly)); QCOMPARE(actual.readAll(), bytes); actual.close();
        QVERIFY(services.files().writeFile("example/file1.txt", "replaced").ok());
        QCOMPARE(services.files().readFile("example\\file1.txt").data, QByteArray("replaced"));
        QVERIFY(services.files().writeFile("empty.txt", {}).ok());
        QVERIFY(services.files().readFile("empty.txt").ok());
        QVERIFY(!services.files().readFile("missing.txt").ok());
        for (const QString &path : {QString("../escape"), QString("/escape"), QString("C:/escape"),
             QString("example/../../escape"), QString("file:stream"), QString("example/../escape"), QString("example./escape")}) {
            QVERIFY(!services.files().writeFile(path, "bad").ok());
            QVERIFY(!services.files().readFile(path).ok());
        }
        QVERIFY(!QFileInfo::exists(directory.filePath("escape")));
        // A regular file cannot be traversed as a directory.
        QVERIFY(!services.files().writeFile("empty.txt/child", "bad").ok());
        QVERIFY(!services.files().readFile("example").ok());
        HostServicesImpl other("file-transfer", directory.filePath("plugins-data"));
        QVERIFY(other.files().writeFile("example/file1.txt", "other plugin").ok());
        QCOMPARE(services.files().readFile("example/file1.txt").data, QByteArray("replaced"));
        QCOMPARE(other.files().readFile("example/file1.txt").data, QByteArray("other plugin"));
        QVERIFY(!services.files().writeFile("../file-transfer/example/file1.txt", "bad").ok());
        HostServicesImpl chinese(QStringLiteral("语音聊天"), directory.filePath("plugins-data"));
        QVERIFY(chinese.files().writeFile("file1.txt", "voice").ok());
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("plugins-data/语音聊天/file1.txt"))));
        for (const QString &name : {QString("../escape"), QString(".."), QString("chat/other"), QString("chat\\other"),
             QString("chat:"), QString("CON.txt"), QString("chat."), QString("chat "), QString()}) {
            HostServicesImpl invalid(name, directory.filePath("plugins-data"));
            QVERIFY(!invalid.files().writeFile("file1.txt", "bad").ok());
            QVERIFY(!invalid.files().readFile("file1.txt").ok());
        }
        // On Unix QFile::link creates a symlink (on Windows it creates a .lnk).
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(actual.fileName(), directory.filePath("plugins-data/chat/link")));
        QVERIFY(!services.files().readFile("link").ok());
        QVERIFY(!services.files().writeFile("link", "bad").ok());
        QVERIFY(QFile::link(directory.filePath("plugins-data/file-transfer"), directory.filePath("plugins-data/linked-plugin")));
        HostServicesImpl linkedPlugin("linked-plugin", directory.filePath("plugins-data"));
        QVERIFY(!linkedPlugin.files().readFile("example/file1.txt").ok());
        QVERIFY(!linkedPlugin.files().writeFile("example/file1.txt", "bad").ok());
        QVERIFY(QFile::link(directory.filePath("plugins-data"), directory.filePath("linked-data")));
        HostServicesImpl linkedRoot("chat", directory.filePath("linked-data"));
        QVERIFY(!linkedRoot.files().readFile("example/file1.txt").ok());
        QVERIFY(!linkedRoot.files().writeFile("example/file1.txt", "bad").ok());
#endif
    }

    void pluginZeroTierQueriesPreserveJsonAndCancelCallbacks() {
        using namespace vusiif::ztgui::api;
        HostServicesImpl services("query-test", m_settings.filePath("plugin-tools"));
        QObject context;
        int calls = 0;
        JsonResult info, peers, networks, moons;
        services.zeroTier().getZTInfo(&context, [&](const JsonResult &result) { info = result; ++calls; });
        services.zeroTier().getZTPeers(&context, [&](const JsonResult &result) { peers = result; ++calls; });
        services.zeroTier().getZTNetworks(&context, [&](const JsonResult &result) { networks = result; ++calls; });
        services.zeroTier().getZTMoons(&context, [&](const JsonResult &result) { moons = result; ++calls; });
        QCOMPARE(calls, 0);
        QTRY_COMPARE(calls, 4);
        QVERIFY(info.ok()); QCOMPARE(info.document.object().value("extraField").toString(), QString("preserved"));
        QVERIFY(peers.ok()); QCOMPARE(peers.document.array().size(), 35);
        QVERIFY(networks.ok()); QCOMPARE(networks.document.array().size(), 40);
        QVERIFY(moons.ok()); QCOMPARE(moons.document.array().first().toObject().value("id").toString(), QString("0000000123456789"));
        for (const char *mode : {"malformed", "error"}) {
            qputenv("ZT_TEST_MODE", mode);
            calls = 0;
            services.zeroTier().getZTInfo(&context, [&](const JsonResult &result) { info = result; ++calls; });
            QTRY_COMPARE(calls, 1); QVERIFY(!info.ok()); QVERIFY(!info.error.isEmpty());
        }
        qunsetenv("ZT_TEST_MODE");
        calls = 0;
        auto canceled = std::make_unique<QObject>();
        services.zeroTier().getZTInfo(canceled.get(), [&](const JsonResult &) { ++calls; });
        canceled.reset();
        QTest::qWait(100); QCOMPARE(calls, 0);
        auto selfDeleting = std::make_unique<QObject>();
        services.zeroTier().getZTInfo(selfDeleting.get(), [&](const JsonResult &result) {
            QVERIFY(result.ok()); selfDeleting.reset(); ++calls;
        });
        QTRY_COMPARE(calls, 1);
        calls = 0;
        services.zeroTier().getZTInfo(&context, [&](const JsonResult &) { ++calls; });
        services.stopRequests();
        QTest::qWait(100); QCOMPARE(calls, 0);
    }

    void macScriptQuotingAndSyntax() {
#ifdef Q_OS_WIN
        const QString shell = "C:/Program Files/Git/bin/bash.exe";
#else
        const QString shell = "/bin/bash";
#endif
        if (!QFileInfo::exists(shell)) QSKIP("A POSIX shell is needed for script syntax checks.");
        const QString value = QStringLiteral("name with 'quotes' and $(never-run)\\backslash\nnext line");
        QProcess quoting;
        quoting.start(shell, {"-c", "printf '%s' " + vusiif::ztgui::platform::quote(value)});
        QVERIFY(quoting.waitForFinished(5000));
        QCOMPARE(quoting.exitCode(), 0);
        QCOMPARE(QString::fromUtf8(quoting.readAllStandardOutput()), value);
        const QString directory = "/Library/Application Support/ZeroTier/One/moons.d";
        const QString target = directory + "/000000abcdef1234.moon";
        const QStringList scripts = {
            vusiif::ztgui::platform::serviceCommands("start", "/usr/local/bin/zerotier-cli"),
            vusiif::ztgui::platform::serviceCommands("stop", "/usr/local/bin/zerotier-cli"),
            vusiif::ztgui::platform::serviceCommands("restart", "/usr/local/bin/zerotier-cli"),
            vusiif::ztgui::platform::packageScript(false, "/Library/Application Support/ZeroTier"),
            vusiif::ztgui::platform::packageScript(true, "/Library/Application Support/ZeroTier"),
            vusiif::ztgui::platform::copyMoons({{"/tmp/file's.moon", target}}, directory),
            vusiif::ztgui::platform::deleteMoon(target, directory),
            vusiif::ztgui::platform::synchronizeSession("/Library/Application Support/ZeroTier/One", "/tmp/ztgui-session-test", 501)
        };
        QTemporaryDir temporary;
        for (const auto &script : scripts) {
            QFile file(temporary.filePath("operation.sh"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(script.toUtf8()); file.close();
            QProcess syntax;
            syntax.start(shell, {"-n", file.fileName()});
            QVERIFY(syntax.waitForFinished(5000));
            QCOMPARE(syntax.exitCode(), 0);
        }
        QCOMPARE(vusiif::ztgui::platform::deleteMoon("/tmp/outside.moon", directory), QString("exit 1\n"));
        QVERIFY(!vusiif::ztgui::platform::synchronizeSession("/source", "/cache", 501).contains("identity.secret"));
        QCOMPARE(vusiif::ztgui::platform::appleScriptString("a\"b\\c"), QString("\"a\\\"b\\\\c\""));
    }

    void serviceStopErrorsDoNotBlockRestart() {
        ScriptWindow window;
        bool result = false;
        window.runService("restart", [&](bool ok) { result = ok; });
        QVERIFY(result);
        QVERIFY(window.succeeded);
        window.runService("stop", [&](bool ok) { result = ok; });
        QVERIFY(result);
        qputenv("ZT_TEST_START_FAILURE", "1");
        window.runService("restart", [&](bool ok) { result = ok; });
        QVERIFY(!result); // Starting errors must still propagate.
    }

    void bundleFormatRejectsInvalidContent() {
        const vusiif::ztgui::zerotier::bundle::Configuration original {"ABCDEF1234000001", "000000ABCDEF1234.moon", QByteArray("signed\0moon", 11)};
        const QByteArray encoded = vusiif::ztgui::zerotier::bundle::encode(original);
        QVERIFY(!encoded.isEmpty());
        vusiif::ztgui::zerotier::bundle::Configuration decoded;
        QString error;
        QVERIFY(vusiif::ztgui::zerotier::bundle::decode(encoded, decoded, error));
        QCOMPARE(decoded.networkId, original.networkId.toLower());
        QCOMPARE(decoded.moonFileName, original.moonFileName);
        QCOMPARE(decoded.moonContents, original.moonContents);
        const auto originalObject = QJsonDocument::fromJson(encoded).object();
        for (const QString &scenario : {QString("version"), QString("id"), QString("path"), QString("mismatch"), QString("base64"), QString("empty")}) {
            auto object = originalObject;
            auto moon = object["moon"].toObject();
            if (scenario == "version") object.insert("version", 2);
            if (scenario == "id") object.insert("networkId", "bad");
            if (scenario == "path") moon.insert("fileName", "../000000abcdef1234.moon");
            if (scenario == "mismatch") moon.insert("fileName", "0000009999999999.moon");
            if (scenario == "base64") moon.insert("contentBase64", "!!!");
            if (scenario == "empty") moon.insert("contentBase64", "");
            object.insert("moon", moon);
            QVERIFY(!vusiif::ztgui::zerotier::bundle::decode(QJsonDocument(object).toJson(), decoded, error));
            QVERIFY(!error.isEmpty());
            QVERIFY(decoded.networkId.isEmpty());
        }
        QVERIFY(!vusiif::ztgui::zerotier::bundle::decode("not json", decoded, error));
        QVERIFY(!vusiif::ztgui::zerotier::bundle::decode(QByteArray(vusiif::ztgui::zerotier::bundle::MaxFileSize + 1, 'a'), decoded, error));
    }

    void uninstallOnlyCleansAfterSuccess() {
#ifdef Q_OS_WIN
        // Both the script and deletion operate only inside this verified test directory.
        QTemporaryDir sandbox(QDir::currentPath() + "/cleanup-XXXXXX");
        QVERIFY(sandbox.isValid());
        QVERIFY(QFileInfo(sandbox.path()).canonicalFilePath().startsWith(QDir::currentPath() + '/'));
        const auto target = QDir(sandbox.path()).filePath("ZeroTier");
        QVERIFY(QDir().mkpath(target + "/One"));
        QFile identity(target + "/One/identity.secret");
        QVERIFY(identity.open(QIODevice::WriteOnly));
        identity.write("test identity");
        identity.close();
        const auto original = qgetenv("ProgramData");
        qputenv("ProgramData", sandbox.path().toLocal8Bit());
        const QString script = vusiif::ztgui::zerotier::packageScript(true, vusiif::ztgui::zerotier::executable());
        qputenv("ProgramData", original);
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("ProgramData", sandbox.path());
        environment.insert("ZT_TEST_WINGET_CODE", "23");
        process.setProcessEnvironment(environment);
        const auto encoded = QByteArray(reinterpret_cast<const char *>(script.utf16()), script.size() * 2).toBase64();
        const QStringList args = {"-NoProfile", "-NonInteractive", "-EncodedCommand", QString::fromLatin1(encoded)};
        process.start("powershell.exe", args);
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitCode(), 23);
        QVERIFY(QFile::exists(identity.fileName()));
        environment.insert("ZT_TEST_WINGET_CODE", "0");
        process.setProcessEnvironment(environment);
        process.start("powershell.exe", args);
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(!QFileInfo::exists(target));
#else
        QSKIP("Windows package cleanup uses PowerShell.");
#endif
    }

    void powerShellRedirectionDoesNotBlockPayload() {
#ifdef Q_OS_WIN
        // Reproduce the old bug using an actual PowerShell process, without UAC
        // or any system service changes. close() keeps the temporary file locked.
        QTemporaryFile locked;
        QVERIFY(locked.open());
        const QString lockedPath = locked.fileName();
        locked.close();
        const QString brokenScript = "& { Write-Output 'payload'; exit 0 } *> "
                                      + vusiif::ztgui::zerotier::quotePowerShell(lockedPath);
        const auto brokenEncoded = QByteArray(reinterpret_cast<const char *>(brokenScript.utf16()),
                                              brokenScript.size() * 2).toBase64();
        QProcess broken;
        broken.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-EncodedCommand",
                                        QString::fromLatin1(brokenEncoded)});
        QVERIFY(broken.waitForFinished(10000));
        QCOMPARE(broken.exitCode(), 1);

        // Exercise the same arguments and log lifetime as runElevatedScript.
        QTemporaryDir payloadDirectory;
        QVERIFY(payloadDirectory.isValid());
        const QString actualMarker = payloadDirectory.filePath("payload-ran.txt");
        auto invocation = vusiif::ztgui::zerotier::preparePowerShell(
            "Set-Content -LiteralPath " + vusiif::ztgui::zerotier::quotePowerShell(actualMarker)
            + " -Value 'executed'\nWrite-Output '命令实际执行成功'\nexit 0");
        QVERIFY(invocation.directory->isValid());
        QVERIFY(!QFileInfo::exists(invocation.logPath));
        QProcess process;
        process.start("powershell.exe", invocation.arguments);
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(QFileInfo::exists(actualMarker));
        QFile log(invocation.logPath);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QTextStream output(&log);
        QVERIFY(output.readAll().contains(QStringLiteral("命令实际执行成功")));
        log.close();

        auto failure = vusiif::ztgui::zerotier::preparePowerShell(QStringLiteral("Write-Output '实际失败原因'; exit 23"));
        process.start("powershell.exe", failure.arguments);
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 23);
        QFile errorLog(failure.logPath);
        QVERIFY(errorLog.open(QIODevice::ReadOnly));
        QTextStream errorOutput(&errorLog);
        QVERIFY(errorOutput.readAll().contains(QStringLiteral("实际失败原因")));
#else
        QSKIP("PowerShell redirection is a Windows-specific check.");
#endif
    }
    void pluginRegistrationIsAtomicAndVersioned() {
        using namespace vusiif::ztgui::api;
        QCOMPARE(ApiMajor, 0);
        QCOMPARE(ApiMinor, 5);
        QCOMPARE(QString::fromLatin1(ApiVersion), QStringLiteral(ZTGUI_API_VERSION));
        PluginRegistry registry;
        QSignalSpy changes(&registry, &PluginRegistry::menusChanged);
        MenuContribution menu;
        menu.id = "overview";
        menu.title = QStringLiteral("概览");
        menu.createPage = [](QQmlEngine *, QQuickItem *parent) { return new QQuickItem(parent); };
        PluginDefinition plugin {{"example-alpha", "Alpha", "1.0"}, {menu}};
        auto invalid = plugin;
        invalid.menus.append(menu);
        QCOMPARE(registry.registerPlugin(invalid).error, Error::InvalidMenu);
        QVERIFY(registry.menus().isEmpty());
        QCOMPARE(changes.count(), 0);
        invalid = plugin;
        invalid.info.apiMajor = ApiMajor + 1;
        QCOMPARE(registry.registerPlugin(invalid).error, Error::IncompatibleApi);
        invalid = plugin;
        invalid.info.apiMinor = ApiMinor + 1;
        QCOMPARE(registry.registerPlugin(invalid).error, Error::IncompatibleApi);
        invalid = plugin;
        invalid.info.apiMinor = ApiMinor - 1;
        QCOMPARE(registry.registerPlugin(invalid).error, Error::IncompatibleApi);
        invalid = plugin;
        invalid.info.id = "builtin";
        QCOMPARE(registry.registerPlugin(invalid).error, Error::InvalidPlugin);
        invalid = plugin;
        invalid.menus[0].id = "../zerotier";
        QCOMPARE(registry.registerPlugin(invalid).error, Error::InvalidMenu);
        invalid = plugin;
        invalid.info.id = "example-alpha\n";
        QCOMPARE(registry.registerPlugin(invalid).error, Error::InvalidPlugin);
        invalid = plugin;
        invalid.menus[0].createPage = {};
        QCOMPARE(registry.registerPlugin(invalid).error, Error::InvalidMenu);
        QVERIFY(registry.registerPlugin(plugin).ok());
        QCOMPARE(registry.registerPlugin(plugin).error, Error::DuplicatePlugin);
        QCOMPARE(registry.plugins().size(), 1);
        QCOMPARE(registry.menus().first().id, QString("example-alpha/overview"));
        QCOMPARE(changes.count(), 1);
        plugin.info.id = "example-beta"; plugin.info.name="Beta";
        plugin.menus[0].order = 10;
        QVERIFY(registry.registerPlugin(plugin).ok());
        QCOMPARE(registry.menus().first().id, QString("example-beta/overview"));
        QCOMPARE(registry.unregisterPlugin("missing").error, Error::NotFound);
        QCOMPARE(registry.unregisterPlugin("builtin").error, Error::NotFound);
        QCOMPARE(registry.menus().size(), 2);
    }
    void pluginPagesDieBeforeFactoryResources() {
        using namespace vusiif::ztgui::api;
        QQmlEngine engine; QQuickItem host;
        QPointer<QQuickItem> page;
        bool pageWasDestroyedFirst = false;
        const auto addPlugin = [&](PluginRegistry &registry) {
            auto resource = std::shared_ptr<int>(new int(1), [&](int *value) {
                pageWasDestroyedFirst = page.isNull();
                delete value;
            });
            MenuContribution menu;
            menu.id = "overview";
            menu.title = "Lifetime";
            menu.createPage = [resource](QQmlEngine *, QQuickItem *parent) { return new QQuickItem(parent); };
            QVERIFY(registry.registerPlugin({{"example-lifetime", "Lifetime", "1.0"}, {menu}}).ok());
            page = registry.page("example-lifetime/overview", &engine, &host).page;
            QVERIFY(page);
        };
        {
            PluginRegistry registry;
            addPlugin(registry);
            QVERIFY(!pageWasDestroyedFirst);
            QVERIFY(registry.unregisterPlugin("example-lifetime").ok());
            QVERIFY(pageWasDestroyedFirst);
        }
        pageWasDestroyedFirst = false;
        {
            PluginRegistry registry;
            addPlugin(registry);
            QVERIFY(!pageWasDestroyedFirst);
        }
        QVERIFY(pageWasDestroyedFirst);
    }
    void pluginMutationRejectsWorkerThreads() {
        using namespace vusiif::ztgui::api;
        PluginRegistry registry;
        Result registration;
        Result removal;
        PageResult creation;
        std::thread worker([&] {
            registration = registry.registerPlugin({});
            removal = registry.unregisterPlugin("missing");
            creation = registry.page("missing/page", nullptr, nullptr);
        });
        worker.join();
        QCOMPARE(registration.error, Error::WrongThread);
        QCOMPARE(removal.error, Error::WrongThread);
        QCOMPARE(creation.result.error, Error::WrongThread);
        QVERIFY(registry.plugins().isEmpty());
    }
};
QTEST_MAIN(CoreTests)
#include "CoreTests.moc"
