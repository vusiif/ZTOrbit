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
#include "storage/StorageContext.h"
#include "storage/StorageMaintenance.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QUrl>
#include <QtTest>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <QTcpServer>
#include <QTcpSocket>
#include <ztprotocol/WireProtocol.h>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "SystemOperations.h"
using namespace vusiif::zto::api;
using namespace vusiif::zto::util;
using namespace vusiif::zto::plugins;
using namespace vusiif::zto::storage;
using namespace vusiif::zto::ui;
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
        const auto invocation = vusiif::zto::zerotier::preparePowerShell(serviceMocks() + script);
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
        QCoreApplication::setOrganizationName("ZTOrbit-Tests");
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
        QVERIFY(vusiif::zto::zerotier::executable().startsWith(QCoreApplication::applicationDirPath()));
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
        auto &service=vusiif::zto::network::ClientNetworkService::instance();
        QTest::qWait(1200); QVERIFY(queries().isEmpty());
        auto *context=new QObject; bool invoked=false;
        service.withTransport(context,[&](vusiif::zto::network::NetworkTransport &) { invoked=true; });
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
        service.withTransport(pending,[&](vusiif::zto::network::NetworkTransport &) { cancelled=true; });
        QTRY_VERIFY(queries().contains("slow|")); delete pending; QTest::qWait(150);
        QVERIFY(!cancelled);
        qputenv("ZT_TEST_MODE","empty"); auto *resumed=new QObject; bool ready=false;
        service.withTransport(resumed,[&](vusiif::zto::network::NetworkTransport &) { ready=true; });
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
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,vusiif::zto::protocol::NetworkPort));
        connect(&server,&QTcpServer::newConnection,this,[&] {
            auto *socket=server.nextPendingConnection(); auto input=std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::readyRead,socket,[socket,input] {
                *input+=socket->readAll(); vusiif::zto::protocol::Packet packet;
                if(vusiif::zto::protocol::takeFrame(*input,packet)!=vusiif::zto::protocol::ReadResult::Packet) return;
                const auto request=QJsonDocument::fromJson(packet.payload).object();
                const QJsonArray members{QJsonObject{{"nodeId","0123456789"},{"authorized",true},{"ipAssignments",QJsonArray{"127.0.0.2"}}},
                    QJsonObject{{"nodeId","abcdef0123"},{"authorized",true},{"ipAssignments",QJsonArray{"127.0.0.3"}}}};
                const QJsonValue data=request["op"]=="controller.members"?QJsonValue(members):QJsonValue(QJsonObject{});
                socket->write(vusiif::zto::protocol::frame("builtin",QJsonDocument(QJsonObject{{"v",1},{"id",1},{"ok",true},{"data",data}}).toJson(QJsonDocument::Compact)));
            });
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        });
        QTemporaryDir directory; HostServicesImpl host("SubscriptionFixture",directory.path(),"idle-fixture"); QObject context;
        int received=0; auto *sub=host.subscribeNetworkData("0123456789abcdef",&context,[&](const NetworkMessage &message) { QCOMPARE(message.data,QByteArray("background")); ++received; }); QVERIFY(sub);
        bool ready=false; host.getNetworkRelays("0123456789abcdef",&context,[&](const RelayResult &result) { QVERIFY(result.ok()); ready=true; }); QTRY_VERIFY(ready);
        QUdpSocket sender; QVERIFY(sender.bind(QHostAddress("127.0.0.3"),0));
        const auto payload=vusiif::zto::protocol::encodeDelivery({"0123456789abcdef","abcdef0123","0123456789","idle-fixture",QByteArray(16,'a'),"background"});
        sender.writeDatagram(payload,QHostAddress("127.0.0.2"),vusiif::zto::protocol::ClientNetworkPort);
        QTRY_COMPARE(received,1); sub->cancel(); QTRY_VERIFY(!host.hasCallbacks()); QTest::qWait(100);
        QUdpSocket replacement;
        QTRY_VERIFY(replacement.state()==QAbstractSocket::BoundState || replacement.bind(QHostAddress("127.0.0.2"),vusiif::zto::protocol::ClientNetworkPort));
    }
    void pausedSlowPollerResumesWithoutReportingFailure() {
        QTemporaryDir directory; const auto path=directory.filePath("queries.log");
        qputenv("ZT_TEST_QUERY_LOG",path.toUtf8()); qputenv("ZT_TEST_MODE","slow");
        const auto queries=[&] { QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); };
        vusiif::zto::zerotier::JsonPoller poller(this,"info",true);
        QSignalSpy updated(&poller,&vusiif::zto::zerotier::JsonPoller::updated);
        QSignalSpy availability(&poller,&vusiif::zto::zerotier::JsonPoller::availabilityChanged);
        poller.setActive(true); QTRY_VERIFY(queries().contains("slow|"));
        poller.setActive(false); qputenv("ZT_TEST_MODE","empty"); poller.setActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!updated.isEmpty(),3000);
        for(const auto &event:availability) QVERIFY(event.first().toBool());
        poller.setActive(false); QTest::qWait(150); const auto idle=queries(); QTest::qWait(1200); QCOMPARE(queries(),idle);
    }
    void pluginImportIsAtomicAndDoesNotOverwrite() {
        QTemporaryDir directory;
        const QString name = "example.z2plugin";
        QFile source(directory.filePath(name));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("plugin bytes"); source.close();
        const auto target = directory.filePath("plugins");
        QVERIFY(vusiif::zto::plugins::importPlugin(source.fileName(),target).isEmpty());
        QFile installed(QDir(target).filePath(name)); QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(),QByteArray("plugin bytes")); installed.close();
        QVERIFY(!vusiif::zto::plugins::importPlugin(source.fileName(),target).isEmpty());
        QVERIFY(!vusiif::zto::plugins::importPlugin(directory.filePath("missing.dll"),target).isEmpty());
        const auto oldName=directory.filePath("old.ztplugin");
        QVERIFY(QFile::copy(source.fileName(),oldName));
        QVERIFY(!vusiif::zto::plugins::importPlugin(oldName,target).isEmpty());
        QFile blocker(directory.filePath("blocked")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        QVERIFY(!vusiif::zto::plugins::importPlugin(source.fileName(),blocker.fileName()).isEmpty());
    }

    void pluginLogsWriteBothSinksAndReportPartialFailure() {
        QTemporaryDir directory;
        vusiif::zto::util::HostServicesImpl host(QStringLiteral("测试插件"),directory.path(),"test-logger");
        std::vector<std::future<bool>> tasks;
        for (int i=0;i<4;++i) tasks.push_back(std::async(std::launch::async,[&host,i] {
            for (int j=0;j<20;++j) if (!host.logs().write(vusiif::zto::util::LogLevel::Info,QString("concurrent-%1-%2").arg(i).arg(j)).ok()) return false;
            return true;
        }));
        for (auto &task:tasks) QVERIFY(task.get());
        const QDir logs(QDir(host.dataDirectory()).filePath("logs"));
        QCOMPARE(logs.entryList({"*.log"},QDir::Files).size(),1);
        QFile plugin(logs.filePath(logs.entryList({"*.log"},QDir::Files).first()));
        QVERIFY(plugin.open(QIODevice::ReadOnly)); const auto entries = plugin.readAll();
        QCOMPARE(entries.count("[test-logger]"),80);
        QFile main(vusiif::zto::util::log::path()); QVERIFY(main.open(QIODevice::ReadOnly));
        QVERIFY(main.readAll().contains("[test-logger] concurrent-3-19"));
        // A display name is UI metadata only, so an unusable data root must be
        // what makes the plugin log sink fail while the host log still works.
        const auto blockedDataRoot = directory.filePath("blocked-data-root");
        { QFile blocker(blockedDataRoot); QVERIFY(blocker.open(QIODevice::WriteOnly)); }
        vusiif::zto::util::HostServicesImpl invalid(QStringLiteral("部分失败"), blockedDataRoot, "test-partial");
        QVERIFY(!invalid.storageReady());
        const auto result=invalid.logs().write(vusiif::zto::util::LogLevel::Warning,"partial-sink-probe");
        QVERIFY(result.hostError.isEmpty()); QVERIFY(!result.pluginError.isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(directory.path()).filePath(QStringLiteral("部分失败"))));
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
        QVERIFY(original.contains("ZTOrbit v" ZTO_VERSION));
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

    void logRotationNotifiesCallback()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString helperName = "app_log_probe";
#ifdef Q_OS_WIN
        helperName += ".exe";
#endif
        const QString helper = directory.filePath(helperName);
        QVERIFY(QFile::copy(QDir(QCoreApplication::applicationDirPath()).filePath(helperName), helper));
        QProcess process;
        process.start(helper, {QStringLiteral("--rotation")});
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        QCOMPARE(QString::fromUtf8(process.readAllStandardOutput()).trimmed(), QStringLiteral("1"));
        const QDir logs(directory.filePath("logs"));
        const QString date = QDateTime::currentDateTime().toString("yyMMdd");
        const auto files = logs.entryList({"*.log"}, QDir::Files, QDir::Name);
        QCOMPARE(files, (QStringList{date + "-1.log", date + "-2.log"}));
        QFile first(logs.filePath(date + "-1.log"));
        QVERIFY(first.open(QIODevice::ReadOnly));
        QVERIFY(first.size() >= qint64(10 * 1024 * 1024));
        QVERIFY(first.size() < qint64(10 * 1024 * 1024 + 8192));
    }

    void cacheWritesNotifyTheMaintenanceHook()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QStringList notified;
        HostServicesImpl host("缓存插件", directory.filePath("plugins-data"), "cache-hook",
                              directory.filePath("meta"), "run", "user",
                              [&notified](const QString &path) { notified.append(path); });
        QVERIFY2(host.storageReady(), qPrintable(host.storageError()));
        QVERIFY(host.files().writeFile("cache/item.bin", "one").ok());
        QCOMPARE(notified, QStringList{"cache/item.bin"});
        QVERIFY(host.files().writeFile("config.json", "config").ok());
        QCOMPARE(notified.size(), 1);
        auto *storage = dynamic_cast<vusiif::zto::util::PluginStorage *>(&host.files());
        QVERIFY(storage);
        const auto changed = storage->updateFile("cache/item.bin",
            [](const vusiif::zto::util::PluginFileSnapshot &) {
                return vusiif::zto::util::PluginFileUpdate::replace("two");
            });
        QVERIFY(changed.ok);
        QCOMPARE(notified, (QStringList{"cache/item.bin", "cache/item.bin"}));
        const auto kept = storage->updateFile("cache/item.bin",
            [](const vusiif::zto::util::PluginFileSnapshot &) {
                return vusiif::zto::util::PluginFileUpdate::keep();
            });
        QVERIFY(kept.ok);
        QCOMPARE(notified.size(), 2);
    }

    void cacheReadsCoordinateWithBackgroundCleanup()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto dataRoot = directory.filePath("plugins-data");
        const auto metadataRoot = directory.filePath(".zto");
        HostServicesImpl host(QStringLiteral("缓存读取"), dataRoot, "cache-reader", metadataRoot, "run", "user");
        QVERIFY2(host.storageReady(), qPrintable(host.storageError()));
        const QByteArray payload(512 * 1024, 'x');
        QVERIFY(host.files().writeFile("cache/data.bin", payload).ok());
        const auto path = QDir(host.dataDirectory()).filePath("cache/data.bin");
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::currentDateTime().addDays(-8), QFileDevice::FileModificationTime));
        }
        const auto context = vusiif::zto::storage::StorageContext::forTesting(directory.path());
        vusiif::zto::storage::StorageMaintenance maintenance(context);
        std::atomic<bool> stop{false};
        std::atomic<int> unexpected{0};
        std::atomic<int> reads{0};
        std::thread reader([&] {
            while (!stop.load()) {
                const auto result = host.files().readFile("cache/data.bin");
                if (result.ok()) {
                    if (result.data.size() != payload.size()) ++unexpected;
                    else ++reads;
                } else if (QFileInfo::exists(path)) {
                    // A missing file after reclamation is expected; any other
                    // failure while the reader still exists is not.
                    ++unexpected;
                }
                QThread::msleep(1);
            }
        });
        QElapsedTimer timer;
        timer.start();
        while (reads.load() < 2 && timer.elapsed() < 5000) QThread::msleep(5);
        const auto report = maintenance.cleanupCaches();
        stop.store(true);
        reader.join();
        QCOMPARE(unexpected.load(), 0);
        QVERIFY(reads.load() >= 2);
        QVERIFY2(report.ok(), qPrintable(report.error));
        QVERIFY(report.released.contains(path));
        QVERIFY(!QFileInfo::exists(path));
        // The directory is removed only after it became empty.
        QVERIFY(!QFileInfo::exists(QDir(host.dataDirectory()).filePath("cache")));
    }

    void pluginFilesUseDataRootAndRejectEscapes() {
        using namespace vusiif::zto::api;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        HostServicesImpl services("chat", directory.filePath("plugins-data"), "chat");
        QVERIFY2(services.storageReady(), qPrintable(services.storageError()));
        const QByteArray bytes("binary\0data", 11);
        QVERIFY(services.files().writeFile("example/file1.txt", bytes).ok());
        QFile actual(QDir(services.dataDirectory()).filePath("example/file1.txt"));
        QVERIFY(actual.open(QIODevice::ReadOnly)); QCOMPARE(actual.readAll(), bytes); actual.close();
        QVERIFY(services.files().writeFile("example/file1.txt", "replaced").ok());
        QCOMPARE(services.files().readFile("example\\file1.txt").data, QByteArray("replaced"));
        QVERIFY(services.files().writeFile("empty.txt", {}).ok());
        QVERIFY(services.files().readFile("empty.txt").ok());
        QVERIFY(!services.files().readFile("missing.txt").ok());
        for (const QString &path : {QString("../escape"), QString("/escape"), QString("C:/escape"),
             QString("example/../../escape"), QString("file:stream"), QString("example/../escape"), QString("example./escape"),
             QString("CON.txt"), QString("example/NUL"), QString("example/file1.txt. ")}) {
            QVERIFY(!services.files().writeFile(path, "bad").ok());
            QVERIFY(!services.files().readFile(path).ok());
        }
        QVERIFY(!QFileInfo::exists(directory.filePath("escape")));
        // A regular file cannot be traversed as a directory.
        QVERIFY(!services.files().writeFile("empty.txt/child", "bad").ok());
        QVERIFY(!services.files().readFile("example").ok());
        HostServicesImpl other("file-transfer", directory.filePath("plugins-data"), "file-transfer");
        QVERIFY(other.files().writeFile("example/file1.txt", "other plugin").ok());
        QCOMPARE(services.files().readFile("example/file1.txt").data, QByteArray("replaced"));
        QCOMPARE(other.files().readFile("example/file1.txt").data, QByteArray("other plugin"));
        QVERIFY(!services.files().writeFile("../file-transfer/example/file1.txt", "bad").ok());
        // Legacy display-name renames still migrate data, but the file service
        // only ever addresses the stable ID directory.
        HostServicesImpl chinese(QStringLiteral("语音聊天"), directory.filePath("plugins-data"), "voice-chat");
        QVERIFY2(chinese.storageReady(), qPrintable(chinese.storageError()));
        QVERIFY(chinese.files().writeFile("file1.txt", "voice").ok());
        QVERIFY(QFileInfo::exists(QDir(chinese.dataDirectory()).filePath("file1.txt")));
        QCOMPARE(QFileInfo(chinese.dataDirectory()).fileName(), QStringLiteral("id-voice-chat"));
        QVERIFY(!QFileInfo::exists(QDir(directory.filePath("plugins-data")).filePath(QStringLiteral("语音聊天"))));
        for (const QString &name : {QString("../escape"), QString(".."), QString("chat/other"), QString("chat\\other"),
             QString("chat:"), QString("CON.txt"), QString("chat."), QString("chat "), QString(), QString("builtin"),
             QStringLiteral("语音聊天")}) {
            HostServicesImpl invalid(QStringLiteral("显示名"), directory.filePath("plugins-data"), name);
            QVERIFY(!invalid.storageReady());
            QVERIFY(!invalid.storageError().isEmpty());
            QVERIFY(invalid.dataDirectory().isEmpty());
            QVERIFY(!invalid.files().writeFile("file1.txt", "bad").ok());
            QVERIFY(!invalid.files().readFile("file1.txt").ok());
        }
        QVERIFY(!QFileInfo::exists(QDir(directory.filePath("plugins-data")).filePath(QStringLiteral("显示名"))));
        // A legal ID that looks like a reserved device name is still safe
        // because the data directory always carries the "id-" prefix.
        HostServicesImpl deviceName(QStringLiteral("设备名"), directory.filePath("plugins-data"), "CON");
        QVERIFY2(deviceName.storageReady(), qPrintable(deviceName.storageError()));
        QVERIFY(deviceName.files().writeFile("file1.txt", "device").ok());
        QCOMPARE(QFileInfo(deviceName.dataDirectory()).fileName(), QStringLiteral("id-con"));
        // On Unix QFile::link creates a symlink (on Windows it creates a .lnk).
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(actual.fileName(), QDir(services.dataDirectory()).filePath("link")));
        QVERIFY(!services.files().readFile("link").ok());
        QVERIFY(!services.files().writeFile("link", "bad").ok());
        QVERIFY(QFile::link(QDir(services.dataDirectory()).absolutePath(),
                            directory.filePath("plugins-data/id-linked-plugin")));
        HostServicesImpl linkedPlugin("linked-plugin", directory.filePath("plugins-data"), "linked-plugin");
        QVERIFY(!linkedPlugin.files().readFile("example/file1.txt").ok());
        QVERIFY(!linkedPlugin.files().writeFile("example/file1.txt", "bad").ok());
        QVERIFY(QFile::link(directory.filePath("plugins-data"), directory.filePath("linked-data")));
        HostServicesImpl linkedRoot("chat", directory.filePath("linked-data"), "chat");
        QVERIFY(!linkedRoot.files().readFile("example/file1.txt").ok());
        QVERIFY(!linkedRoot.files().writeFile("example/file1.txt", "bad").ok());
#endif
    }

    void pluginStorageUsesStableIdAndCoordinatesTransactions() {
        using namespace vusiif::zto::util;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        HostServicesImpl host(QStringLiteral("测试插件"), directory.filePath("plugins-data"), "plugin-id");
        QVERIFY2(host.storageReady(), qPrintable(host.storageError()));
        QCOMPARE(QFileInfo(host.dataDirectory()).fileName(), QStringLiteral("id-plugin-id"));
        QVERIFY(QFileInfo::exists(QDir(host.dataDirectory()).absolutePath()));
        auto *storage = dynamic_cast<PluginStorage *>(&host.files());
        QVERIFY(storage);

        bool sawMissing = false;
        auto result = storage->updateFile("state.json", [&](const PluginFileSnapshot &snapshot) {
            sawMissing = !snapshot.exists && snapshot.data.isEmpty();
            return PluginFileUpdate::keep();
        });
        QVERIFY(result.ok); QVERIFY(!result.changed); QVERIFY(sawMissing);
        QVERIFY(!host.files().readFile("state.json").ok());

        result = storage->updateFile("state.json", [](const PluginFileSnapshot &snapshot) {
            return PluginFileUpdate::replace(QByteArray("one:") + snapshot.data);
        });
        QVERIFY(result.ok); QVERIFY(result.changed); QCOMPARE(result.data, QByteArray("one:"));
        QCOMPARE(host.files().readFile("state.json").data, QByteArray("one:"));

        result = storage->updateFile("state.json", [](const PluginFileSnapshot &snapshot) {
            return PluginFileUpdate::replace(snapshot.data);
        });
        QVERIFY(result.ok); QVERIFY(!result.changed); QCOMPARE(result.data, QByteArray("one:"));

        result = storage->updateFile("state.json", [](const PluginFileSnapshot &) {
            return PluginFileUpdate::failure(QStringLiteral("rejected"));
        });
        QVERIFY(!result.ok); QCOMPARE(result.error, QStringLiteral("rejected"));
        QCOMPARE(host.files().readFile("state.json").data, QByteArray("one:"));

        result = storage->updateFile("state.json", [](const PluginFileSnapshot &) -> PluginFileUpdate {
            throw std::runtime_error("boom");
        });
        QVERIFY(!result.ok); QCOMPARE(host.files().readFile("state.json").data, QByteArray("one:"));

        result = storage->updateFile("state.json", [](const PluginFileSnapshot &) {
            return PluginFileUpdate::replace({});
        });
        QVERIFY(result.ok); QVERIFY(result.changed);
        const auto empty = host.files().readFile("state.json");
        QVERIFY(empty.ok()); QVERIFY(empty.data.isEmpty());

        bool escapeCalled = false;
        result = storage->updateFile("../escape.json", [&](const PluginFileSnapshot &) {
            escapeCalled = true;
            return PluginFileUpdate::keep();
        });
        QVERIFY(!result.ok); QVERIFY(!escapeCalled);
        QVERIFY(!QFileInfo::exists(directory.filePath("escape.json")));

        std::vector<std::future<bool>> tasks;
        for (int thread = 0; thread < 4; ++thread) tasks.push_back(std::async(std::launch::async, [&storage] {
            for (int index = 0; index < 25; ++index) {
                const auto update = storage->updateFile("counter.json", [](const PluginFileSnapshot &snapshot) {
                    const int current = snapshot.exists ? snapshot.data.toInt() : 0;
                    return PluginFileUpdate::replace(QByteArray::number(current + 1));
                });
                if (!update.ok) return false;
            }
            return true;
        }));
        for (auto &task : tasks) QVERIFY(task.get());
        QCOMPARE(host.files().readFile("counter.json").data, QByteArray("100"));

        const auto allowed = storage->legacyImportAllowed();
        QVERIFY2(allowed.ok(), qPrintable(allowed.error)); QVERIFY(allowed.allowed);
        QVERIFY(storage->completeLegacyImport().ok());
        QVERIFY(!storage->legacyImportAllowed().allowed);
        QVERIFY(storage->completeLegacyImport().ok());
        // Host-owned metadata lives under the root, outside plugin data.
        QVERIFY(QFileInfo::exists(QDir(directory.path()).filePath(".zto/plugins/plugin-id.json")));
    }

    void pendingMigrationBlocksConsumerInitializationUntilRetry()
    {
        using namespace vusiif::zto::util;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto shared = directory.filePath("application");
        const auto personalBase = directory.filePath("user-data");
        const auto sharedPluginData = QDir(shared).filePath("plugins-data/id-field-probe");
        QVERIFY(QDir().mkpath(sharedPluginData));
        const auto sourceConfig = QDir(sharedPluginData).filePath("config.json");
        {
            QFile config(sourceConfig);
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"nickname\":\"original\",\"volume\":123}");
        }
        // Pre-record the personal route so the selection is deterministic.
        const auto routeDirectory = QDir(personalBase).filePath(
            QStringLiteral("installations/") + StorageContext::installKey(shared));
        QVERIFY(QDir().mkpath(routeDirectory));
        {
            QFile route(QDir(routeDirectory).filePath("route.json"));
            QVERIFY(route.open(QIODevice::WriteOnly));
            route.write(QJsonDocument(QJsonObject{{"format", 1}, {"mode", "personal"}}).toJson());
        }
        // Block reading the source for the duration of the first prepare, which
        // forces a deterministic partial migration without touching the target.
#ifdef Q_OS_WIN
        const HANDLE sourceHandle = CreateFileW(reinterpret_cast<LPCWSTR>(sourceConfig.utf16()),
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(sourceHandle != INVALID_HANDLE_VALUE);
#else
        if (::geteuid() == 0) QSKIP("root can read the blocked migration source");
        QVERIFY(QFile::setPermissions(sourceConfig, QFile::Permissions()));
#endif
        auto storage = StorageContext::resolveForTesting(shared, personalBase);
        QCOMPARE(storage.mode(), StorageContext::Mode::Personal);
        QString error;
        QVERIFY2(storage.prepare(error), qPrintable(error));
        QVERIFY(!storage.migrationComplete());
        QVERIFY(!storage.migrationCorrupt());
#ifdef Q_OS_WIN
        CloseHandle(sourceHandle);
#else
        QVERIFY(QFile::setPermissions(sourceConfig, QFile::ReadOwner | QFile::WriteOwner
            | QFile::ReadUser | QFile::WriteUser));
#endif
        // A normal consumer starts before the next retry. It must not seed
        // defaults into the directory that is still waiting for the original.
        {
            HostServicesImpl host(QStringLiteral("FieldProbe"), storage.pluginDataDirectory(), "field-probe",
                                  storage.metadataDirectory(), storage.runId(), storage.userKeyValue());
            QVERIFY2(host.storageReady(), qPrintable(host.storageError()));
            QVERIFY(!host.migrationBlock().isEmpty());
            const auto written = host.files().writeFile("config.json", "{\"nickname\":\"default\"}");
            QVERIFY(!written.ok());
            QVERIFY(written.error.contains(QStringLiteral("共享数据尚未恢复")));
            auto *pluginStorage = dynamic_cast<PluginStorage *>(&host.files());
            QVERIFY(pluginStorage);
            bool called = false;
            const auto updated = pluginStorage->updateFile("config.json", [&](const PluginFileSnapshot &) {
                called = true;
                return PluginFileUpdate::replace("{\"nickname\":\"default\"}");
            });
            QVERIFY(!updated.ok);
            QVERIFY(!called);
            QVERIFY(!host.files().readFile("config.json").ok());
            // Disposable cache and log paths stay usable during the wait.
            QVERIFY(host.files().writeFile("cache/entry.bin", "cache").ok());
            QVERIFY(host.files().writeFile("logs/250101-1.log", "log").ok());
        }
        QVERIFY(!QFileInfo::exists(QDir(storage.pluginDataDirectory("field-probe")).filePath("config.json")));
        // Restart: the failed item is retried, then the consumer reads the
        // original configuration instead of a default value.
        auto retry = StorageContext::resolveForTesting(shared, personalBase);
        QVERIFY2(retry.prepare(error), qPrintable(error));
        QVERIFY(retry.migrationComplete());
        HostServicesImpl recovered(QStringLiteral("FieldProbe"), retry.pluginDataDirectory(), "field-probe",
                                   retry.metadataDirectory(), retry.runId(), retry.userKeyValue());
        QVERIFY2(recovered.storageReady(), qPrintable(recovered.storageError()));
        QVERIFY(recovered.migrationBlock().isEmpty());
        const auto config = recovered.files().readFile("config.json");
        QVERIFY2(config.ok(), qPrintable(config.error));
        const auto object = QJsonDocument::fromJson(config.data).object();
        QCOMPARE(object.value("nickname").toString(), QStringLiteral("original"));
        QCOMPARE(object.value("volume").toInt(), 123);
    }

    void pluginStorageUpdatesAreAtomicAcrossProcesses() {
#ifdef STORAGE_UPDATE_PROBE
        using namespace vusiif::zto::util;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto dataRoot = directory.filePath("plugins-data");
        QProcess first, second;
        first.start(STORAGE_UPDATE_PROBE, {dataRoot, "120"});
        second.start(STORAGE_UPDATE_PROBE, {dataRoot, "120"});
        QVERIFY(first.waitForStarted(5000)); QVERIFY(second.waitForStarted(5000));
        QVERIFY(first.waitForFinished(30000)); QVERIFY(second.waitForFinished(30000));
        QVERIFY2(first.exitStatus() == QProcess::NormalExit && first.exitCode() == 0,
                 first.readAllStandardError().constData());
        QVERIFY2(second.exitStatus() == QProcess::NormalExit && second.exitCode() == 0,
                 second.readAllStandardError().constData());
        HostServicesImpl host(QStringLiteral("StorageProbe"), dataRoot, "storage-probe");
        QVERIFY2(host.storageReady(), qPrintable(host.storageError()));
        QCOMPARE(host.files().readFile("counter.json").data.toInt(), 240);
#else
        QSKIP("Storage update probe not configured");
#endif
    }

    void concurrentProcessesMergeDifferentFieldsAndRemarks() {
#ifdef STORAGE_UPDATE_PROBE
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto dataRoot = directory.filePath("plugins-data");
        const auto start = [&](QProcess &process, const QStringList &arguments) {
            process.start(STORAGE_UPDATE_PROBE, arguments);
            return process.waitForStarted(5000);
        };
        const auto finish = [](QProcess &process) {
            if (!process.waitForFinished(30000)) return false;
            if (process.exitStatus() != QProcess::NormalExit) return false;
            if (process.exitCode() != 0) {
                qWarning().noquote() << process.readAllStandardError();
                return false;
            }
            return true;
        };
        // Two processes update different fields of the same file.
        QProcess nickname, volume;
        QVERIFY(start(nickname, {"--json-field", dataRoot, "config.json", "nickname", "Alice", "60"}));
        QVERIFY(start(volume, {"--json-field", dataRoot, "config.json", "volume", "35", "60"}));
        QVERIFY(finish(nickname));
        QVERIFY(finish(volume));
        HostServicesImpl host(QStringLiteral("MergeProbe"), dataRoot, "field-probe");
        QVERIFY2(host.storageReady(), qPrintable(host.storageError()));
        const auto configResult = host.files().readFile("config.json");
        QVERIFY2(configResult.ok(), qPrintable(configResult.error));
        const auto config = QJsonDocument::fromJson(configResult.data).object();
        QCOMPARE(config.value("nickname").toString(), QStringLiteral("Alice"));
        QCOMPARE(config.value("volume").toInt(), 35);

        // Two processes add remarks for different member IDs.
        QProcess alice, bob;
        QVERIFY(start(alice, {"--remark", dataRoot, "a1b2c3d4e5", QStringLiteral("本地朋友"), "40"}));
        QVERIFY(start(bob, {"--remark", dataRoot, "1234567890", "Bob", "40"}));
        QVERIFY(finish(alice));
        QVERIFY(finish(bob));
        HostServicesImpl remarksHost(QStringLiteral("RemarkMergeProbe"), dataRoot, "remark-probe");
        QVERIFY2(remarksHost.storageReady(), qPrintable(remarksHost.storageError()));
        const auto remarksResult = remarksHost.files().readFile("member-remarks.json");
        QVERIFY2(remarksResult.ok(), qPrintable(remarksResult.error));
        const auto remarks = QJsonDocument::fromJson(remarksResult.data).object();
        QCOMPARE(remarks.value("a1b2c3d4e5").toString(), QStringLiteral("本地朋友"));
        QCOMPARE(remarks.value("1234567890").toString(), QStringLiteral("Bob"));
#else
        QSKIP("Storage update probe not configured");
#endif
    }

    void pluginZeroTierQueriesPreserveJsonAndCancelCallbacks() {
        using namespace vusiif::zto::api;
        HostServicesImpl services("query-test", m_settings.filePath("plugin-tools"), "query-test");
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
        quoting.start(shell, {"-c", "printf '%s' " + vusiif::zto::platform::quote(value)});
        QVERIFY(quoting.waitForFinished(5000));
        QCOMPARE(quoting.exitCode(), 0);
        QCOMPARE(QString::fromUtf8(quoting.readAllStandardOutput()), value);
        const QString directory = "/Library/Application Support/ZeroTier/One/moons.d";
        const QString target = directory + "/000000abcdef1234.moon";
        const QStringList scripts = {
            vusiif::zto::platform::serviceCommands("start", "/usr/local/bin/zerotier-cli"),
            vusiif::zto::platform::serviceCommands("stop", "/usr/local/bin/zerotier-cli"),
            vusiif::zto::platform::serviceCommands("restart", "/usr/local/bin/zerotier-cli"),
            vusiif::zto::platform::packageScript(false, "/Library/Application Support/ZeroTier"),
            vusiif::zto::platform::packageScript(true, "/Library/Application Support/ZeroTier"),
            vusiif::zto::platform::copyMoons({{"/tmp/file's.moon", target}}, directory),
            vusiif::zto::platform::deleteMoon(target, directory),
            vusiif::zto::platform::synchronizeSession("/Library/Application Support/ZeroTier/One", "/tmp/zto-session-test", 501)
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
        QCOMPARE(vusiif::zto::platform::deleteMoon("/tmp/outside.moon", directory), QString("exit 1\n"));
        QVERIFY(!vusiif::zto::platform::synchronizeSession("/source", "/cache", 501).contains("identity.secret"));
        QCOMPARE(vusiif::zto::platform::appleScriptString("a\"b\\c"), QString("\"a\\\"b\\\\c\""));
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
        const vusiif::zto::zerotier::bundle::Configuration original {"ABCDEF1234000001", "000000ABCDEF1234.moon", QByteArray("signed\0moon", 11)};
        const QByteArray encoded = vusiif::zto::zerotier::bundle::encode(original);
        QVERIFY(!encoded.isEmpty());
        vusiif::zto::zerotier::bundle::Configuration decoded;
        QString error;
        QVERIFY(vusiif::zto::zerotier::bundle::decode(encoded, decoded, error));
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
            QVERIFY(!vusiif::zto::zerotier::bundle::decode(QJsonDocument(object).toJson(), decoded, error));
            QVERIFY(!error.isEmpty());
            QVERIFY(decoded.networkId.isEmpty());
        }
        QVERIFY(!vusiif::zto::zerotier::bundle::decode("not json", decoded, error));
        QVERIFY(!vusiif::zto::zerotier::bundle::decode(QByteArray(vusiif::zto::zerotier::bundle::MaxFileSize + 1, 'a'), decoded, error));
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
        const QString script = vusiif::zto::zerotier::packageScript(true, vusiif::zto::zerotier::executable());
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
                                      + vusiif::zto::zerotier::quotePowerShell(lockedPath);
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
        auto invocation = vusiif::zto::zerotier::preparePowerShell(
            "Set-Content -LiteralPath " + vusiif::zto::zerotier::quotePowerShell(actualMarker)
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

        auto failure = vusiif::zto::zerotier::preparePowerShell(QStringLiteral("Write-Output '实际失败原因'; exit 23"));
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
        using namespace vusiif::zto::api;
        QCOMPARE(ApiMajor, 0);
        QCOMPARE(ApiMinor, 5);
        QCOMPARE(QString::fromLatin1(ApiVersion), QStringLiteral(ZTO_API_VERSION));
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
        using namespace vusiif::zto::api;
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
        using namespace vusiif::zto::api;
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
