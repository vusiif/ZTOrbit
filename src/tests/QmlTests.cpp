#include "AppController.h"
#include "RecordModel.h"
#include "NetworkBundle.h"
#include "PluginRuntime.h"
#include "storage/SettingsStore.h"
#include "storage/StorageContext.h"
#include <zto/PluginPage.h>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QPixmap>
#include <QQuickStyle>
#include <QSettings>
#include <QTemporaryDir>
#include <QDir>
#include <QClipboard>
#include <QJsonArray>
#include <QFontDatabase>
#include <QIcon>
#include <QStyleHints>
#include <QTcpServer>
#include <QTcpSocket>
#include <ztprotocol/WireProtocol.h>
#include <QSignalSpy>
#include <QtTest>
#include <algorithm>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif
using namespace vusiif::zto::ui;
using namespace vusiif::zto::api;

class DeferredOperations : public SystemOperations {
public:
    using SystemOperations::SystemOperations;
    QString script;
    std::function<void(bool)> completion;
    void runElevatedScript(const QString &, const QString &value, std::function<void(bool)> finished) override { script = value; completion = std::move(finished); }
    void complete(bool ok) { auto done = std::move(completion); if (done) done(ok); }
};
class RecordingOperations : public SystemOperations {
public:
    int operations = 0;
    QStringList scripts;
    void runElevatedScript(const QString &, const QString &script, std::function<void(bool)> finished) override
    {
        ++operations;
        scripts.append(script);
        if (finished) finished(false);
    }
};
class RevealOperations : public SystemOperations {
public:
    int calls = 0;
    QString path;
    bool revealFile(const QString &value, QString &error) override
    {
        ++calls;
        path = value;
        error.clear();
        return true;
    }
};
class ScriptExecutingOperations : public SystemOperations {
public:
    int operations = 0;
    QString lastScript;
    QString output;
    void runElevatedScript(const QString &, const QString &script, std::function<void(bool)> finished) override
    {
        ++operations;
        lastScript = script;
#ifdef Q_OS_WIN
        const auto invocation = vusiif::zto::zerotier::preparePowerShell(
            "function Stop-Service { }\n"
            "function Start-Service { }\n"
            "function netsh { $global:LASTEXITCODE = 0 }\n"
            "function Get-Content { [CmdletBinding()] param([string]$LiteralPath,[switch]$Raw); "
            "if ($LiteralPath -like '*zerotier-one.port') { return '9993' }; "
            "if ($LiteralPath -like '*authtoken.secret') { return 'mock-token' }; "
            "Microsoft.PowerShell.Management\\Get-Content @PSBoundParameters }\n"
            "function Invoke-RestMethod { return @{address='0123456789'} }\n"
            "function Get-Service { $s = New-Object PSObject; Add-Member -InputObject $s -MemberType ScriptMethod "
            "-Name WaitForStatus -Value {}; return $s }\n" + script);
        QProcess process;
        process.start("powershell.exe", invocation.arguments);
        const bool completed = process.waitForFinished(30000);
        const bool ok = completed && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        QFile log(invocation.logPath);
        if (log.open(QIODevice::ReadOnly)) output = QString::fromUtf8(log.readAll());
        if (finished) finished(ok);
#else
        Q_UNUSED(script)
        if (finished) finished(false);
#endif
    }
};
class QmlTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_directory;
    vusiif::zto::storage::StorageContext m_storage;
    QByteArray m_path, m_programData;
    QStringList m_errors;
    std::unique_ptr<AppController> m_controller;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    QVariant setting(const QString &key, const QVariant &fallback = {}) const {
        vusiif::zto::storage::SettingsStore store(m_storage.settingsFile());
        return store.value(key, fallback);
    }
    bool hasSetting(const QString &key) const {
        vusiif::zto::storage::SettingsStore store(m_storage.settingsFile());
        return store.contains(key);
    }
    QObject *object(const char *name) const {
        if (!m_window) return nullptr;
        const auto target=QString::fromLatin1(name);
        if (auto *found=m_window->findChild<QObject *>(target)) return found;
        // Repeater delegates can have a different QObject ownership tree from
        // the visual tree, so locate dynamically created items by visual parent.
        QSet<QObject *> visited;
        std::function<QObject *(QObject *)> find=[&](QObject *item)->QObject * {
            if (visited.contains(item)) return nullptr;
            visited.insert(item);
            if (item->objectName()==target) return item;
            for (auto *child:item->children()) if (auto *found=find(child)) return found;
            if (auto *visual=qobject_cast<QQuickItem *>(item))
                for (auto *child:visual->childItems()) if (auto *found=find(child)) return found;
            return nullptr;
        };
        return find(m_window->contentItem());
    }
    QString localConfigPath() const { return vusiif::zto::zerotier::localConfigPath(); }
    void prepareLocalConfigDirectory() const
    {
        QDir().mkpath(vusiif::zto::zerotier::systemHomeDirectory());
        QDir().mkpath(vusiif::zto::zerotier::moonDirectory());
    }
    QByteArray readLocalConfig() const
    {
        QFile file(localConfigPath());
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    void writeLocalConfig(const QByteArray &bytes) const
    {
        prepareLocalConfigDirectory();
        QFile file(localConfigPath());
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }
    void writeMoonFile(const QString &name = QStringLiteral("0000000123456789.moon"),
                       const QByteArray &bytes = QByteArray("signed-moon")) const
    {
        prepareLocalConfigDirectory();
        QFile file(QDir(vusiif::zto::zerotier::moonDirectory()).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }
    void openWindow(const QString &pluginDirectory = {}, SystemOperations *operations = nullptr) {
        m_controller = std::make_unique<AppController>(nullptr,pluginDirectory,operations,m_storage);
        m_engine = std::make_unique<QQmlApplicationEngine>();
        connect(m_engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) { for (const auto &error : errors) m_errors.append(error.toString()); });
        m_engine->setInitialProperties({{"backend", QVariant::fromValue(m_controller.get())}});
        m_engine->load(QUrl("qrc:/qt/qml/ZTOrbit/qml/Main.qml"));
        QVERIFY2(!m_engine->rootObjects().isEmpty(), qPrintable(m_errors.join('\n')));
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
        QVERIFY(m_window); m_controller->setWindow(m_window);
        QTest::qWait(100);
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void scrollSettingsTo(const char *name) {
        auto *scroll = qobject_cast<QQuickItem *>(object("settingsScroll"));
        QVERIFY(scroll);
        auto *flick = qobject_cast<QQuickItem *>(scroll->property("contentItem").value<QObject *>());
        QVERIFY(flick);
        auto *target = qobject_cast<QQuickItem *>(object(name));
        QVERIFY(target);
        const auto point = target->mapToItem(flick, QPointF(0, 0));
        const auto maximum = std::max(0.0, flick->property("contentHeight").toDouble() - flick->height());
        flick->setProperty("contentY", qBound(0.0,
            flick->property("contentY").toDouble() + point.y() - 80, maximum));
        QTest::qWait(120);
    }
    void click(const char *name) {
        auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY(item);
        QTest::mouseClick(m_window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint());
    }
    QObject *named(const QString &name) const { return object(name.toLatin1().constData()); }
    QQuickItem *item(const QString &name) const { return qobject_cast<QQuickItem *>(named(name)); }
    QQuickItem *navigationItem(const QString &menuId) const { return item(QStringLiteral("navigation-") + menuId); }
    void writeSetting(const QString &key, const QVariant &value) {
        vusiif::zto::storage::SettingsStore store(m_storage.settingsFile());
        store.setValue(key, value);
        QString error;
        QVERIFY2(store.sync(error), qPrintable(error));
    }
    QVariant evaluate(QObject *target, const QString &expression) const {
        if (!target) return {};
        QQmlExpression expr(qmlContext(target), target, expression);
        if (expr.hasError()) return {};
        return expr.evaluate();
    }
private slots:
    void initTestCase() {
        QVERIFY(m_directory.isValid());
        QCoreApplication::setOrganizationName("ZTOrbit-QML-Tests"); QCoreApplication::setApplicationName("QmlTests");
        QCoreApplication::setApplicationVersion(ZTO_VERSION);
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_directory.path());
        m_storage = vusiif::zto::storage::StorageContext::forTesting(m_directory.filePath("storage"));
        m_path = qgetenv("PATH"); m_programData = qgetenv("ProgramData");
        qputenv("PATH", QByteArray(ZTO_FAKE_PING_DIRECTORY) + QDir::listSeparator().toLatin1() + QCoreApplication::applicationDirPath().toLocal8Bit() + QDir::listSeparator().toLatin1() + m_path);
        qputenv("ProgramData", m_directory.path().toLocal8Bit());
        qputenv("ZT_TEST_DATA_DIRECTORY", (m_directory.path()+"/ZeroTier").toLocal8Bit());
        QDir().mkpath(".tmp/screenshots");
    }
    void init() {
        qputenv("ZT_TEST_MODE", "compact-lists");
        m_errors.clear();
        if (QFileInfo::exists(localConfigPath()))
            QFile::setPermissions(localConfigPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ReadUser | QFileDevice::WriteUser);
        QFile::remove(localConfigPath());
        const QDir moons(vusiif::zto::zerotier::moonDirectory());
        for (const auto &info : moons.entryInfoList(QDir::Files))
            QFile::remove(info.absoluteFilePath());
    }
    void trayAndMinimizePauseUiPollingAndRefreshOnRestore() {
        const auto path=m_directory.filePath("tray-queries.log"); QFile::remove(path);
        qputenv("ZT_TEST_QUERY_LOG",path.toUtf8());
        const auto queries=[&] { QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); };
        openWindow(); QTRY_COMPARE(m_controller->networks()->count(),2);
        const auto old=m_controller->networks()->get(0); m_controller->setExpanded(old["rowId"].toString(),true);
        m_window->hide(); QTRY_VERIFY(!m_window->isVisible()); QTest::qWait(200);
        for(auto *poller:m_controller->findChildren<vusiif::zto::zerotier::JsonPoller *>()) QVERIFY(!poller->isActive());
        const auto hidden=queries(); QSignalSpy hiddenUpdates(m_controller.get(),&AppController::stateChanged);
        QTest::qWait(2200); QCOMPARE(queries(),hidden); QCOMPARE(hiddenUpdates.size(),0);
        qputenv("ZT_TEST_MODE","empty");
        m_controller->restoreWindow(); QTRY_VERIFY(m_window->isVisible()); QTRY_COMPARE(m_controller->networks()->count(),0);
        m_window->showMinimized(); QTRY_COMPARE(m_window->windowState(),Qt::WindowMinimized); QTest::qWait(200);
        const auto minimized=queries(); QTest::qWait(1200); QCOMPARE(queries(),minimized);
        qputenv("ZT_TEST_MODE","compact-lists");
        m_controller->restoreWindow(); QTRY_COMPARE(m_controller->networks()->count(),2);
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void hiddenWindowCancelsMemberRequestsAndPings() {
        qputenv("ZT_TEST_MODE","members"); qputenv("ZT_TEST_PING_MODE","hang");
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,vusiif::zto::protocol::NetworkPort));
        int requests=0; bool respond=false; QPointer<QTcpSocket> waiting;
        connect(&server,&QTcpServer::newConnection,this,[&] {
            auto *socket=server.nextPendingConnection(); auto input=std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::readyRead,socket,[&,socket,input] {
                *input+=socket->readAll(); vusiif::zto::protocol::Packet packet;
                if(vusiif::zto::protocol::takeFrame(*input,packet)!=vusiif::zto::protocol::ReadResult::Packet) return;
                const auto request=QJsonDocument::fromJson(packet.payload).object(); ++requests;
                if(!respond) { waiting=socket; return; }
                const QJsonArray members{QJsonObject{{"nodeId","0123456789"},{"ipAssignments",QJsonArray{"127.0.0.3"}}}};
                socket->write(vusiif::zto::protocol::frame("builtin",QJsonDocument(QJsonObject{{"v",1},{"id",request["id"]},{"ok",true},{"data",members}}).toJson(QJsonDocument::Compact)));
            });
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        });
        openWindow(); QTRY_COMPARE(m_controller->networks()->count(),1);
        m_controller->setExpanded("0123456789abcdef",true); QTRY_COMPARE(requests,1); QVERIFY(waiting);
        m_window->hide(); QTRY_VERIFY(!waiting || waiting->state()==QAbstractSocket::UnconnectedState);
        QTest::qWait(1200); QCOMPARE(requests,1);
        respond=true; m_controller->restoreWindow(); QTRY_VERIFY(requests>=2);
        QTRY_VERIFY(!m_controller->networks()->record("0123456789abcdef")["members"].toList().isEmpty());
        QTRY_VERIFY(!m_controller->findChildren<QProcess *>().isEmpty());
        m_window->hide();
        QTRY_VERIFY(m_controller->findChildren<QProcess *>().isEmpty());
        QVERIFY(m_controller->networks()->record("0123456789abcdef")["expanded"].toBool());
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void pluginSelectionAndDeleteConfirmation() {
        QTemporaryDir packages; const auto path=packages.filePath("invalid.z2plugin");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("invalid package"); file.close();
        openWindow(packages.path()); QTRY_COMPARE(m_controller->plugins().size(),1);
        QTRY_VERIFY(!m_controller->plugins().first().toMap()["loading"].toBool());
        QTRY_VERIFY(object("notificationBanner") && object("notificationBanner")->property("visible").toBool());
        click("dismissNotificationButton"); QTRY_VERIFY(!object("notificationBanner")->property("visible").toBool());
        click("settingsButton"); QTest::qWait(220); click("openPluginsButton"); QTest::qWait(220);
        click("pluginRow-"); QVERIFY(object("pluginRow-")->property("highlighted").toBool()); QVERIFY(!object("pluginAboutDialog"));
        const auto removeFromMenu=[this] {
            auto *row=qobject_cast<QQuickItem *>(object("pluginRow-")); QVERIFY(row);
            QTest::mouseClick(m_window,Qt::RightButton,{},row->mapToScene(QPointF(20,18)).toPoint()); QTest::qWait(180);
            QVERIFY(object("pluginLoad-")->property("visible").toBool()); QVERIFY(!object("pluginUnload-")->property("visible").toBool());
            QVERIFY(object("pluginDelete-")->property("visible").toBool()); click("pluginDelete-");
        };
        removeFromMenu(); QTRY_VERIFY(object("confirmationDialog") && object("confirmationDialog")->property("visible").toBool());
        QVERIFY(QFileInfo::exists(path)); click("cancelActionButton"); QTRY_VERIFY(!object("confirmationDialog")->property("visible").toBool()); QVERIFY(QFileInfo::exists(path));
        QTest::qWait(180); removeFromMenu(); QTRY_VERIFY(object("confirmationDialog")->property("visible").toBool()); click("confirmActionButton");
        QTRY_VERIFY(m_controller->plugins().isEmpty()); QVERIFY(!QFileInfo::exists(path));
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void missingDependencyShowsNoticeWithoutLoading() {
#ifdef ZTO_UI_VOICE_PACKAGE
        QTemporaryDir packages; QVERIFY(QFile::copy(ZTO_UI_VOICE_PACKAGE,packages.filePath("voice.z2plugin")));
        openWindow(packages.path());
        QTRY_VERIFY_WITH_TIMEOUT(object("notificationBanner") && object("notificationBanner")->property("visible").toBool(),10000);
        QVERIFY(object("notificationBanner")->property("message").toString().contains("media"));
        QVERIFY(m_controller->pluginRegistry()->plugins().isEmpty());
        for(const auto &v:m_controller->plugins()) { QVERIFY(!v.toMap()["loaded"].toBool()); QVERIFY(!v.toMap()["loading"].toBool()); }
        click("dismissNotificationButton"); QTRY_VERIFY(!object("notificationBanner")->property("visible").toBool());
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
#else
        QSKIP("Voice package required");
#endif
    }
    void serviceDependenciesBlockUnload_data() {
        QTest::addColumn<bool>("dark"); QTest::newRow("light")<<false; QTest::newRow("dark")<<true;
    }
    void serviceDependenciesBlockUnload() {
#ifndef ZTO_UI_MEDIA_PACKAGE
        QSKIP("Configure the independently built media and voice packages for this integration check");
#else
        QFETCH(bool,dark);
        QTemporaryDir packages; QVERIFY(QFile::copy(ZTO_UI_VOICE_PACKAGE,packages.filePath("voice.z2plugin")));
        QVERIFY(QFile::copy(ZTO_UI_MEDIA_PACKAGE,packages.filePath("media.z2plugin")));
        int ticks=0; QTimer heartbeat; heartbeat.setInterval(5); connect(&heartbeat,&QTimer::timeout,this,[&] { ++ticks; }); heartbeat.start();
        openWindow(packages.path());
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->pluginRegistry()->plugins().size(),2,80000);
        QTRY_COMPARE(m_controller->pluginRegistry()->preparationState("voice-chat"),vusiif::zto::plugins::PreparationState::Ready);
        click("settingsButton"); QTest::qWait(200);
        if(dark) { click("themeOption-dark"); QTRY_VERIFY(m_controller->dark()); QTest::qWait(250); }
        click("openPluginsButton"); QTRY_VERIFY(object("pluginDialog") && object("pluginDialog")->property("visible").toBool()); QTest::qWait(200);
        click("pluginRow-media"); QVERIFY(!object("pluginAboutDialog")); QVERIFY(object("pluginRow-media")->property("highlighted").toBool());
        auto *selected=qobject_cast<QQuickItem *>(object("pluginRow-media")); QVERIFY(selected);
        QTest::mouseClick(m_window,Qt::RightButton,{},selected->mapToScene(QPointF(20,18)).toPoint()); QTest::qWait(180);
        QVERIFY(!object("pluginLoad-media")->property("visible").toBool()); QVERIFY(!object("pluginDelete-media")->property("visible").toBool());
        click("pluginAbout-media"); QTRY_VERIFY(object("pluginAboutDialog") && object("pluginAboutDialog")->property("visible").toBool()); QTest::qWait(200);
        QVERIFY(object("pluginServicesLabel")->property("text").toString().contains("media.session"));
        QVERIFY(m_window->grabWindow().save(dark?".tmp/screenshots/plugin-services-dark.png":".tmp/screenshots/plugin-services-light.png"));
        QTest::keyClick(m_window,Qt::Key_Escape); QTRY_VERIFY(!object("pluginAboutDialog")->property("visible").toBool());
        m_controller->loadPlugin("MEDIA"); QTRY_VERIFY(object("notificationBanner") && object("notificationBanner")->property("visible").toBool());
        QVERIFY(object("notificationBanner")->property("message").toString().contains(QStringLiteral("同 ID")));
        QCOMPARE(m_controller->pluginRegistry()->plugins().size(),2);
        click("dismissNotificationButton"); QTRY_VERIFY(!object("notificationBanner")->property("visible").toBool());
        const auto unloadFromMenu=[this] {
            auto *row=qobject_cast<QQuickItem *>(object("pluginRow-media")); QVERIFY(row);
            QTest::mouseClick(m_window,Qt::RightButton,{},row->mapToScene(QPointF(row->width()/2,row->height()/2)).toPoint());
            QTest::qWait(160); click("pluginUnload-media");
        };
        unloadFromMenu(); QTRY_VERIFY(object("notificationBanner") && object("notificationBanner")->property("visible").toBool());
        QVERIFY(object("notificationBanner")->property("message").toString().contains("voice-chat"));
        QTest::qWait(180); QVERIFY(m_window->grabWindow().save(dark?".tmp/screenshots/plugin-dependency-dark.png":".tmp/screenshots/plugin-dependency-light.png"));
        click("dismissNotificationButton"); QTRY_VERIFY(!object("notificationBanner")->property("visible").toBool());
        QCOMPARE(m_controller->pluginRegistry()->plugins().size(),2);
        m_controller->unloadPlugin("voice-chat"); QTRY_VERIFY_WITH_TIMEOUT(m_controller->pluginRegistry()->plugins().isEmpty(),10000);
        QVERIFY(ticks>5); QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
        QSignalSpy exited(m_controller.get(),&AppController::exitFinished); m_controller->requestQuit(); QTRY_COMPARE(exited.size(),1);
#endif
    }
    void mediaOnlyUnloadMemory() {
#if defined(Q_OS_WIN) && defined(ZTO_UI_MEDIA_PACKAGE)
        if(!qEnvironmentVariableIntValue("ZTO_TEST_MEDIA_MEMORY")) QSKIP("Set ZTO_TEST_MEDIA_MEMORY=1 for the repeated physical worker memory check");
        QTemporaryDir packages; openWindow(packages.path()); QTest::qWait(500);
        QFile file(".tmp/screenshots/media-unload-memory.csv"); QVERIFY(file.open(QIODevice::WriteOnly));
        QTextStream csv(&file); csv<<"round,privateBytes,workingSet,handles,threads\n";
        const auto measure=[&](int round) {
            PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory);
            QVERIFY(K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory)));
            DWORD handles=0; QVERIFY(GetProcessHandleCount(GetCurrentProcess(),&handles));
            int threads=0; const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0); QVERIFY(snapshot!=INVALID_HANDLE_VALUE);
            THREADENTRY32 entry{}; entry.dwSize=sizeof(entry);
            if(Thread32First(snapshot,&entry)) do { if(entry.th32OwnerProcessID==GetCurrentProcessId()) ++threads; } while(Thread32Next(snapshot,&entry));
            CloseHandle(snapshot);
            csv<<round<<','<<qulonglong(memory.PrivateUsage)<<','<<qulonglong(memory.WorkingSetSize)<<','<<handles<<','<<threads<<'\n'; csv.flush();
            const auto modules=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId()); QVERIFY(modules!=INVALID_HANDLE_VALUE);
            MODULEENTRY32W module{}; module.dwSize=sizeof(module);
            QStringList mediaModules;
            if(Module32FirstW(modules,&module)) do {
                const auto name=QString::fromWCharArray(module.szModule).toLower();
                if(name.contains("multimedia") || name.startsWith("avcodec") || name.startsWith("avformat") || name.contains("z2_media")) mediaModules.append(name);
            } while(Module32NextW(modules,&module));
            CloseHandle(modules); QVERIFY2(mediaModules.isEmpty(),qPrintable(mediaModules.join(',')));
        };
        measure(0);
        for(int round=1;round<=6;++round) {
            if(round==1) m_controller->importPlugin(QUrl::fromLocalFile(ZTO_UI_MEDIA_PACKAGE)); else m_controller->loadPlugin("media");
            QTRY_COMPARE_WITH_TIMEOUT(m_controller->pluginRegistry()->plugins().size(),1,80000);
            QTRY_COMPARE_WITH_TIMEOUT(m_controller->pluginRegistry()->preparationState("media"),vusiif::zto::plugins::PreparationState::Ready,80000);
            qint64 pid=0; for(const auto &v:m_controller->plugins()) if(v.toMap()["id"]=="media") pid=v.toMap()["pid"].toLongLong();
            QVERIFY(pid>0); const auto process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(pid)); QVERIFY(process);
            wchar_t image[32768]; DWORD imageSize=32768; QVERIFY(QueryFullProcessImageNameW(process,0,image,&imageSize));
            const auto cache=QFileInfo(QString::fromWCharArray(image,imageSize)).dir().absoluteFilePath("../..");
            m_controller->unloadPlugin("media"); QTRY_VERIFY_WITH_TIMEOUT(m_controller->pluginRegistry()->plugins().isEmpty(),10000);
            QCOMPARE(WaitForSingleObject(process,0),DWORD(WAIT_OBJECT_0)); CloseHandle(process);
            QVERIFY(!QFileInfo::exists(cache));
            const auto runtime=vusiif::zto::package::runtimeStatistics(); QCOMPARE(runtime["directories"].toInt(),0); QCOMPARE(runtime["handles"].toInt(),0);
            QTest::qWait(300); measure(round);
        }
        // Let Qt's default thread-pool expiry distinguish an idle worker from a leak.
        QTest::qWait(31000); measure(7);
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
#else
        QSKIP("Windows media package required");
#endif
    }
    void cleanup() {
        qunsetenv("ZT_TEST_INFO_DELAY");
        m_engine.reset(); m_window = nullptr; m_controller.reset();
        QFile::remove(m_storage.settingsFile()); qunsetenv("ZT_TEST_MODE"); qunsetenv("ZT_TEST_COMMAND_LOG");
        qunsetenv("ZT_TEST_QUERY_LOG");
        qunsetenv("ZT_TEST_PING_MODE"); qunsetenv("ZT_TEST_PING_DELAY"); qunsetenv("ZT_TEST_PING_LOG");
        qunsetenv("ZTO_PLUGIN_CREATE_STEPS");
    }
    void cleanupTestCase() { qputenv("PATH", m_path); qputenv("ProgramData", m_programData); qunsetenv("ZT_TEST_DATA_DIRECTORY"); }
    void recordUpdatesPreserveRowsAndExpansion() {
        RecordModel model;
        model.updateRows({{{"rowId","a"},{"name","A"}},{{"rowId","b"},{"name","B"}}});
        model.update("a", {{"expanded",true},{"members",QVariantList{QVariantMap{{"id","member"}}}}});
        QPersistentModelIndex index(model.index(0));
        QSignalSpy reset(&model,&QAbstractItemModel::modelReset);
        model.updateRows({{{"rowId","b"},{"name","B2"}},{{"rowId","a"},{"name","A2"}}});
        QVERIFY(index.isValid()); QCOMPARE(index.row(),1); QCOMPARE(index.data(RecordModel::RowId).toString(),QString("a"));
        QVERIFY(model.record("a")["expanded"].toBool()); QCOMPARE(model.record("a")["members"].toList().size(),1); QCOMPARE(reset.size(),0);
        model.updateRows({{{"rowId","a"},{"name","A3"}},{{"rowId","a"},{"name","duplicate"}}}); QCOMPARE(model.count(),1);
    }
    void qmlWindowLoadsBothThemesAndScales() {
        openWindow();
        QTRY_COMPARE(m_controller->networks()->count(),2);
        QVERIFY(!m_controller->peersExpanded());
        click("togglePeersButton"); QTRY_COMPARE(m_controller->peers()->count(),4);
        QTRY_COMPARE(m_controller->info()["address"].toString(),QString("0123456789"));
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-light.png"));
        m_controller->setTheme("dark"); QTest::qWait(300);
        QVERIFY(m_controller->dark()); QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-dark.png"));
        m_window->resize(760,520); QTest::qWait(100);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-small.png"));
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void applicationVersionLabelsMatchRootVersion() {
        QFile versionFile(QString::fromUtf8(ZTO_SOURCE_VERSION_FILE));
        QVERIFY(versionFile.open(QIODevice::ReadOnly));
        const auto expectedVersion = QString::fromUtf8(versionFile.readAll().trimmed());
        QVERIFY(!expectedVersion.isEmpty());
        openWindow();
        QCOMPARE(m_controller->version(), expectedVersion);
        auto *sidebarVersion = object("sidebarVersionLabel"); QVERIFY(sidebarVersion);
        QCOMPARE(sidebarVersion->property("text").toString(), "v" + expectedVersion);
        click("settingsButton"); QTest::qWait(180);
        scrollSettingsTo("aboutButton");
        click("aboutButton");
        QTRY_VERIFY(object("aboutDialog") && object("aboutDialog")->property("visible").toBool());
        auto *aboutVersion = object("aboutVersionLabel"); QVERIFY(aboutVersion);
        QCOMPARE(aboutVersion->property("text").toString(), "v" + expectedVersion);
        QCOMPARE(m_controller->serverVersion(), QStringLiteral(ZTOHUB_VERSION));
        QCOMPARE(m_controller->serverApiVersion(), QStringLiteral(ZTOHUB_API_VERSION));
        auto *aboutServerVersion = object("aboutServerVersionLabel"); QVERIFY(aboutServerVersion);
        QCOMPARE(aboutServerVersion->property("text").toString(),
                 "服务端 " + QStringLiteral(ZTOHUB_VERSION)
                     + "（API " + QStringLiteral(ZTOHUB_API_VERSION) + "）");
        QTest::qWait(180);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-application-version.png"));
        m_controller->setTheme("dark"); QTest::qWait(200);
        QCOMPARE(sidebarVersion->property("text").toString(), "v" + expectedVersion);
        QCOMPARE(aboutVersion->property("text").toString(), "v" + expectedVersion);
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void tableSortingUsesNaturalOrderAndKeepsPlaceholdersLast() {
        using vusiif::zto::ui::compareSortValues;
        QVERIFY(compareSortValues("node2", "node10", true) < 0);
        QVERIFY(compareSortValues("Alpha", "beta", true) < 0);
        QVERIFY(compareSortValues("<1 ms", "1 ms", true) < 0);
        QVERIFY(compareSortValues("9 ms", "12.5 ms", true) < 0);
        QVERIFY(compareSortValues("9 ms", "12.5 ms", false) > 0);
        QVERIFY(compareSortValues("—", "3", true) > 0);
        QVERIFY(compareSortValues("—", "3", false) > 0);
        QVERIFY(compareSortValues("-", "不可达", true) == 0);
        openWindow();
        click("togglePeersButton"); QTRY_COMPARE(m_controller->peers()->count(), 4);
        // Let the expand animation settle so the header is where it will be clicked.
        QTest::qWait(400);
        click("tableHeader-peers-4");
        QTRY_COMPARE(m_controller->peers()->sortColumn(), 4); QVERIFY(m_controller->peers()->sortAscending());
        QStringList latencies;
        for (int row = 0; row < m_controller->peers()->count(); ++row)
            latencies << m_controller->peers()->get(row)["cells"].toStringList().value(3);
        for (int row = 1; row < latencies.size(); ++row)
            QVERIFY2(compareSortValues(latencies[row - 1], latencies[row], true) <= 0, qPrintable(latencies.join(',')));
        click("tableHeader-peers-4"); QTRY_VERIFY(!m_controller->peers()->sortAscending());
        click("tableHeader-peers-0"); QTRY_COMPARE(m_controller->peers()->sortColumn(), 0);
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void themeDefaultsToSystemAndTitleBarHasWindowControls() {
        openWindow();
        QCOMPARE(m_controller->theme(), QStringLiteral("system"));
        QCOMPARE(m_controller->dark(), QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
        QVERIFY(object("serviceStatusLogo")); QVERIFY(object("beginnerGuideButton"));
#ifdef Q_OS_WIN
        QVERIFY(m_window->flags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(object("minimizeWindowButton")); QVERIFY(object("maximizeWindowButton")); QVERIFY(object("closeWindowButton"));
#endif
        click("settingsButton"); QTRY_VERIFY(object("settingsPage")); QTest::qWait(220);
        click("themeOption-light"); QTRY_COMPARE(m_controller->theme(), QStringLiteral("light")); QVERIFY(!m_controller->dark());
        click("themeOption-dark"); QTRY_COMPARE(m_controller->theme(), QStringLiteral("dark")); QVERIFY(m_controller->dark());
        click("themeOption-system"); QTRY_COMPARE(m_controller->theme(), QStringLiteral("system"));
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void themeToggleAndMoonPopupHaveUsableGeometry() {
        openWindow();
        QTRY_VERIFY(m_controller->service()["available"].toBool());
        QVERIFY(object("serviceStatusLogo"));
        click("settingsButton"); QTRY_VERIFY(object("settingsPage"));
        QTest::qWait(220); click("themeOption-dark"); QTRY_VERIFY(m_controller->dark());
        QTest::qWait(250); QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-settings-dark.png"));
        m_controller->navigate(ZeroTierMenuId,qobject_cast<QQuickItem *>(object("pluginContainer"))); QTest::qWait(180);
        const auto dir=QDir(vusiif::zto::zerotier::moonDirectory()); QVERIFY(QDir().mkpath(dir.path()));
        QFile file(dir.filePath("0000000123456789.moon")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("moon"); file.close();
        click("moonsButton"); QTRY_VERIFY(m_controller->moonsActive());
        QTRY_VERIFY(m_controller->moons()->count()>0); QTest::qWait(250);
        auto *records=qobject_cast<QQuickItem *>(object("moonRecords")); QVERIFY(records); QVERIFY(records->width()>100);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-moons-dark.png"));
        QVERIFY(QFile::remove(file.fileName()));
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void gpuAccelerationPreferenceSurvivesWindowRecreation() {
        if (QCoreApplication::arguments().contains("--disable-gpu-acceleration")) QSKIP("Command-line override is tested separately");
        openWindow();
        QVERIFY(m_controller->disableGpuAcceleration());
        const auto backend = QQuickWindow::sceneGraphBackend();
        click("settingsButton"); QTest::qWait(220);
        auto *toggle = object("disableGpuAccelerationSwitch"); QVERIFY(toggle);
        QVERIFY(toggle->property("checked").toBool()); QVERIFY(toggle->property("enabled").toBool());
        QVERIFY(!object("gpuAccelerationHint"));
        click("disableGpuAccelerationSwitch"); QTRY_VERIFY(!m_controller->disableGpuAcceleration());
        QTRY_VERIFY(object("notificationBanner") && object("notificationBanner")->property("visible").toBool());
        QVERIFY(object("notificationBanner")->property("message").toString().contains(QStringLiteral("重新启动")));
        click("dismissNotificationButton"); QTRY_VERIFY(!object("notificationBanner")->property("visible").toBool());
        QCOMPARE(QQuickWindow::sceneGraphBackend(), backend);
        m_engine.reset(); m_window = nullptr; m_controller.reset();
        openWindow();
        QVERIFY(!m_controller->disableGpuAcceleration());
        click("settingsButton"); QTest::qWait(220);
        QVERIFY(!object("disableGpuAccelerationSwitch")->property("checked").toBool());
        m_controller->setTheme("dark"); m_window->resize(720,480); QTest::qWait(300);
        auto *popup = object("settingsPage"); QVERIFY(popup);
        QVERIFY(popup->property("y").toReal() + popup->property("height").toReal() <= m_window->height());
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-gpu-settings-dark-small.png"));
        click("disableGpuAccelerationSwitch"); QTRY_VERIFY(m_controller->disableGpuAcceleration());
        QVERIFY(!object("notificationBanner") || !object("notificationBanner")->property("visible").toBool());
        m_controller->setTheme("light"); QTest::qWait(300);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-gpu-settings-light-small.png"));
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void gpuAccelerationCommandLineOverrideIsTemporary() {
        if (!QCoreApplication::arguments().contains("--disable-gpu-acceleration")) QSKIP("Run with --disable-gpu-acceleration to test override");
        openWindow();
        QVERIFY(m_controller->gpuAccelerationForcedOff());
        QVERIFY(m_controller->disableGpuAcceleration());
        click("settingsButton"); QTest::qWait(220);
        auto *toggle = object("disableGpuAccelerationSwitch"); QVERIFY(toggle);
        QVERIFY(toggle->property("checked").toBool()); QVERIFY(!toggle->property("enabled").toBool());
        click("disableGpuAccelerationSwitch");
        QVERIFY(m_controller->disableGpuAcceleration());
        QVERIFY(!hasSetting("rendering/disableGpuAcceleration"));
        QVERIFY(!object("gpuAccelerationHint"));
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void notificationsFloatWithoutResizingAndRepeatAfterDismiss() {
        prepareLocalConfigDirectory(); writeMoonFile();
        writeLocalConfig("{\"settings\":{\"tcpFallbackRelay\":\"192.0.2.10/9443\"}}");
        qputenv("ZT_TEST_MODE", "members"); openWindow();
        m_controller->refreshTcpFallbackServers(); QTRY_VERIFY(!m_controller->tcpFallbackServersBusy());
        QVERIFY(!m_window->property("footer").value<QObject *>());
        auto *page=qobject_cast<QQuickItem *>(object("zeroTierScroll")); QVERIFY(page);
        const auto pageHeight=page->height();
        auto *banner=qobject_cast<QQuickItem *>(object("notificationBanner")); QVERIFY(banner);
        m_controller->selectTcpFallbackServer("0123456789abcdef");
        QTRY_VERIFY(banner->property("shown").toBool());
        QTRY_VERIFY(banner->property("opacity").toReal()>0.99);
        QVERIFY(!banner->property("error").toBool());
        QCOMPARE(banner->property("message").toString(),QStringLiteral("当前已使用该 TCP 回退中转服务器。"));
        QCOMPARE(page->height(),pageHeight);
        const auto position=banner->mapToScene(QPointF(0,0));
        QCOMPARE(position.y(),18.0);
        QVERIFY(qAbs(position.x()+banner->width()/2-m_window->width()/2)<1);
        click("dismissNotificationButton"); QTRY_VERIFY(!banner->isVisible());
        m_controller->refresh(); QTest::qWait(250); QVERIFY(!banner->isVisible());
        m_controller->selectTcpFallbackServer("0123456789abcdef");
        QTRY_VERIFY(banner->isVisible()); // Same text is a new event.
        m_controller->joinNetwork("invalid");
        QTRY_VERIFY(banner->property("error").toBool());
        QVERIFY(banner->property("message").toString().contains(QStringLiteral("16 位")));
        QCOMPARE(page->height(),pageHeight);
        click("dismissNotificationButton"); QTRY_VERIFY(!banner->isVisible());
        // The closing overlay no longer intercepts the controls beneath it.
        click("beginnerGuideButton"); QTRY_VERIFY(object("guideOverlay")->property("visible").toBool());
        QTest::keyClick(m_window,Qt::Key_Escape);
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void notificationColorsAndLongText_data() {
        QTest::addColumn<QString>("theme"); QTest::addColumn<bool>("error");
        QTest::newRow("light-info") << QString("light") << false;
        QTest::newRow("light-error") << QString("light") << true;
        QTest::newRow("dark-info") << QString("dark") << false;
        QTest::newRow("dark-error") << QString("dark") << true;
    }
    void notificationColorsAndLongText() {
        QFETCH(QString,theme); QFETCH(bool,error);
        openWindow(); m_controller->setTheme(theme); m_window->resize(720,480);
        const auto text=QStringLiteral("很长的处理结果与路径 <test> C:/Z2/data/：请查看并继续操作。\n").repeated(30);
        m_controller->notificationRequested(text,error);
        auto *banner=qobject_cast<QQuickItem *>(object("notificationBanner")); QVERIFY(banner);
        QTRY_VERIFY(banner->property("opacity").toReal()>0.99); QTest::qWait(250);
        QVERIFY(banner->height()<=260);
        const auto position=banner->mapToScene(QPointF(0,0));
        QVERIFY(position.x()>=0 && position.y()>=0);
        QVERIFY(position.x()+banner->width()<=m_window->width());
        QVERIFY(position.y()+banner->height()<=m_window->height());
        const auto background=object("notificationBackground")->property("color").value<QColor>();
        const QColor expected(theme=="dark" ? (error?"#57363b":"#284b3b") : (error?"#fbe3e5":"#e0f3e7"));
        QCOMPARE(background,expected);
        QCOMPARE(object("notificationMessage")->property("text").toString(),text);
        QVERIFY(m_window->grabWindow().save(QString(".tmp/screenshots/notification-%1-%2.png").arg(theme,error?"error":"info")));
        click("dismissNotificationButton"); QTRY_VERIFY(!banner->isVisible());
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void busyAndFailureNotificationsDoNotReserveWindowSpace() {
        DeferredOperations operations; openWindow({},&operations);
        QTRY_VERIFY(m_controller->service()["known"].toBool());
        const auto pageHeight=object("zeroTierScroll")->property("height").toReal();
        m_controller->serviceAction("restart");
        auto *banner=object("notificationBanner"); QVERIFY(banner);
        QVERIFY(banner->property("working").toBool()); QVERIFY(banner->property("shown").toBool());
        QCOMPARE(object("zeroTierScroll")->property("height").toReal(),pageHeight);
        operations.complete(false);
        QVERIFY(!banner->property("working").toBool()); QVERIFY(banner->property("error").toBool());
        QTRY_VERIFY(banner->property("opacity").toReal()>0.99);
        QCOMPARE(object("zeroTierScroll")->property("height").toReal(),pageHeight);
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void nodeCopyFeedbackRestartsAndRejectsPlaceholder() {
        qputenv("ZT_TEST_MODE", "invalid-id");
        openWindow(); QTRY_COMPARE(m_controller->info()["address"].toString(), QStringLiteral("—"));
        QGuiApplication::clipboard()->setText("keep");
        click("nodeId"); QTest::qWait(180);
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("keep"));
        QVERIFY(!object("notificationBanner")->property("visible").toBool());
        qputenv("ZT_TEST_MODE", "compact-lists"); m_controller->refresh();
        QTRY_COMPARE(m_controller->info()["address"].toString(), QString("0123456789"));
        auto *id = qobject_cast<QQuickItem *>(object("nodeId")); QVERIFY(id);
        QTest::mouseMove(m_window, id->mapToScene(QPointF(10, 10)).toPoint());
        QTRY_VERIFY(id->property("font").value<QFont>().underline());
        click("nodeId"); QTRY_VERIFY(object("notificationBanner")->property("visible").toBool());
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("0123456789"));
        auto *toast = object("notificationBanner");
        QTest::qWait(900); click("nodeId"); QTest::qWait(700);
        QCOMPARE(object("notificationBanner"), toast); QVERIFY(toast->property("opacity").toReal() > 0.95);
        QTRY_VERIFY(!toast->property("visible").toBool());
        QTest::mouseClick(m_window, Qt::RightButton, {}, id->mapToScene(QPointF(10,10)).toPoint());
        QTest::qWait(180); QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void destructiveServiceOperationInvalidatesOldQueries_data() {
        QTest::addColumn<QString>("action"); QTest::addColumn<bool>("success");
        QTest::newRow("stop") << QString("stop") << true;
        QTest::newRow("restart") << QString("restart") << true;
        QTest::newRow("authorization-cancelled") << QString("restart") << false;
    }
    void destructiveServiceOperationInvalidatesOldQueries() {
        QFETCH(QString, action); QFETCH(bool, success);
        DeferredOperations operations;
        openWindow({}, &operations);
        QTRY_VERIFY(m_controller->service()["available"].toBool());
        QTRY_VERIFY(m_controller->service()["known"].toBool());
        const auto path = m_directory.filePath("interruption-queries.log"); QFile::remove(path);
        qputenv("ZT_TEST_QUERY_LOG", path.toUtf8()); qputenv("ZT_TEST_INFO_DELAY", "1800");
        const auto queried = [&] { QFile f(path); return f.open(QIODevice::ReadOnly) && f.readAll().contains("info"); };
        m_controller->refresh(); QTRY_VERIFY(queried());
        m_controller->serviceAction("invalid"); QVERIFY(m_controller->service()["available"].toBool());
        m_controller->serviceAction(action);
        QVERIFY(m_controller->busy()); QVERIFY(!m_controller->service()["available"].toBool());
        QVERIFY(bool(operations.completion));
        QTest::qWait(2000); QVERIFY(!m_controller->service()["available"].toBool());
        QVERIFY(!object("sidebarToggleButton")->property("online").toBool()); QVERIFY(object("serviceStatusLogo")->property("source").toUrl().toString().endsWith("logo-offline.png"));
        qputenv("ZT_TEST_MODE", "error"); operations.complete(success);
        QVERIFY(!m_controller->busy()); QTest::qWait(1000);
        QVERIFY(!m_controller->service()["available"].toBool());
        qunsetenv("ZT_TEST_INFO_DELAY"); qputenv("ZT_TEST_MODE", "compact-lists"); m_controller->refresh();
        QTRY_VERIFY(m_controller->service()["available"].toBool());
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void uninstallCancellationAndPreflightPreserveAvailability() {
        DeferredOperations operations; openWindow({}, &operations);
        QTRY_VERIFY(m_controller->service()["available"].toBool());
        QSignalSpy confirmation(m_controller.get(), &AppController::confirmationRequested);
        const auto path = qgetenv("PATH"); qputenv("PATH", "");
        m_controller->packageAction(true); qputenv("PATH", path);
#ifndef Q_OS_MACOS
        QCOMPARE(confirmation.size(), 0); QVERIFY(!operations.completion);
        QVERIFY(m_controller->service()["available"].toBool());
#endif
        m_controller->packageAction(true);
        if (confirmation.isEmpty()) QSKIP("winget unavailable; preflight verified");
        QVERIFY(m_controller->service()["available"].toBool());
        m_controller->confirm(false); QVERIFY(!operations.completion);
        QVERIFY(m_controller->service()["available"].toBool());
        m_controller->packageAction(true); m_controller->confirm(true);
        QVERIFY(bool(operations.completion)); QVERIFY(!m_controller->service()["available"].toBool());
        operations.complete(false); QTRY_VERIFY(m_controller->service()["available"].toBool());
    }
    void globalMembersDefaultCollapsedAndRefreshKeepsEditor() {
        const auto path=m_directory.filePath("peer-queries.log"); QFile::remove(path);
        qputenv("ZT_TEST_QUERY_LOG", path.toUtf8()); openWindow();
        QTRY_COMPARE(m_controller->networks()->count(),2);
        QVERIFY(!m_controller->peersExpanded()); QCOMPARE(m_controller->peers()->count(),0);
        for (int i=0; i<m_controller->networks()->count(); ++i) QVERIFY(!m_controller->networks()->get(i)["expanded"].toBool());
        auto queries=[&] { QFile f(path); return f.open(QIODevice::ReadOnly)? f.readAll():QByteArray(); };
        QTest::qWait(1200); QVERIFY(!queries().contains("listpeers"));
        click("togglePeersButton"); QTRY_COMPARE(m_controller->peers()->count(),4);
        auto *peers=object("peers"); QVERIFY(peers);
        QVERIFY(QMetaObject::invokeMethod(peers,"edit",Q_ARG(QVariant,"0000000001"),Q_ARG(QVariant,"editing")));
        QTRY_VERIFY(object("remarkEditor")); QPointer<QObject> editor=object("remarkEditor");
        m_controller->refresh(); QTest::qWait(1200);
        QVERIFY(m_controller->peersExpanded()); QCOMPARE(object("remarkEditor"),editor.data());
        QCOMPARE(editor->property("text").toString(),QString("editing"));
        QTest::keyClick(m_window,Qt::Key_Escape);
        m_controller->setPeersExpanded(false); QTest::qWait(200);
        const auto collapsed=queries(); QTest::qWait(1200); QCOMPARE(queries().count("listpeers"),collapsed.count("listpeers"));
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void infoPollingDebouncesFailuresAndRecovers() {
        openWindow(); QTRY_VERIFY(m_controller->service()["available"].toBool());
        qputenv("ZT_TEST_MODE","error"); m_controller->refresh(); QTest::qWait(1500);
        QVERIFY(m_controller->service()["available"].toBool());
        QVERIFY(!m_controller->service()["warning"].toString().contains(QStringLiteral("接口未响应")));
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->service()["available"].toBool(),8000);
        qputenv("ZT_TEST_MODE","compact-lists"); m_controller->refresh();
        QTRY_VERIFY(m_controller->service()["available"].toBool());
    }
    void timeoutCountsEachRequestOnce() {
        qputenv("ZT_TEST_MODE","slow");
        vusiif::zto::zerotier::JsonPoller poller(this,"info",true);
        QSignalSpy availability(&poller,&vusiif::zto::zerotier::JsonPoller::availabilityChanged);
        poller.setActive(true);
        QTest::qWait(11500); QCOMPARE(availability.size(),0);
        QTRY_VERIFY_WITH_TIMEOUT(!availability.isEmpty(),9000);
        QCOMPARE(availability.first().first().toBool(),false);
        qputenv("ZT_TEST_MODE","compact-lists");
        // A slow request can already be in flight when the fixture switches.
        // Allow its five-second deadline plus the next polling interval.
        QTRY_VERIFY_WITH_TIMEOUT(availability.last().first().toBool(),7000);
    }
    void joinDialogUsesAsyncCommandAndReportsFailure() {
        openWindow(); click("joinNetworkButton");
        auto *dialog=object("joinNetworkDialog"); QVERIFY2(dialog,qPrintable(m_errors.join('\n')));
        QTRY_VERIFY(dialog->property("visible").toBool());
        auto *input=object("networkIdInput"); QVERIFY(input); input->setProperty("text","invalid");
        click("confirmJoinButton"); QVERIFY(m_controller->feedback().contains(QStringLiteral("16 位")));
        qputenv("ZT_TEST_MODE","join-error"); input->setProperty("text","0123456789abcdef");
        click("confirmJoinButton"); QTRY_VERIFY(!m_controller->busy()); QVERIFY(m_controller->feedback().contains("join failed"));
        QVERIFY(dialog->property("visible").toBool());
        qputenv("ZT_TEST_MODE","compact-lists"); click("confirmJoinButton"); QTRY_VERIFY(!dialog->property("visible").toBool());
    }
    void pollingSurvivesFailuresAndRetainsRemarks() {
        openWindow(); QTRY_COMPARE(m_controller->networks()->count(),2);
        const auto id=m_controller->networks()->get(0)["rowId"].toString();
        m_controller->rename("networks",id,QStringLiteral("本机备注"));
        m_controller->setExpanded(id,true);
        m_controller->refresh(); QTest::qWait(100);
        QCOMPARE(m_controller->networks()->record(id)["name"].toString(),QStringLiteral("本机备注"));
        QVERIFY(m_controller->networks()->record(id)["expanded"].toBool());
        // The offline debounce itself takes at least five seconds; allow one
        // polling interval and native process scheduling before declaring failure.
        qputenv("ZT_TEST_MODE","error"); m_controller->refresh(); QTRY_VERIFY_WITH_TIMEOUT(!m_controller->networkStatus().isEmpty(),8000);
        QCOMPARE(m_controller->networks()->count(),2);
        qputenv("ZT_TEST_MODE","compact-lists"); m_controller->refresh(); QTRY_VERIFY(m_controller->networkStatus().isEmpty());
        m_controller->rename("networks",id,""); QCOMPARE(m_controller->networks()->record(id)["name"].toString(),QStringLiteral("团队网络"));
    }
    void pluginPagesUseQuickAndValidateOwnership() {
        QQmlEngine engine; QQuickItem parent;
        vusiif::zto::plugins::PluginRegistry registry;
        int creations=0;
        MenuContribution menu; menu.id="overview"; menu.title="Test";
        menu.createPage=[&](QQmlEngine *,QQuickItem *owner){ ++creations; return new QQuickItem(owner); };
        QVERIFY(registry.registerPlugin({{"test-quick","Quick","1"},{menu}}).ok());
        auto page=registry.page("test-quick/overview",&engine,&parent);
        QVERIFY(page.result.ok()); QVERIFY(page.page); QCOMPARE(page.page->parentItem(),&parent); QCOMPARE(page.page->parent(),&parent);
        QCOMPARE(registry.page("test-quick/overview",&engine,&parent).page,page.page); QCOMPARE(creations,1);
        QPointer<QQuickItem> alive=page.page; QVERIFY(registry.unregisterPlugin("test-quick").ok()); QVERIFY(alive.isNull());
        auto *borrowed=new QQuickItem(&parent); menu.createPage=[borrowed](QQmlEngine *,QQuickItem *) { return borrowed; };
        QVERIFY(registry.registerPlugin({{"test-borrowed","Borrowed","1"},{menu}}).ok());
        QCOMPARE(registry.page("test-borrowed/overview",&engine,&parent).result.error,Error::PageCreationFailed); QVERIFY(borrowed->parent()==&parent);
    }
    void storageSectionShowsPathsAndCleansOrphanData() {
        openWindow();
        const auto orphan = m_storage.pluginDataDirectory("orphan-plugin");
        QVERIFY(QDir().mkpath(orphan));
        QFile payload(QDir(orphan).filePath("config.json"));
        QVERIFY(payload.open(QIODevice::WriteOnly));
        payload.write("{\"value\":1}");
        payload.close();
        m_controller->refreshStorageUsage();
        // Directory walks and orphan classification run on a worker thread.
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->storageScanBusy(), 10000);
        QCOMPARE(m_controller->storageUsage().value("mode").toString(), QStringLiteral("shared"));
        QVERIFY(m_controller->storageUsage().value("pluginDataBytes").toDouble() > 0);
        QVERIFY(m_controller->storageUsage().contains("previewText"));
        QVERIFY(m_controller->storageUsage().contains("orphansText"));
        click("settingsButton");
        scrollSettingsTo("storageDetailsButton"); click("storageDetailsButton");
        QTRY_VERIFY(object("storageDetailsPopup")->property("visible").toBool());
        QVERIFY(object("storagePathLabel")->property("text").toString().contains(m_storage.activeRoot()));
        QVERIFY(object("cleanupLogsButton"));
        QVERIFY(object("cleanupCacheButton"));
        QVERIFY(object("cleanupOrphansButton"));
        QTest::keyClick(m_window, Qt::Key_Escape); QTest::qWait(150);
        const auto orphans = m_controller->storageOrphans();
        QVERIFY(std::any_of(orphans.cbegin(), orphans.cend(), [](const QVariant &value) {
            return value.toMap().value("id").toString() == "orphan-plugin"
                && value.toMap().value("deletable").toBool();
        }));
        QSignalSpy confirmation(&*m_controller, &AppController::confirmationRequested);
        m_controller->cleanupStorage("orphans");
        QTRY_COMPARE(confirmation.size(), 1);
        m_controller->confirm(true);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(orphan), 10000);
        QVERIFY(QFileInfo::exists(m_storage.metadataDirectory() + "/plugins/orphan-plugin.json"));
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void storageScanIsAsynchronousAndPreviewsPendingCleanup() {
        openWindow();
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->storageScanBusy(), 10000);
        const auto logs = m_storage.logsDirectory();
        QVERIFY(QDir().mkpath(logs));
        const auto oldLog = QDir(logs).filePath("250101-1.log");
        {
            QFile file(oldLog);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("old log");
            file.close();
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::currentDateTime().addDays(-31),
                                     QFileDevice::FileModificationTime));
        }
        const auto cache = QDir(m_storage.pluginDataDirectory("example")).filePath("cache");
        QVERIFY(QDir().mkpath(cache));
        const auto oldCache = QDir(cache).filePath("old.bin");
        {
            QFile file(oldCache);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("old cache");
            file.close();
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(QDateTime::currentDateTime().addDays(-8),
                                     QFileDevice::FileModificationTime));
        }
        const auto bulk = QDir(m_storage.pluginDataDirectory("bulk")).filePath("nested");
        QVERIFY(QDir().mkpath(bulk));
        for (int i = 0; i < 400; ++i) {
            QFile file(QDir(bulk).filePath(QStringLiteral("item-%1.bin").arg(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray(128, 'x'));
        }
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(1);
        connect(&heartbeat, &QTimer::timeout, this, [&ticks] { ++ticks; });
        heartbeat.start();
        QSignalSpy scanned(&*m_controller, &AppController::storageScanFinished);
        QElapsedTimer elapsed;
        elapsed.start();
        m_controller->refreshStorageUsage();
        // The call only starts worker work; it must not walk the tree.
        QVERIFY2(elapsed.elapsed() < 100, qPrintable(QString::number(elapsed.elapsed())));
        QVERIFY(m_controller->storageScanBusy());
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->storageScanBusy(), 10000);
        QVERIFY(scanned.size() >= 1);
        QVERIFY(ticks > 0);
        const auto usage = m_controller->storageUsage();
        QCOMPARE(usage.value("previewLogFiles").toInt(), 1);
        QVERIFY(usage.value("previewLogBytes").toDouble() > 0);
        QCOMPARE(usage.value("previewCacheFiles").toInt(), 1);
        QVERIFY(usage.value("previewCacheBytes").toDouble() > 0);
        QVERIFY(usage.value("previewText").toString().contains(QStringLiteral("待清理")));
        QVERIFY(usage.value("orphansText").toString().contains(QStringLiteral("bulk")));

        QSignalSpy confirmation(&*m_controller, &AppController::confirmationRequested);
        m_controller->cleanupStorage("logs");
        QTRY_COMPARE(confirmation.size(), 1);
        QVERIFY(confirmation.last().at(1).toString().contains(QStringLiteral("历史日志")));
        m_controller->confirm(true);
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->storageBusy(), 10000);
        QVERIFY(!QFileInfo::exists(oldLog));
        // A scan is scheduled after cleanup; the next cleanup request waits for
        // it and then shows its own confirmation.
        m_controller->cleanupStorage("cache");
        QTRY_COMPARE(confirmation.size(), 2);
        m_controller->confirm(true);
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->storageBusy(), 10000);
        QVERIFY(!QFileInfo::exists(oldCache));
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }

    void settingsIsMenuAndTooltipWaitsBeforeShowing() {
        openWindow(); auto *container=qobject_cast<QQuickItem *>(object("pluginContainer")); QVERIFY(container);
        QVERIFY(m_controller->navigate(SettingsMenuId,container)); QCOMPARE(m_controller->activeMenu(),QString(SettingsMenuId));
        QVERIFY(object("settingsPage")->property("visible").toBool()); QVERIFY(!object("zeroTierScroll")->property("visible").toBool());
        for(const auto &value:m_controller->menus()) QVERIFY(value.toMap()["id"].toString()!=QString(SettingsMenuId));
        auto *gear=qobject_cast<QQuickItem *>(object("settingsButton")); QVERIFY(gear);
        // A previous test may leave the pointer on the same hit item; move away
        // first so the hover-enter event actually starts the tooltip delay.
        QTest::mouseMove(m_window,QPoint(2,2)); QTest::qWait(120);
        QTest::mouseMove(m_window,gear->mapToScene(QPointF(gear->width()/2,gear->height()/2)).toPoint());
        auto *tip=object("settingsTooltip"); QVERIFY(tip); QTest::qWait(1000); QVERIFY(!tip->property("visible").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(tip->property("visible").toBool(),4000);
        QTest::mouseMove(m_window,QPoint(m_window->width()-20,20)); QTRY_VERIFY(!tip->property("visible").toBool());
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void navigationCreatesQuickPagesLazily() {
        openWindow(); int created=0; MenuContribution menu; menu.id="page"; menu.title="Plugin";
        menu.createPage=[&](QQmlEngine *,QQuickItem *owner) { ++created; auto *item=new QQuickItem(owner); item->setObjectName("testPluginPage"); return item; };
        QVERIFY(m_controller->pluginRegistry()->registerPlugin({{"test-page","Page","1"},{menu}}).ok());
        QCOMPARE(created,0); auto *container=qobject_cast<QQuickItem *>(object("pluginContainer")); QVERIFY(container);
        QVERIFY(m_controller->navigate("test-page/page",container)); QCOMPARE(created,1); QVERIFY(object("testPluginPage"));
        QVERIFY(m_controller->navigate("builtin/zerotier",container)); QVERIFY(!object("testPluginPage")->property("visible").toBool());
        QVERIFY(m_controller->navigate("test-page/page",container)); QCOMPARE(created,1);
        QVERIFY(m_controller->pluginRegistry()->unregisterPlugin("test-page").ok()); QCOMPARE(m_controller->activeMenu(),QString("builtin/zerotier"));
    }
    void pluginReloadKeepsOtherPagesInteractiveUntilReady() {
        QTemporaryDir plugins;
        QVERIFY(QFile::copy(FIXTURE_CANCELLABLE,plugins.filePath("test.z2plugin")));
        qputenv("ZTO_PLUGIN_CREATE_STEPS","100");
        openWindow(plugins.path());
        const QString menuId="test-cancellable/overview";
        const auto ready=[&] {
            for(const auto &value:m_controller->menus()) {
                const auto row=value.toMap();
                if(row["id"].toString()==menuId) return row["ready"].toBool();
            }
            return false;
        };
        QTRY_VERIFY(ready());
        m_controller->unloadPlugin("test-cancellable");
        QTRY_VERIFY(!m_controller->plugins().first().toMap()["loaded"].toBool());
        m_controller->loadPlugin("test-cancellable");
        QVERIFY(m_controller->plugins().first().toMap()["loading"].toBool());
        QVERIFY(!m_controller->busy()); QVERIFY(!ready());
        QTRY_VERIFY(object("navigation-test-cancellable/overview"));
        QVERIFY(!object("navigation-test-cancellable/overview")->property("enabled").toBool());
        auto *container=qobject_cast<QQuickItem *>(object("pluginContainer")); QVERIFY(container);
        QVERIFY(!m_controller->navigate(menuId,container));
        click("settingsButton"); QTRY_COMPARE(m_controller->activeMenu(),QString(SettingsMenuId));
        QVERIFY(object("settingsPage")->property("visible").toBool());
        m_controller->setTheme("dark"); QVERIFY(m_controller->dark());
        QTRY_VERIFY(ready());
        QTRY_VERIFY(object("navigation-test-cancellable/overview")->property("enabled").toBool());
        QVERIFY(!m_controller->plugins().first().toMap()["loading"].toBool());
        click("navigation-test-cancellable/overview"); QTRY_COMPARE(m_controller->activeMenu(),menuId);
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void confirmationCancelsWithoutMutation() {
        DeferredOperations operations; AppController controller(nullptr,{},&operations,m_storage);
        const auto dir=QDir(vusiif::zto::zerotier::moonDirectory()); QVERIFY(QDir().mkpath(dir.path()));
        QFile file(dir.filePath("0000000123456789.moon")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("moon"); file.close();
        controller.refresh(); QTRY_VERIFY(controller.moons()->count()>0); QSignalSpy confirmation(&controller,&AppController::confirmationRequested);
        controller.deleteMoon("0000000123456789"); QCOMPARE(confirmation.size(),1); controller.confirm(false); QVERIFY(operations.script.isEmpty());
        controller.deleteMoon("0000000123456789"); controller.confirm(true); QVERIFY(!operations.script.isEmpty()); QVERIFY(controller.busy());
        operations.complete(false); QVERIFY(!controller.busy()); QVERIFY(QFileInfo::exists(file.fileName())); QVERIFY(QFile::remove(file.fileName()));
    }
    void closingWaitsForOperationsAndKeepsEventLoopAlive() {
        DeferredOperations operations; AppController controller(nullptr,{},&operations,m_storage);
        QFile source(m_directory.filePath("test.moon")); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("moon"); source.close();
        controller.importMoons({QUrl::fromLocalFile(source.fileName())}); QVERIFY(controller.busy());
        QSignalSpy exited(&controller,&AppController::exitFinished);
        controller.requestQuit(); controller.requestQuit(); QTest::qWait(100); QCOMPARE(exited.size(),0);
        operations.complete(true); QTRY_COMPARE(exited.size(),1);
    }
    void emptyListsAndGuideRemainUsable() {
        qputenv("ZT_TEST_MODE","empty"); openWindow(); QTRY_COMPARE(m_controller->networks()->count(),0);
        click("beginnerGuideButton"); QTRY_VERIFY(object("guideOverlay")->property("visible").toBool());
        m_window->resize(760,520); m_controller->setTheme("dark"); QTest::qWait(80);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-guide.png"));
        QTest::keyClick(m_window,Qt::Key_Escape); QTRY_VERIFY(!object("guideOverlay")->property("visible").toBool());
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void memberPollingRetainsEditorAndCancelsOnCollapse() {
        qputenv("ZT_TEST_MODE","members"); QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,18100));
        qputenv("ZT_TEST_PING_DELAY", "700");
        const auto latencyAt = [this](int index) {
            return m_controller->networks()->record("0123456789abcdef")["members"].toList().value(index).toMap()["latency"].toString();
        };
        int requests=0;
        connect(&server,&QTcpServer::newConnection,this,[&] {
            auto *socket=server.nextPendingConnection(); auto input=std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::readyRead,socket,[&,socket,input] {
                *input+=socket->readAll(); vusiif::zto::protocol::Packet packet;
                if (vusiif::zto::protocol::takeFrame(*input,packet)!=vusiif::zto::protocol::ReadResult::Packet) return;
                const auto request=QJsonDocument::fromJson(packet.payload).object(); if(request["data"].toObject()["purpose"]=="ui-members") ++requests;
                const QJsonArray members{
                    QJsonObject{{"nodeId","012345678b"},{"ipAssignments",QJsonArray{"12.34.56.100"}}},
                    QJsonObject{{"nodeId","012345678a"},{"ipAssignments",QJsonArray{"12.34.56.10"}}},
                    QJsonObject{{"nodeId","0123456789"},{"authorized",true},{"ipAssignments",QJsonArray{requests==1?"12.34.56.2":"12.34.56.3"}}}};
                socket->write(vusiif::zto::protocol::frame("builtin",QJsonDocument(QJsonObject{{"v",1},{"id",request["id"]},{"ok",true},{"data",members}}).toJson(QJsonDocument::Compact)));
            });
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        });
        openWindow(); QTRY_COMPARE(m_controller->networks()->count(),1);
        m_controller->setExpanded("0123456789abcdef",true);
        QTRY_VERIFY(requests>=1);
        QTRY_VERIFY(!m_controller->networks()->record("0123456789abcdef")["members"].toList().isEmpty());
        const auto sorted=m_controller->networks()->record("0123456789abcdef")["members"].toList();
        QCOMPARE(sorted.size(),3); QCOMPARE(sorted[0].toMap()["id"].toString(),QString("0123456789"));
        QCOMPARE(sorted[1].toMap()["ip"].toString(),QString("12.34.56.10"));
        QCOMPARE(sorted[2].toMap()["ip"].toString(),QString("12.34.56.100"));
        QTRY_COMPARE(latencyAt(0), QString("12 ms"));
        auto previous = m_controller->networks()->record("0123456789abcdef")["members"].toList();
        auto successful = previous[0].toMap(); successful["latency"] = "37 ms"; previous[0] = successful;
        auto failed = previous[1].toMap(); failed["latency"] = QStringLiteral("-"); previous[1] = failed;
        m_controller->networks()->update("0123456789abcdef", {{"members", previous}});
        QTest::qWait(1500); QCOMPARE(requests, 1); // No old two-second polling cycle.
        QTest::qWait(250); m_controller->setTheme("dark"); QTest::qWait(350);
        auto *memberLine=qobject_cast<QQuickItem *>(object("networkMemberRow")); QVERIFY(memberLine);
        QTest::mouseClick(m_window,Qt::RightButton,{},memberLine->mapToScene(QPointF(100,20)).toPoint());
        QTRY_VERIFY(object("networkMemberMenu")); QTest::qWait(200);
        QVERIFY(object("networkMemberMenu")->property("width").toReal()<200);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-member-menu-dark.png"));
        QTest::keyClick(m_window,Qt::Key_Escape); QTest::qWait(150);
        QTRY_VERIFY(object("networkMemberRow")); auto *member=object("networkMemberRow"); member->setProperty("editing",true);
        auto *editor=object("networkMemberRemarkEditor"); QVERIFY(editor); QTRY_VERIFY(editor->property("visible").toBool()); editor->setProperty("text",QStringLiteral("成员备注"));
        QPointer<QObject> identity=editor;
        QTRY_VERIFY_WITH_TIMEOUT(requests>=2,6000);
        QTRY_COMPARE(m_controller->networks()->record("0123456789abcdef")["members"].toList().first().toMap()["ip"].toString(), QString("12.34.56.3"));
        QCOMPARE(latencyAt(0), QString("37 ms"));
        QCOMPARE(latencyAt(1), QStringLiteral("-"));
        QCOMPARE(object("networkMemberRemarkEditor"),identity.data()); QCOMPARE(editor->property("text").toString(),QStringLiteral("成员备注"));
        QTRY_COMPARE(latencyAt(0), QString("12 ms"));
        QTRY_COMPARE(latencyAt(1), QString("12 ms"));
        QTest::keyClick(m_window,Qt::Key_Return);
        QTRY_COMPARE(setting("names/peers/0123456789").toString(),QStringLiteral("成员备注"));
        qputenv("ZT_TEST_PING_MODE", "hang");
        const auto pingProcesses = [this] {
            QList<QPointer<QProcess>> result;
            for (auto *process : m_controller->findChildren<QProcess *>())
                if (QFileInfo(process->program()).baseName() == "ping" && process->state() == QProcess::Running) result.append(process);
            return result;
        };
        m_controller->refresh(); QTRY_COMPARE(pingProcesses().size(), 3);
        const auto pending = pingProcesses(); const int previousRequests = requests;
        m_controller->refresh(); QTRY_VERIFY(requests > previousRequests);
        QCOMPARE(pingProcesses().size(), 3);
        m_controller->setExpanded("0123456789abcdef",false); const int before=requests; QTest::qWait(5100); QCOMPARE(requests,before);
        for (const auto &process : pending) QVERIFY(process.isNull());
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void closingDuringInitializationRemainsResponsive_data() {
        QTest::addColumn<QString>("library");
        QTest::newRow("cooperative")<<QString::fromUtf8(FIXTURE_CANCELLABLE);
        QTest::newRow("bounded-call")<<QString::fromUtf8(FIXTURE_CANCELLABLE_BLOCKED);
    }
    void closingDuringInitializationRemainsResponsive() {
        QFETCH(QString,library); QTemporaryDir directory; QVERIFY(QFile::copy(library,directory.filePath(QFileInfo(library).fileName())));
        const auto marker=directory.filePath("started"); qputenv("ZTO_PLUGIN_PREPARATION_STARTED_FILE",marker.toUtf8()); qputenv("ZTO_PLUGIN_CREATE_STEPS","1000");
        openWindow(directory.path()); QTRY_VERIFY(QFileInfo::exists(marker));
        QSignalSpy exited(m_controller.get(),&AppController::exitFinished);
        int heartbeats=0; QTimer timer; connect(&timer,&QTimer::timeout,this,[&] { ++heartbeats; }); timer.start(10);
        QElapsedTimer elapsed; elapsed.start(); m_controller->requestQuit(); QVERIFY(elapsed.elapsed()<50); QTRY_COMPARE_WITH_TIMEOUT(exited.size(),1,2000); if(library==QString::fromUtf8(FIXTURE_CANCELLABLE_BLOCKED)) QVERIFY(heartbeats>0); QVERIFY(m_controller->closing());
        qunsetenv("ZTO_PLUGIN_PREPARATION_STARTED_FILE"); qunsetenv("ZTO_PLUGIN_CREATE_STEPS");
        m_engine.reset(); m_window=nullptr; m_controller.reset();
    }
    void manualColumnsPersistAndDetailsRemainNonModal() {
        openWindow(); QTRY_COMPARE(m_controller->networks()->count(),2);
        auto *records=object("networks"); QVERIFY(records);
        QVERIFY(QMetaObject::invokeMethod(records,"resizeColumn",Q_ARG(QVariant,0),Q_ARG(QVariant,80)));
        const auto widths=records->property("columnWidths").value<QJSValue>().toVariant().toList();
        QVERIFY(!widths.isEmpty()); QCOMPARE(widths.first().toInt(),260);
        const auto id=m_controller->networks()->get(0)["rowId"].toString();
        QVERIFY(m_controller->detailsJson("networks",id).contains("mtu")); QVERIFY(m_controller->detailsJson("networks",id).contains("routes"));
        QVERIFY(QMetaObject::invokeMethod(m_window,"showDetails",Q_ARG(QVariant,"networks"),Q_ARG(QVariant,id),Q_ARG(QVariant,"Network")));
        QTRY_VERIFY(object("detailsPopup")->property("visible").toBool()); QVERIFY(!object("detailsPopup")->property("modal").toBool());
        m_engine.reset(); m_window=nullptr; m_controller.reset(); openWindow();
        const auto restored=object("networks")->property("columnWidths").value<QJSValue>().toVariant().toList(); QCOMPARE(restored.first().toInt(),260);
    }
    void failedHostSettingsMigrationBlocksQmlPersistence()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto shared = directory.filePath("application");
        const auto personalBase = directory.filePath("user-data");
        QVERIFY(QDir().mkpath(shared));
        const auto source = QDir(shared).filePath("settings.ini");
        {
            QSettings original(source, QSettings::IniFormat);
            original.setValue("appearance/theme", QStringLiteral("dark"));
            original.setValue("names/networks/test", QStringLiteral("Original Name"));
            original.sync();
        }
        const auto routeDirectory = QDir(personalBase).filePath(
            QStringLiteral("installations/") + vusiif::zto::storage::StorageContext::installKey(shared));
        QVERIFY(QDir().mkpath(routeDirectory));
        {
            QFile route(QDir(routeDirectory).filePath("route.json"));
            QVERIFY(route.open(QIODevice::WriteOnly));
            route.write(QJsonDocument(QJsonObject{{"format", 1}, {"mode", "personal"}}).toJson());
        }
        // Blocking the source read makes only the host INI item fail, just like
        // an unreadable shared settings file at startup.
        const HANDLE sourceHandle = CreateFileW(reinterpret_cast<LPCWSTR>(source.utf16()),
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(sourceHandle != INVALID_HANDLE_VALUE);
        auto storage = vusiif::zto::storage::StorageContext::resolveForTesting(shared, personalBase);
        QCOMPARE(storage.mode(), vusiif::zto::storage::StorageContext::Mode::Personal);
        QString storageError;
        QVERIFY2(storage.prepare(storageError), qPrintable(storageError));
        QVERIFY(storage.settingsMigrationPending());
        const auto target = storage.settingsFile();
        QVERIFY(!QFileInfo::exists(target));
        {
            const auto pluginDirectory = directory.filePath("plugins");
            auto controller = std::make_unique<AppController>(nullptr, pluginDirectory, nullptr, storage);
            QVERIFY(!controller->settingsWritable());
            auto engine = std::make_unique<QQmlApplicationEngine>();
            QStringList errors;
            connect(engine.get(), &QQmlEngine::warnings, this, [&errors](const QList<QQmlError> &list) {
                for (const auto &error : list) errors.append(error.toString());
            });
            engine->setInitialProperties({{"backend", QVariant::fromValue(controller.get())}});
            engine->load(QUrl("qrc:/qt/qml/ZTOrbit/qml/Main.qml"));
            QVERIFY2(!engine->rootObjects().isEmpty(), qPrintable(errors.join('\n')));
            auto *window = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
            QVERIFY(window);
            controller->setWindow(window);
            QTest::qWait(100);
            window->resize(1200, 820);
            // The record tables persist their column widths through the same
            // host store, so that write path is exercised under the gate too.
            m_window = window;
            auto *records = object("networks");
            m_window = nullptr;
            QVERIFY(records);
            QVERIFY(QMetaObject::invokeMethod(records, "resizeColumn", Q_ARG(QVariant, 0), Q_ARG(QVariant, 80)));
            QTest::qWait(100);
            // Destroying the window saves its size through the backend; a QML
            // Settings element would have created the INI with defaults on load.
            engine.reset();
            QTest::qWait(100);
            controller.reset();
            QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        }
        // Neither window creation nor column or geometry persistence created
        // the target of the still pending host settings copy.
        QVERIFY(!QFileInfo::exists(target));
        CloseHandle(sourceHandle);
        // The restart restores the shared settings and QML persistence resumes.
        auto retry = vusiif::zto::storage::StorageContext::resolveForTesting(shared, personalBase);
        QVERIFY2(retry.prepare(storageError), qPrintable(storageError));
        QVERIFY(retry.migrationComplete());
        QVERIFY(!retry.settingsMigrationPending());
        {
            vusiif::zto::storage::SettingsStore recovered(retry.settingsFile());
            QCOMPARE(recovered.value("appearance/theme").toString(), QStringLiteral("dark"));
            QCOMPARE(recovered.value("names/networks/test").toString(), QStringLiteral("Original Name"));
        }
        {
            const auto pluginDirectory = directory.filePath("plugins");
            auto controller = std::make_unique<AppController>(nullptr, pluginDirectory, nullptr, retry);
            QVERIFY(controller->settingsWritable());
            auto engine = std::make_unique<QQmlApplicationEngine>();
            QStringList errors;
            connect(engine.get(), &QQmlEngine::warnings, this, [&errors](const QList<QQmlError> &list) {
                for (const auto &error : list) errors.append(error.toString());
            });
            engine->setInitialProperties({{"backend", QVariant::fromValue(controller.get())}});
            engine->load(QUrl("qrc:/qt/qml/ZTOrbit/qml/Main.qml"));
            QVERIFY2(!engine->rootObjects().isEmpty(), qPrintable(errors.join('\n')));
            auto *window = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
            QVERIFY(window);
            controller->setWindow(window);
            QTest::qWait(100);
            window->resize(1180, 800);
            QTest::qWait(100);
            engine.reset();
            QTest::qWait(100);
            controller.reset();
            QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        }
        vusiif::zto::storage::SettingsStore saved(retry.settingsFile());
        // The recovered values survive the new QML persistence writes.
        QCOMPARE(saved.value("appearance/theme").toString(), QStringLiteral("dark"));
        QVERIFY(saved.value("window/qml/savedWidth").toInt() >= 720);
#else
        QSKIP("exclusive read handles keep the migration source unreadable on Windows only");
#endif
    }

    void windowsCaptionTrayAndRestore() {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName()=="offscreen") QSKIP("Native Windows tray requires the windows platform");
        openWindow(); QSignalSpy requested(m_controller.get(),&AppController::trayRequested);
        auto *tray=object("systemTrayIcon"); QVERIFY(tray);
        QVERIFY(tray->property("visible").toBool());
        QVERIFY(tray->property("available").toBool());
        QVERIFY(!QIcon(":/qt/qml/ZTOrbit/assets/logo-online.png").isNull()); QVERIFY(!QIcon(":/qt/qml/ZTOrbit/assets/logo-offline.png").isNull());
        SendMessageW(reinterpret_cast<HWND>(m_window->winId()),WM_SYSCOMMAND,SC_CLOSE,1);
        QTRY_COMPARE(requested.size(),1); QTRY_VERIFY(!m_window->isVisible() || m_window->windowState()==Qt::WindowMinimized);
        m_controller->restoreWindow(); QTRY_VERIFY(m_window->isVisible()); QVERIFY(m_window->windowState()!=Qt::WindowMinimized);
        QSignalSpy exited(m_controller.get(),&AppController::exitFinished); m_controller->requestQuit(); QTRY_COMPARE(exited.size(),1);
#else
        QSKIP("Windows-only caption behavior");
#endif
    }
    void localConfigSettingsReadActualValuesAndSaveOnce() {
        prepareLocalConfigDirectory();
        const QByteArray original(
            "{\n"
            "  \"settings\": {\n"
            "    \"primaryPort\": 19999,\n"
            "    \"allowTcpFallbackRelay\": true,\n"
            "    \"forceTcpRelay\": true,\n"
            "    \"tcpFallbackRelay\": \"192.0.2.10/9443\",\n"
            "    \"extraSetting\": \"keep\"\n"
            "  },\n"
            "  \"unknown\": {\"nested\": 42}\n"
            "}\n");
        writeLocalConfig(original);
        RecordingOperations operations;
        openWindow({}, &operations);
        QTRY_VERIFY(m_controller->localConfig().value("loaded").toBool());
        QCOMPARE(readLocalConfig(), original);
        click("settingsButton");
        QTRY_VERIFY(object("settingsPage") && object("settingsPage")->property("visible").toBool());
        QVERIFY(!object("allowTcpFallbackSwitch"));
        click("switchTcpFallbackServerButton");
        QTRY_VERIFY(object("tcpFallbackRelayPopup")->property("opened").toBool());
        auto *force = object("forceTcpFallbackSwitch"); QVERIFY(force);
        QVERIFY(force->property("checked").toBool());
        click("forceTcpFallbackSwitch");
        QTRY_VERIFY(!m_controller->localConfig().value("forceTcpRelay").toBool());
        auto state = vusiif::zto::zerotier::localconfig::parse(readLocalConfig(), true);
        QVERIFY(state.allowTcpFallbackRelay); QVERIFY(!state.forceTcpRelay);
        QCOMPARE(state.root["settings"].toObject()["primaryPort"].toInt(), 19999);
        QCOMPARE(state.root["settings"].toObject()["extraSetting"].toString(), QString("keep"));
        QCOMPARE(state.root["unknown"].toObject()["nested"].toInt(), 42);
        click("forceTcpFallbackSwitch");
        QTRY_VERIFY(m_controller->localConfig().value("forceTcpRelay").toBool());
        QCOMPARE(operations.operations, 0);
        const QByteArray edited("{\"settings\":{\"allowTcpFallbackRelay\":false,\"forceTcpRelay\":false},\"manual\":{\"keep\":true}}");
        writeLocalConfig(edited); m_controller->refreshLocalConfig();
        QTRY_VERIFY(!force->property("checked").toBool());
        QVERIFY(force->property("enabled").toBool());
        QCOMPARE(readLocalConfig(), edited);
        click("forceTcpFallbackSwitch");
        QTRY_VERIFY(m_controller->localConfig().value("forceTcpRelay").toBool());
        state = vusiif::zto::zerotier::localconfig::parse(readLocalConfig(), true);
        QVERIFY(state.allowTcpFallbackRelay); QVERIFY(state.forceTcpRelay);
        QVERIFY(state.root["manual"].toObject()["keep"].toBool());
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void localConfigInvalidFileIsReportedWithoutRewrite() {
        prepareLocalConfigDirectory();
        const QByteArray invalid("{\"settings\": []}");
        writeLocalConfig(invalid);
        openWindow();
        QTRY_VERIFY(m_controller->localConfig().value("loaded").toBool());
        QVERIFY(!m_controller->localConfig().value("valid").toBool());
        click("settingsButton");
        QTRY_VERIFY(object("settingsPage")->property("visible").toBool());
        click("switchTcpFallbackServerButton");
        QTRY_VERIFY(object("tcpFallbackRelayPopup")->property("opened").toBool());
        QVERIFY(!object("forceTcpFallbackSwitch")->property("enabled").toBool());
        QVERIFY(object("localConfigErrorLabel")->property("text").toString().contains(QStringLiteral("settings")));
        QCOMPARE(readLocalConfig(), invalid);
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void localConfigSaveFailureRestoresRealState() {
        prepareLocalConfigDirectory();
        const QByteArray original("{\"settings\":{\"allowTcpFallbackRelay\":true,\"forceTcpRelay\":false},"
                                  "\"unknown\":{\"keep\":1}}");
        writeLocalConfig(original);
        QFile::setPermissions(localConfigPath(), QFileDevice::ReadOwner | QFileDevice::ReadUser);
        RecordingOperations operations;
        AppController controller(nullptr, {}, &operations, m_storage);
        QTRY_VERIFY(controller.localConfig().value("loaded").toBool());
        QVERIFY(controller.localConfig().value("allowTcpFallbackRelay").toBool());
        controller.setTcpFallbackSwitches(false, false);
        QTRY_VERIFY(!controller.localConfig().value("busy").toBool());
        QVERIFY(operations.operations >= 1);
        QCOMPARE(controller.localConfig().value("allowTcpFallbackRelay").toBool(), true);
        QCOMPARE(controller.localConfig().value("forceTcpRelay").toBool(), false);
        QVERIFY(!controller.localConfig().value("error").toString().isEmpty());
        QCOMPARE(readLocalConfig(), original);
        QFile::setPermissions(localConfigPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
#if defined(Q_OS_WIN)
            | QFileDevice::ReadUser | QFileDevice::WriteUser
#else
            | QFileDevice::ReadGroup | QFileDevice::ReadOther
#endif
        );
    }
    void tcpFallbackServerSelectionSavesAndPromptsManualRestart() {
        prepareLocalConfigDirectory();
        const QByteArray original(
            "{\"settings\":{\"primaryPort\":19999,\"allowTcpFallbackRelay\":true,"
            "\"forceTcpRelay\":false,\"tcpFallbackRelay\":\"192.0.2.99/9443\"},\"unknown\":true}");
        writeLocalConfig(original);
        writeMoonFile();
        qputenv("ZT_TEST_MODE", "members");
        RecordingOperations operations;
        openWindow({}, &operations);
        click("settingsButton");
        QTRY_VERIFY(object("settingsPage")->property("visible").toBool());
        click("switchTcpFallbackServerButton");
        QTRY_VERIFY(object("tcpFallbackRelayPopup") && object("tcpFallbackRelayPopup")->property("visible").toBool());
        QTRY_COMPARE(m_controller->tcpFallbackServers().size(), 1);
        const auto candidate = m_controller->tcpFallbackServers().first().toMap();
        QCOMPARE(candidate.value("networkId").toString(), QString("0123456789abcdef"));
        QCOMPARE(candidate.value("relay").toString(), QString("192.0.2.10:9443"));
        QVERIFY(candidate.value("enabled").toBool());
        click("tcpFallbackServerSelector");
        QTRY_VERIFY(object("tcpFallbackServer-0123456789abcdef"));
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/tcp-fallback-popup.png"));
        QTest::qWait(160);
        click("tcpFallbackServer-0123456789abcdef");
        QTest::qWait(250);
        QTRY_VERIFY(object("confirmationDialog") && object("confirmationDialog")->property("visible").toBool());
        QVERIFY(object("confirmationDialog")->property("message").toString().contains("192.0.2.10:9443"));
        click("cancelActionButton");
        QTRY_VERIFY(!object("confirmationDialog")->property("visible").toBool());
        QTest::qWait(80);
        QCOMPARE(readLocalConfig(), original);
        QCOMPARE(operations.operations, 0);
        QVERIFY(!object("notificationBanner") || !object("notificationBanner")->property("visible").toBool());

        click("switchTcpFallbackServerButton");
        QTRY_VERIFY(object("tcpFallbackRelayPopup")->property("visible").toBool());
        QTRY_COMPARE(m_controller->tcpFallbackServers().size(), 1);
        click("tcpFallbackServerSelector");
        QTest::qWait(160);
        click("tcpFallbackServer-0123456789abcdef");
        QTest::qWait(250);
        QTRY_VERIFY(object("confirmationDialog")->property("visible").toBool());
        click("confirmActionButton");
        QTRY_COMPARE(vusiif::zto::zerotier::localconfig::parse(readLocalConfig(), true).tcpFallbackRelay,
                     QString("192.0.2.10/9443"));
        QTRY_VERIFY(object("notificationBanner") && object("notificationBanner")->property("visible").toBool());
        QCOMPARE(object("notificationBanner")->property("message").toString(),
                 QStringLiteral("TCP回退中转服务器已更新，请手动重启 ZeroTier，以及时刷新配置并切换中继连接。"));
        auto state = vusiif::zto::zerotier::localconfig::parse(readLocalConfig(), true);
        QVERIFY(state.valid());
        QVERIFY(state.allowTcpFallbackRelay);
        QVERIFY(!state.forceTcpRelay);
        QCOMPARE(state.root.value("settings").toObject().value("primaryPort").toInt(), 19999);
        QCOMPARE(operations.operations, 0);
        click("dismissNotificationButton");
        QTRY_VERIFY(!object("notificationBanner")->property("visible").toBool());

        QCOMPARE(m_controller->tcpFallbackNetworkName(), QStringLiteral("管理员网络"));
        const auto selected = readLocalConfig();
        click("switchTcpFallbackServerButton");
        QTRY_VERIFY(object("tcpFallbackRelayPopup")->property("visible").toBool());
        QTRY_COMPARE(m_controller->tcpFallbackServers().size(), 1);
        QVERIFY(m_controller->tcpFallbackServers().first().toMap().value("current").toBool());
        click("tcpFallbackServerSelector");
        QTest::qWait(160);
        click("tcpFallbackServer-0123456789abcdef");
        QTest::qWait(120);
        QVERIFY(!object("confirmationDialog") || !object("confirmationDialog")->property("visible").toBool());
        QVERIFY(object("notificationBanner")->property("visible").toBool());
        QVERIFY(!object("notificationBanner")->property("error").toBool());
        QCOMPARE(readLocalConfig(), selected);
        QVERIFY(m_controller->feedback().contains(QStringLiteral("当前已使用")));
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void tcpFallbackNameRestoresAndNeverGuessesIdentity() {
        prepareLocalConfigDirectory(); writeMoonFile();
        writeLocalConfig("{\"settings\":{\"tcpFallbackRelay\":\"192.0.2.10/9443\"}}");
        qputenv("ZT_TEST_MODE", "members");
        {
            AppController controller(nullptr, {}, nullptr, m_storage);
            controller.rename("networks", "0123456789abcdef", QStringLiteral("本机中转备注"));
            controller.refreshTcpFallbackServers();
            QTRY_VERIFY(!controller.tcpFallbackServersBusy());
            QCOMPARE(controller.tcpFallbackNetworkName(), QStringLiteral("本机中转备注"));
        }
        AppController restored(nullptr, {}, nullptr, m_storage);
        restored.refreshTcpFallbackServers(); QTRY_VERIFY(!restored.tcpFallbackServersBusy());
        QCOMPARE(restored.tcpFallbackNetworkName(), QStringLiteral("本机中转备注"));
        qputenv("ZT_TEST_MODE", "ambiguous-relay");
        restored.refreshTcpFallbackServers(); QTRY_VERIFY(!restored.tcpFallbackServersBusy());
        QCOMPARE(restored.tcpFallbackServers().size(), 2);
        QCOMPARE(restored.tcpFallbackNetworkName(), QStringLiteral("已配置（对应多个网络）"));
        writeLocalConfig("{\"settings\":{\"tcpFallbackRelay\":\"192.0.2.99/9443\"}}");
        restored.refreshLocalConfig();
        QCOMPARE(restored.tcpFallbackNetworkName(), QStringLiteral("已配置（无法关联网络）"));
        qputenv("ZT_TEST_MODE", "error"); restored.refreshTcpFallbackServers();
        QTRY_VERIFY(!restored.tcpFallbackServersBusy()); QVERIFY(restored.tcpFallbackServers().isEmpty());
        QVERIFY(!restored.tcpFallbackServersMessage().isEmpty());
        QVERIFY(!restored.tcpFallbackNetworkName().contains("192.0.2"));
        writeLocalConfig(vusiif::zto::zerotier::localconfig::defaultContent()); restored.refreshLocalConfig();
        qputenv("ZT_TEST_MODE", "empty"); restored.refreshTcpFallbackServers();
        QTRY_VERIFY(!restored.tcpFallbackServersBusy()); QVERIFY(restored.tcpFallbackServers().isEmpty());
        QCOMPARE(restored.tcpFallbackNetworkName(), QStringLiteral("未选择"));
    }
    void tcpFallbackRelaySaveFailureDoesNotDisplaySelection() {
        prepareLocalConfigDirectory(); writeMoonFile();
        const QByteArray original("{\"settings\":{\"tcpFallbackRelay\":\"192.0.2.99/9443\"},\"keep\":true}");
        writeLocalConfig(original); qputenv("ZT_TEST_MODE", "members");
        RecordingOperations operations; openWindow({}, &operations);
        m_controller->refreshTcpFallbackServers(); QTRY_VERIFY(!m_controller->tcpFallbackServersBusy());
        QSignalSpy notices(m_controller.get(), &AppController::pluginNoticeRequested);
        QFile::setPermissions(localConfigPath(), QFileDevice::ReadOwner | QFileDevice::ReadUser);
        m_controller->selectTcpFallbackServer("0123456789abcdef");
        QTRY_VERIFY(object("confirmationDialog") && object("confirmationDialog")->property("opened").toBool());
        click("confirmActionButton");
        QTRY_VERIFY(!object("confirmationDialog")->property("visible").toBool());
        QTRY_VERIFY(!m_controller->localConfig()["busy"].toBool());
        QVERIFY(operations.operations > 0); QCOMPARE(notices.size(),0);
        QCOMPARE(readLocalConfig(), original);
        QCOMPARE(m_controller->tcpFallbackNetworkName(), QStringLiteral("已配置（无法关联网络）"));
        QVERIFY(!m_controller->localConfig()["error"].toString().isEmpty());
        QFile::setPermissions(localConfigPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser);
        click("settingsButton"); QTest::qWait(200); click("switchTcpFallbackServerButton");
        QTRY_VERIFY(object("tcpFallbackRelayPopup")->property("opened").toBool());
        QTRY_VERIFY(!m_controller->tcpFallbackServersBusy());
        QCOMPARE(object("tcpFallbackServerSelector")->property("displayText").toString(), m_controller->tcpFallbackNetworkName());
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void tcpFallbackServerWithoutMoonMetadataIsDisabled() {
        prepareLocalConfigDirectory();
        writeLocalConfig(vusiif::zto::zerotier::localconfig::defaultContent());
        writeMoonFile(QStringLiteral("0000008056c2e21c.moon"), QByteArray("moon"));
        openWindow();
        click("settingsButton");
        QTRY_VERIFY(object("settingsPage")->property("visible").toBool());
        click("switchTcpFallbackServerButton");
        QTRY_VERIFY(object("tcpFallbackRelayPopup")->property("visible").toBool());
        QTRY_VERIFY(m_controller->tcpFallbackServers().size() >= 1);
        click("tcpFallbackServerSelector");
        QTest::qWait(160);
        bool found = false;
        for (const auto &value : m_controller->tcpFallbackServers()) {
            const auto candidate = value.toMap();
            if (candidate.value("networkId").toString() != QString("8056c2e21c000000")) continue;
            found = true;
            QVERIFY(!candidate.value("enabled").toBool());
            QVERIFY(candidate.value("reason").toString().contains(QStringLiteral("Moon")));
            QVERIFY(!object("tcpFallbackServer-8056c2e21c000000")->property("enabled").toBool());
        }
        QVERIFY(found);
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void localConfigRevealUsesSystemPathAndReportsMissingFile() {
        prepareLocalConfigDirectory();
        writeLocalConfig(vusiif::zto::zerotier::localconfig::defaultContent());
        RevealOperations operations;
        AppController controller(nullptr, {}, &operations, m_storage);
        QTRY_VERIFY(controller.localConfig().value("loaded").toBool());
        controller.revealLocalConfig();
        QCOMPARE(operations.calls, 1);
        QCOMPARE(operations.path, QFileInfo(localConfigPath()).absoluteFilePath());
        QFile::remove(localConfigPath());
        controller.revealLocalConfig();
        QCOMPARE(operations.calls, 1);
        QVERIFY(controller.feedback().contains(QStringLiteral("尚未创建")));
        SystemOperations real;
        QString error;
        QVERIFY(!real.revealFile(m_directory.filePath("missing-local.conf"), error));
        QVERIFY(error.contains(QStringLiteral("尚未创建")));
#ifdef Q_OS_WIN
        QCOMPARE(vusiif::zto::ui::explorerSelectParameters("C:/ProgramData/ZeroTier/One/带 空格/local.conf"),
                 QString("/select,\"C:\\ProgramData\\ZeroTier\\One\\带 空格\\local.conf\""));
#endif
    }
    void localConfigSettingsThemeAndMinimumSize() {
        prepareLocalConfigDirectory();
        writeLocalConfig(vusiif::zto::zerotier::localconfig::defaultContent());
        writeMoonFile(); qputenv("ZT_TEST_MODE", "members");
        openWindow(); click("settingsButton");
        QTRY_VERIFY(object("settingsPage")->property("visible").toBool());
        QCOMPARE(object("openLocalConfigButton")->property("text").toString(), QStringLiteral("显示"));
        QCOMPARE(object("openSettingsButton")->property("text").toString(), QStringLiteral("显示"));
        m_window->resize(720, 480);
        for (const auto &theme : {"dark", "light"}) {
            m_controller->setTheme(theme); QTest::qWait(300);
            scrollSettingsTo("switchTcpFallbackServerButton");
            QVERIFY(m_window->grabWindow().save(QString(".tmp/screenshots/settings-%1-small.png").arg(theme)));
            click("switchTcpFallbackServerButton");
            QTRY_VERIFY(object("tcpFallbackRelayPopup")->property("opened").toBool());
            QTRY_COMPARE(m_controller->tcpFallbackServers().size(), 1);
            const auto longName=QStringLiteral("很长的网络名称与本机备注用于检查选择框省略和布局边界").repeated(5).left(128);
            m_controller->rename("networks", "0123456789abcdef", longName);
            writeLocalConfig("{\"settings\":{\"tcpFallbackRelay\":\"192.0.2.10/9443\"}}"); m_controller->refreshLocalConfig();
            QTRY_COMPARE(object("tcpFallbackServerSelector")->property("displayText").toString(), longName);
            QTest::qWait(180);
            auto *popup = object("tcpFallbackRelayPopup");
            QVERIFY(popup->property("x").toReal() >= 0); QVERIFY(popup->property("y").toReal() >= 0);
            QVERIFY(popup->property("x").toReal()+popup->property("width").toReal() <= m_window->width());
            QVERIFY(popup->property("y").toReal()+popup->property("height").toReal() <= m_window->height());
            QVERIFY(m_window->grabWindow().save(QString(".tmp/screenshots/tcp-popup-%1-small.png").arg(theme)));
            click("tcpFallbackServerSelector"); QTest::qWait(180);
            auto *dropdown=object("tcpFallbackServerSelector")->property("popup").value<QObject *>(); QVERIFY(dropdown);
            QVERIFY(dropdown->property("opened").toBool());
            auto *content=qobject_cast<QQuickItem *>(dropdown->property("contentItem").value<QObject *>()); QVERIFY(content);
            const auto origin=content->mapToScene(QPointF(0,0));
            QVERIFY(origin.x() >= 0 && origin.y() >= 0);
            QVERIFY(origin.x()+content->width() <= m_window->width());
            QVERIFY(origin.y()+content->height() <= m_window->height());
            QVERIFY(m_window->grabWindow().save(QString(".tmp/screenshots/tcp-selector-%1-small.png").arg(theme)));
            QTest::keyClick(m_window,Qt::Key_Escape); QTRY_VERIFY(!dropdown->property("visible").toBool());
            QTest::keyClick(m_window, Qt::Key_Escape);
            QTRY_VERIFY(!popup->property("visible").toBool());
            // Outside press closes and must not leave a transparent input blocker.
            click("switchTcpFallbackServerButton"); QTRY_VERIFY(popup->property("opened").toBool());
            QTest::mouseClick(m_window, Qt::LeftButton, {}, QPoint(40, 320));
            QTRY_VERIFY(!popup->property("visible").toBool());
            click("switchTcpFallbackServerButton"); QTRY_VERIFY(popup->property("opened").toBool());
            QTest::keyClick(m_window, Qt::Key_Escape); QTRY_VERIFY(!popup->property("visible").toBool());
        }
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void forcedTcpFallbackDoesNotReportConnectionFault() {
        prepareLocalConfigDirectory();
        writeLocalConfig("{\"settings\":{\"allowTcpFallbackRelay\":true,\"forceTcpRelay\":true}}");
        qputenv("ZT_TEST_MODE", "tcp-fallback");
        openWindow();
        QTRY_VERIFY(m_controller->service().value("available").toBool());
        QTRY_VERIFY(m_controller->info().value("tcpFallbackActive").toBool());
        QTRY_VERIFY(m_controller->service().value("warning").toString().isEmpty());
        writeLocalConfig("{\"settings\":{\"allowTcpFallbackRelay\":true,\"forceTcpRelay\":false}}");
        m_controller->refreshLocalConfig();
        QTRY_VERIFY(!m_controller->localConfig().value("forceTcpRelay").toBool());
        QTRY_VERIFY(m_controller->service().value("warning").toString().contains(QStringLiteral("TCP 中转")));
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void exportConfigurationCarriesRelayOnlyForMatchingServer() {
        prepareLocalConfigDirectory();
        writeMoonFile();
        writeLocalConfig("{\"settings\":{\"allowTcpFallbackRelay\":true,\"forceTcpRelay\":false,"
                         "\"tcpFallbackRelay\":\"192.0.2.10/9443\"}}");
        qputenv("ZT_TEST_MODE", "members");
        AppController controller(nullptr, {}, nullptr, m_storage);
        const auto match = m_directory.filePath("export-match");
        controller.exportConfiguration("0123456789abcdef", QUrl::fromLocalFile(match));
        QTRY_VERIFY(QFileInfo::exists(match + ".z2bundle"));
        vusiif::zto::zerotier::bundle::Configuration decoded; QString error;
        QFile first(match + ".z2bundle"); QVERIFY(first.open(QIODevice::ReadOnly));
        QVERIFY2(vusiif::zto::zerotier::bundle::decode(first.readAll(), decoded, error), qPrintable(error));
        QCOMPARE(decoded.tcpFallbackRelay, QString("192.0.2.10:9443"));

        writeLocalConfig("{\"settings\":{\"allowTcpFallbackRelay\":true,\"forceTcpRelay\":false,"
                         "\"tcpFallbackRelay\":\"192.0.2.11/9443\"}}");
        const auto mismatch = m_directory.filePath("export-mismatch");
        controller.exportConfiguration("0123456789abcdef", QUrl::fromLocalFile(mismatch));
        QTRY_VERIFY(QFileInfo::exists(mismatch + ".z2bundle"));
        QFile second(mismatch + ".z2bundle"); QVERIFY(second.open(QIODevice::ReadOnly));
        QVERIFY2(vusiif::zto::zerotier::bundle::decode(second.readAll(), decoded, error), qPrintable(error));
        QVERIFY(decoded.tcpFallbackRelay.isEmpty());

        // An invalid current file must not fall back to a cached relay value.
        writeLocalConfig("{\"settings\":");
        const auto invalid = m_directory.filePath("export-invalid");
        controller.exportConfiguration("0123456789abcdef", QUrl::fromLocalFile(invalid));
        QTRY_VERIFY(QFileInfo::exists(invalid + ".z2bundle"));
        QFile third(invalid + ".z2bundle"); QVERIFY(third.open(QIODevice::ReadOnly));
        QVERIFY2(vusiif::zto::zerotier::bundle::decode(third.readAll(), decoded, error), qPrintable(error));
        QVERIFY(decoded.tcpFallbackRelay.isEmpty());
    }
    void importConfigurationChangesOnlyRelayAfterMoonConfirmation() {
#ifdef Q_OS_WIN
        prepareLocalConfigDirectory();
        const QByteArray original(
            "{\"settings\":{\"primaryPort\":19999,\"allowTcpFallbackRelay\":true,"
            "\"forceTcpRelay\":false,\"tcpFallbackRelay\":\"198.51.100.9/9443\"},\"unknown\":{\"keep\":1}}");
        writeLocalConfig(original);
        writeMoonFile(QStringLiteral("0000000123456789.moon"), QByteArray("old-moon"));
        vusiif::zto::zerotier::bundle::Configuration configuration{
            "0123456789abcdef", "0000000123456789.moon", QByteArray("new-moon"), "192.0.2.10:9443"};
        const auto encoded = vusiif::zto::zerotier::bundle::encode(configuration);
        QVERIFY(!encoded.isEmpty());
        const auto bundlePath = m_directory.filePath("import.z2bundle");
        QFile bundle(bundlePath); QVERIFY(bundle.open(QIODevice::WriteOnly)); bundle.write(encoded); bundle.close();
        ScriptExecutingOperations operations;
        AppController controller(nullptr, {}, &operations, m_storage);
        QSignalSpy confirmation(&controller, &AppController::confirmationRequested);
        controller.importConfiguration(QUrl::fromLocalFile(bundlePath));
        QTRY_COMPARE(confirmation.size(), 1);
        controller.confirm(false);
        QTest::qWait(120);
        QCOMPARE(operations.operations, 0);
        QCOMPARE(readLocalConfig(), original);

        controller.importConfiguration(QUrl::fromLocalFile(bundlePath));
        QTRY_COMPARE(confirmation.size(), 2);
        controller.confirm(true);
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(operations.operations, 1);
        auto state = vusiif::zto::zerotier::localconfig::parse(readLocalConfig(), true);
        QVERIFY(state.valid());
        QCOMPARE(state.tcpFallbackRelay, QString("192.0.2.10/9443"));
        QVERIFY(state.allowTcpFallbackRelay);
        QVERIFY(!state.forceTcpRelay);
        QCOMPARE(state.root.value("settings").toObject().value("primaryPort").toInt(), 19999);
        QCOMPARE(state.root.value("unknown").toObject().value("keep").toInt(), 1);
        QFile moonFile(QDir(vusiif::zto::zerotier::moonDirectory()).filePath("0000000123456789.moon"));
        QVERIFY(moonFile.open(QIODevice::ReadOnly));
        QCOMPARE(moonFile.readAll(), QByteArray("new-moon"));

        configuration.tcpFallbackRelay.clear();
        const auto withoutRelay = vusiif::zto::zerotier::bundle::encode(configuration);
        const auto withoutPath = m_directory.filePath("without-relay.z2bundle");
        QFile without(withoutPath); QVERIFY(without.open(QIODevice::WriteOnly)); without.write(withoutRelay); without.close();
        controller.importConfiguration(QUrl::fromLocalFile(withoutPath));
        QTRY_COMPARE(confirmation.size(), 3);
        controller.confirm(true);
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(operations.operations, 2);
        QCOMPARE(vusiif::zto::zerotier::localconfig::parse(readLocalConfig(), true).tcpFallbackRelay,
                 QString("192.0.2.10/9443"));

        const auto beforeInvalid = readLocalConfig();
        auto object = QJsonDocument::fromJson(encoded).object();
        object["tcpFallbackRelay"] = "192.0.2.10:9993";
        const auto invalidPath = m_directory.filePath("invalid-relay.z2bundle");
        QFile invalid(invalidPath); QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write(QJsonDocument(object).toJson()); invalid.close();
        controller.importConfiguration(QUrl::fromLocalFile(invalidPath));
        QTest::qWait(120);
        QCOMPARE(operations.operations, 2);
        QCOMPARE(readLocalConfig(), beforeInvalid);
        QVERIFY(controller.feedback().contains("tcpFallbackRelay"));
#else
        QSKIP("Script execution fixture is Windows-only");
#endif
    }
    void configurationImportOnlyJoinsAfterSuccessfulCopy() {
        DeferredOperations operations; AppController controller(nullptr,{},&operations,m_storage);
        const auto bytes=vusiif::zto::zerotier::bundle::encode({"0123456789abcdef","0000000123456789.moon",QByteArray("signed-moon-bytes")});
        QVERIFY(!bytes.isEmpty()); QFile source(m_directory.filePath("network.z2bundle")); QVERIFY(source.open(QIODevice::WriteOnly)); source.write(bytes); source.close();
        const auto trace=m_directory.filePath("joins.txt"); qputenv("ZT_TEST_COMMAND_LOG",trace.toUtf8());
        controller.importConfiguration(QUrl::fromLocalFile(source.fileName())); QVERIFY(controller.busy()); QVERIFY(operations.script.contains("signed-moon-bytes")==false);
        QVERIFY(!QFileInfo::exists(trace)); operations.complete(false); QVERIFY(!QFileInfo::exists(trace)); QVERIFY(!controller.busy());
        QSignalSpy joined(&controller,&AppController::networkJoined);
        controller.importConfiguration(QUrl::fromLocalFile(source.fileName())); QVERIFY(controller.busy()); operations.complete(true);
        QTRY_COMPARE(joined.size(),1); QFile log(trace); QVERIFY(log.open(QIODevice::ReadOnly)); QVERIFY(log.readAll().contains("join 0123456789abcdef"));
    }
    void lazyEditorsMenusAndDialogsKeepTheirInteraction() {
        openWindow(); QTRY_COMPARE(m_controller->networks()->count(),2);
        QVERIFY(!object("joinNetworkDialog")); QVERIFY(!object("detailsPopup")); QVERIFY(!object("guideOverlay"));
        QVERIFY(!object("recordContextMenu")); QVERIFY(!object("remarkEditor"));
        const auto id=m_controller->networks()->get(0)["rowId"].toString();
        auto *row=qobject_cast<QQuickItem *>(object(qPrintable("recordRow-"+id))); QVERIFY(row);
        QTest::mouseClick(m_window,Qt::RightButton,{},row->mapToScene(QPointF(80,row->height()/2)).toPoint());
        QTRY_VERIFY(object("recordContextMenu")); QTRY_VERIFY(object("recordContextMenu")->property("visible").toBool());
        QTest::keyClick(m_window,Qt::Key_Escape);
        auto *records=object("networks"); QVERIFY(records);
        QVERIFY(QMetaObject::invokeMethod(records,"edit",Q_ARG(QVariant,id),Q_ARG(QVariant,"Pending")));
        QTRY_VERIFY(object("remarkEditor")); object("remarkEditor")->setProperty("text","Canceled");
        QTest::keyClick(m_window,Qt::Key_Escape); QTRY_VERIFY(!object("remarkEditor"));
        QVERIFY(!hasSetting("names/networks/"+id));
        QVERIFY(QMetaObject::invokeMethod(records,"edit",Q_ARG(QVariant,id),Q_ARG(QVariant,"Saved")));
        QTRY_VERIFY(object("remarkEditor")); QTest::keyClick(m_window,Qt::Key_Return);
        QTRY_COMPARE(setting("names/networks/"+id).toString(),QString("Saved"));
        click("joinNetworkButton"); QTRY_VERIFY(object("joinNetworkDialog")); QTest::keyClick(m_window,Qt::Key_Escape);
        click("joinNetworkButton"); QTRY_VERIFY(object("joinNetworkDialog")->property("visible").toBool());
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void sidebarUsesFixedWidthsAndIgnoresLegacyKey() {
        // The retired draggable width key must neither size nor be touched.
        writeSetting("window/qml/sidebarWidth", 320);
        openWindow();
        QTRY_COMPARE(m_controller->networks()->count(), 2);
        auto *sidebar = item("sidebar"); QVERIFY(sidebar);
        QCOMPARE(sidebar->width(), 184.0);
        QCOMPARE(m_window->property("sidebarWidth").toReal(), 184.0);
        QCOMPARE(m_window->minimumWidth(), 640);
        QCOMPARE(m_window->minimumHeight(), 440);
        QVERIFY(!object("sidebarHandle"));
        // Dragging where the old SplitView handle lived does not resize anything.
        const auto edge = sidebar->mapToScene(QPointF(sidebar->width() + 2, sidebar->height() / 2)).toPoint();
        QTest::mousePress(m_window, Qt::LeftButton, {}, edge);
        QTest::mouseMove(m_window, edge + QPoint(80, 0));
        QTest::mouseRelease(m_window, Qt::LeftButton, {}, edge + QPoint(80, 0));
        QTest::qWait(120);
        QCOMPARE(sidebar->width(), 184.0);
        QVERIFY(hasSetting("window/qml/sidebarWidth"));
        QCOMPARE(setting("window/qml/sidebarWidth").toInt(), 320);
        click("sidebarToggleButton");
        QTRY_COMPARE(sidebar->width(), 56.0);
        QVERIFY(m_window->property("sidebarCollapsed").toBool());
        QCOMPARE(setting("window/qml/sidebarCollapsed").toBool(), true);
        QCOMPARE(setting("window/qml/sidebarWidth").toInt(), 320);
        // A recreated window restores the user's choice and keeps ignoring the old key.
        m_engine.reset(); m_window = nullptr; m_controller.reset();
        openWindow();
        QTRY_COMPARE(item("sidebar")->width(), 56.0);
        QVERIFY(m_window->property("sidebarCollapsed").toBool());
        click("sidebarToggleButton");
        QTRY_COMPARE(item("sidebar")->width(), 184.0);
        QCOMPARE(setting("window/qml/sidebarCollapsed").toBool(), false);
        QCOMPARE(setting("window/qml/sidebarWidth").toInt(), 320);
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void collapsedSidebarKeepsNavigationUsable() {
        openWindow();
        QTRY_COMPARE(m_controller->networks()->count(), 2);
        auto *networkItem = navigationItem("builtin/zerotier"); QVERIFY(networkItem);
        click("sidebarToggleButton");
        QTRY_COMPARE(item("sidebar")->width(), 56.0);
        // The menu name survives both as a bound mirror and as the attached property.
        QCOMPARE(networkItem->property("accessibleName").toString(), QStringLiteral("ZeroTier"));
        QCOMPARE(evaluate(networkItem, "Accessible.name").toString(), QStringLiteral("ZeroTier"));
        auto *label = item("navigationLabel-builtin/zerotier"); QVERIFY(label);
        QVERIFY(!label->isVisible());
        auto *zeroTierIcon = item("zeroTierMenuIcon"); QVERIFY(zeroTierIcon);
        QVERIFY(zeroTierIcon->isVisible());
        auto *builtinFallback = item("navigationFallbackIcon-builtin/zerotier"); QVERIFY(builtinFallback);
        QVERIFY(!builtinFallback->isVisible());
        // Disabled/loading plugin menus keep their icon slot and spinner when collapsed.
        MenuContribution preparing; preparing.id = "preparing"; preparing.title = "准备中插件";
        preparing.createPage = [](QQmlEngine *, QQuickItem *owner) { return new QQuickItem(owner); };
        QVERIFY(m_controller->pluginRegistry()->registerPlugin({{"test-sidebar-preparing", "Preparing", "1"}, {preparing}}, nullptr, true).ok());
        QTRY_VERIFY(navigationItem("test-sidebar-preparing/preparing"));
        auto *pending = navigationItem("test-sidebar-preparing/preparing");
        QVERIFY(!pending->property("enabled").toBool());
        auto *busy = item("navigationBusy-test-sidebar-preparing/preparing"); QVERIFY(busy);
        QVERIFY(busy->isVisible());
        // Selection and keyboard focus stay visible in the narrow sidebar.
        MenuContribution page; page.id = "page"; page.title = "Sidebar Page";
        page.createPage = [](QQmlEngine *, QQuickItem *owner) {
            auto *created = new QQuickItem(owner); created->setObjectName("sidebarStatePage"); return created;
        };
        QVERIFY(m_controller->pluginRegistry()->registerPlugin({{"test-sidebar-state", "State", "1"}, {page}}).ok());
        QTRY_VERIFY(navigationItem("test-sidebar-state/page"));
        click(qPrintable(QStringLiteral("navigation-test-sidebar-state/page")));
        QTRY_COMPARE(m_controller->activeMenu(), QStringLiteral("test-sidebar-state/page"));
        auto *selected = navigationItem("test-sidebar-state/page"); QVERIFY(selected);
        QVERIFY(selected->property("highlighted").toBool());
        m_window->requestActivate(); QTest::qWait(50);
        selected->forceActiveFocus();
        QTRY_VERIFY(selected->hasActiveFocus());
        QCOMPARE(selected->property("focusPolicy").toInt(), int(Qt::StrongFocus));
        QCOMPARE(evaluate(selected, "background.border.width").toInt(), 2);
        // Hovering a collapsed entry shows its name even without a status message.
        QTest::mouseMove(m_window, QPoint(2, 2)); QTest::qWait(120);
        QTest::mouseMove(m_window, selected->mapToScene(QPointF(selected->width() / 2, selected->height() / 2)).toPoint());
        auto *tip = named("navigationTooltip-test-sidebar-state/page"); QVERIFY(tip);
        QTRY_VERIFY_WITH_TIMEOUT(tip->property("visible").toBool(), 4000);
        QCOMPARE(tip->property("text").toString(), QStringLiteral("Sidebar Page"));
        QTest::mouseMove(m_window, QPoint(m_window->width() - 20, 20));
        QTRY_VERIFY(!tip->property("visible").toBool());
        // Collapsed layout is checked at the minimum window size in both themes.
        m_window->resize(640, 440); QTest::qWait(150);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-sidebar-collapsed-light.png"));
        m_controller->setTheme("dark"); QTest::qWait(300);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-sidebar-collapsed-dark.png"));
        // A collapsed entry is still clickable and switches the active page.
        // Registering menus rebuilt the ListView model, so look the delegate up again.
        click(qPrintable(QStringLiteral("navigation-builtin/zerotier")));
        QTRY_COMPARE(m_controller->activeMenu(), QStringLiteral("builtin/zerotier"));
        auto *builtinItem = navigationItem("builtin/zerotier"); QVERIFY(builtinItem);
        QVERIFY(builtinItem->property("highlighted").toBool());
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void sidebarToggleKeepsPluginPageAndState() {
        openWindow();
        int created = 0;
        MenuContribution page; page.id = "page"; page.title = "Survivor";
        page.createPage = [&created](QQmlEngine *, QQuickItem *owner) {
            ++created;
            auto *createdItem = new QQuickItem(owner);
            createdItem->setObjectName("sidebarSurvivorPage");
            return createdItem;
        };
        QVERIFY(m_controller->pluginRegistry()->registerPlugin({{"test-sidebar-survivor", "Survivor", "1"}, {page}}).ok());
        auto *container = item("pluginContainer"); QVERIFY(container);
        QVERIFY(m_controller->navigate("test-sidebar-survivor/page", container));
        QTRY_VERIFY(named("sidebarSurvivorPage"));
        auto *survivor = item("sidebarSurvivorPage"); QVERIFY(survivor);
        QPointer<QQuickItem> guard(survivor);
        survivor->setProperty("keptState", 42);
        click("sidebarToggleButton");
        QTRY_COMPARE(item("sidebar")->width(), 56.0);
        QVERIFY(guard); QCOMPARE(created, 1);
        QVERIFY(survivor->isVisible());
        QCOMPARE(survivor->property("keptState").toInt(), 42);
        QTRY_COMPARE(survivor->width(), container->width());
        // Leave the toggle so a delayed tooltip cannot intercept the next click.
        QTest::mouseMove(m_window, QPoint(2, 2)); QTest::qWait(120);
        click("sidebarToggleButton");
        QTRY_COMPARE(item("sidebar")->width(), 184.0);
        QVERIFY(guard); QCOMPARE(created, 1);
        QCOMPARE(m_controller->activeMenu(), QStringLiteral("test-sidebar-survivor/page"));
        QVERIFY(survivor->isVisible());
        QCOMPARE(survivor->property("keptState").toInt(), 42);
        QTRY_COMPARE(survivor->width(), container->width());
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void sidebarPluginIconsAndFallbacks() {
        openWindow();
        QPixmap pixmap(20, 20); pixmap.fill(Qt::red);
        MenuContribution withIcon; withIcon.id = "withicon"; withIcon.title = "With Icon"; withIcon.icon = QIcon(pixmap);
        MenuContribution withoutIcon; withoutIcon.id = "noicon"; withoutIcon.title = "No Icon";
        const auto simplePage = [](QQmlEngine *, QQuickItem *owner) { return new QQuickItem(owner); };
        withIcon.createPage = simplePage; withoutIcon.createPage = simplePage;
        QVERIFY(m_controller->pluginRegistry()->registerPlugin({{"test-sidebar-icons", "Icons", "1"}, {withIcon, withoutIcon}}).ok());
        QTRY_VERIFY(item("navigationIcon-test-sidebar-icons/withicon"));
        auto *image = item("navigationIcon-test-sidebar-icons/withicon"); QVERIFY(image->isVisible());
        auto *withIconFallback = item("navigationFallbackIcon-test-sidebar-icons/withicon"); QVERIFY(withIconFallback);
        QVERIFY(!withIconFallback->isVisible());
        auto *fallback = item("navigationFallbackIcon-test-sidebar-icons/noicon"); QVERIFY(fallback);
        QVERIFY(fallback->isVisible());
        auto *noIconImage = item("navigationIcon-test-sidebar-icons/noicon"); QVERIFY(noIconImage);
        QVERIFY(!noIconImage->isVisible());
        // The built-in ZeroTier menu uses the network icon, not the plugin fallback.
        auto *zeroTierIcon = item("zeroTierMenuIcon"); QVERIFY(zeroTierIcon);
        QVERIFY(zeroTierIcon->isVisible());
        QVERIFY(object("gearIcon")); QVERIFY(object("chevronIcon"));
        click("sidebarToggleButton");
        QTRY_COMPARE(item("sidebar")->width(), 56.0);
        QVERIFY(image->isVisible()); QVERIFY(fallback->isVisible());
        QVERIFY(!item("navigationLabel-test-sidebar-icons/noicon")->isVisible());
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void sidebarLongTitlesElideAndManyMenusScroll() {
        openWindow();
        QList<MenuContribution> menus;
        for (int index = 0; index < 26; ++index) {
            MenuContribution menu;
            menu.id = QStringLiteral("menu-%1").arg(index);
            menu.title = QStringLiteral("很长的插件菜单名称用于检查省略与滚动的行为第%1项").arg(index);
            menu.createPage = [](QQmlEngine *, QQuickItem *owner) { return new QQuickItem(owner); };
            menus.append(menu);
        }
        QVERIFY(m_controller->pluginRegistry()->registerPlugin({{"test-sidebar-many", "Many", "1"}, menus}).ok());
        auto *navigation = item("navigation"); QVERIFY(navigation);
        QTRY_VERIFY(navigation->property("contentHeight").toReal() > navigation->height());
        QVERIFY(navigation->property("clip").toBool());
        auto *label = item("navigationLabel-test-sidebar-many/menu-0"); QVERIFY(label);
        QTRY_VERIFY(label->property("truncated").toBool());
        QVERIFY(label->property("elide").toInt() != 0);
        // Scrolling to the end instantiates the trailing delegates and keeps the
        // last one inside the viewport.
        for (int attempt = 0; attempt < 50 && !navigationItem("test-sidebar-many/menu-25"); ++attempt) {
            navigation->setProperty("contentY", std::max(0.0, navigation->property("contentHeight").toReal() - navigation->height()));
            QTest::qWait(30);
        }
        QVERIFY(navigation->property("contentY").toReal() > 0);
        auto *last = navigationItem("test-sidebar-many/menu-25"); QVERIFY(last);
        auto lastTop = last->mapToItem(navigation, 0, 0).y();
        for (int attempt = 0; attempt < 30; ++attempt) {
            navigation->setProperty("contentY", std::max(0.0, navigation->property("contentHeight").toReal() - navigation->height()));
            QTest::qWait(20);
            lastTop = last->mapToItem(navigation, 0, 0).y();
            if (lastTop >= 0 && lastTop + last->height() <= navigation->height() + 1) break;
        }
        QVERIFY(lastTop >= 0);
        QVERIFY(lastTop + last->height() <= navigation->height() + 1);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-sidebar-many-menus.png"));
        // Collapsing keeps the same scrolling list and its clipping.
        click("sidebarToggleButton");
        QTRY_COMPARE(item("sidebar")->width(), 56.0);
        QVERIFY(navigation->property("contentHeight").toReal() > navigation->height());
        QVERIFY(navigation->property("clip").toBool());
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void windowSizeUsesCompactDefaultsAndClampsRestoredValues() {
        // A size saved on a large display is clamped to the work area, never above it.
        writeSetting("window/qml/savedWidth", 5000);
        writeSetting("window/qml/savedHeight", 5000);
        openWindow();
        const auto area = m_window->screen() ? m_window->screen()->availableGeometry() : QRect();
        QCOMPARE(m_window->minimumWidth(), 640);
        QCOMPARE(m_window->minimumHeight(), 440);
        const auto clamped = [](int value, int minimum, int available) {
            const int candidate = std::max(minimum, value);
            return available > 0 ? std::max(minimum, std::min(candidate, available)) : candidate;
        };
        QCOMPARE(m_window->width(), clamped(5000, 640, area.width()));
        QCOMPARE(m_window->height(), clamped(5000, 440, area.height()));
        m_engine.reset(); m_window = nullptr; m_controller.reset();
        // A too-small saved size snaps back to the documented minimum.
        writeSetting("window/qml/savedWidth", 120);
        writeSetting("window/qml/savedHeight", 120);
        openWindow();
        QCOMPARE(m_window->width(), 640);
        QCOMPARE(m_window->height(), 440);
        m_engine.reset(); m_window = nullptr; m_controller.reset();
        // With no saved geometry the compact defaults apply (clamped if needed).
        QFile::remove(m_storage.settingsFile());
        openWindow();
        QCOMPARE(m_window->width(), clamped(960, 640, area.width()));
        QCOMPARE(m_window->height(), clamped(700, 440, area.height()));
        // The clamp arithmetic is testable without a small monitor: below-minimum
        // values snap to the minimum, oversized ones to the work area.
        QCOMPARE(evaluate(m_window, "clampWindowSize(5000, minimumWidth, 800)").toInt(), 800);
        QCOMPARE(evaluate(m_window, "clampWindowSize(120, minimumWidth, 800)").toInt(), 640);
        QCOMPARE(evaluate(m_window, "clampWindowSize(5000, minimumWidth, 0)").toInt(), 5000);
        QCOMPARE(evaluate(m_window, "clampWindowSize(5000, minimumHeight, 300)").toInt(), 440);
        // Critical page controls stay reachable at the documented minimum size.
        m_window->resize(640, 440); QTest::qWait(150);
        auto *container = item("pluginContainer"); QVERIFY(container);
        QVERIFY(m_controller->navigate(SettingsMenuId, container)); QTest::qWait(250);
        auto *settingsPage = item("settingsPage"); QVERIFY(settingsPage);
        QVERIFY(settingsPage->property("visible").toBool());
        QVERIFY(settingsPage->mapToScene(QPointF(0, 0)).y() + settingsPage->height() <= m_window->height() + 1);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-window-minimum-settings.png"));
        m_controller->setTheme("dark"); QTest::qWait(300);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-window-minimum-settings-dark.png"));
        QVERIFY(m_controller->navigate(ZeroTierMenuId, container)); QTest::qWait(250);
        auto *join = item("joinNetworkButton"); QVERIFY(join);
        QVERIFY(join->width() > 0 && join->height() > 0);
        const auto joinPoint = join->mapToScene(QPointF(0, 0));
        QVERIFY(joinPoint.x() >= 0 && joinPoint.x() + join->width() <= m_window->width());
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-window-minimum-zerotier-dark.png"));
        m_controller->setTheme("light"); QTest::qWait(300);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-window-minimum-zerotier.png"));
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    // The settings page hosts both feature sections. The update section must be
    // honest about an unconfigured build instead of pretending to check.
    void settingsHostsUpdateAndResetSections() {
        openWindow(); click("settingsButton");
        QTRY_VERIFY(object("settingsPage")->property("visible").toBool());
        QVERIFY(object("updateSection"));
        QVERIFY(object("resetSection"));
        auto *update = m_controller->updateObject(); QVERIFY(update);
        auto *reset = m_controller->resetObject(); QVERIFY(reset);
        auto *check = object("checkUpdateButton"); QVERIFY(check);
        QCOMPARE(check->property("text").toString(), QStringLiteral("检查更新"));
        QCOMPARE(check->property("enabled").toBool(), update->property("configured").toBool()
                 && !update->property("busy").toBool());
        QVERIFY(!object("updateUnconfiguredLabel"));
        QVERIFY(!object("updateServerInput"));
        QVERIFY(!object("clearUpdateServerButton"));
        QVERIFY(object("updateServerButton"));
        auto *resetButton = object("resetZeroTierStateButton"); QVERIFY(resetButton);
        QCOMPARE(resetButton->property("enabled").toBool(), reset->property("canReset").toBool());
        QCOMPARE(resetButton->property("text").toString(), QStringLiteral("重置"));
        // Neither section may fire a network or elevated operation on its own.
        QVERIFY(!update->property("busy").toBool());
        QVERIFY(!reset->property("busy").toBool());
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void updateServerCanBeSavedAndClearedFromSettings() {
        writeSetting("update/server", QString());
        openWindow(); click("settingsButton");
        QTRY_VERIFY(object("settingsPage")->property("visible").toBool());
        auto *update = m_controller->updateObject(); QVERIFY(update);
        auto *check = object("checkUpdateButton"); QVERIFY(check);
        QVERIFY(!check->property("enabled").toBool());
        scrollSettingsTo("updateServerButton"); click("updateServerButton");
        auto *popup = object("updateServerPopup"); QVERIFY(popup);
        QTRY_VERIFY(popup->property("visible").toBool());
        auto *input = item("updateServerIpInput"); QVERIFY(input);
        auto *port = object("updateServerPortInput"); QVERIFY(port);
        QCOMPARE(port->property("text").toString(), QStringLiteral("18102"));
        QTest::qWait(180); click("updateServerIpInput");
        const auto clipboard = QApplication::clipboard()->text();
        QApplication::clipboard()->setText(QStringLiteral("127.0.0.1"));
        QTest::keyClick(m_window, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(input->property("text").toString(), QStringLiteral("127.0.0.1"));
        QApplication::clipboard()->setText(clipboard);
        QVERIFY(update->property("feedUrl").toString().isEmpty());
        click("saveUpdateServerButton");
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(update->property("feedUrl").toString(), QStringLiteral("http://127.0.0.1:18102/updater"));
        QCOMPARE(setting("update/server").toString(), QStringLiteral("http://127.0.0.1:18102"));
        QVERIFY(!update->property("busy").toBool());

        click("updateServerButton"); QTRY_VERIFY(popup->property("visible").toBool());
        input->setProperty("text", QStringLiteral("999.1.1.1"));
        QTest::qWait(180); click("saveUpdateServerButton");
        QVERIFY(popup->property("visible").toBool());
        QCOMPARE(update->property("serverIp").toString(), QStringLiteral("127.0.0.1"));
        input->setProperty("text", QString()); click("saveUpdateServerButton");
        QTRY_VERIFY(!popup->property("visible").toBool());
        QVERIFY(!hasSetting("update/server"));
        QVERIFY(!check->property("enabled").toBool());
        QCOMPARE(update->property("state").toString(), QStringLiteral("unconfigured"));

        m_window->resize(640, 440); scrollSettingsTo("updateServerButton");
        click("updateServerButton"); QTRY_VERIFY(popup->property("visible").toBool());
        m_controller->setTheme("light"); QTest::qWait(300);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-update-server-light.png"));
        m_controller->setTheme("dark"); QTest::qWait(300);
        QVERIFY(m_window->grabWindow().save(".tmp/screenshots/qml-update-server-dark.png"));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
};
int main(int argc,char **argv) {
    qputenv("QT_QUICK_BACKEND","software");
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR", (qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts").toLocal8Bit());
#endif
    QApplication app(argc,argv); QQuickStyle::setStyle("Basic");
#ifdef Q_OS_WIN
    QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei", 10));
#endif
    auto testArguments = app.arguments();
    testArguments.removeAll("--disable-gpu-acceleration");
    QmlTests tests; return QTest::qExec(&tests,testArguments);
}
#include "QmlTests.moc"
