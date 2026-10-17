#include "AppController.h"
#include "RecordModel.h"
#include "NetworkBundle.h"
#include "PluginRuntime.h"
#include <ztgui/PluginPage.h>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSettings>
#include <QTemporaryDir>
#include <QDir>
#include <QClipboard>
#include <QJsonArray>
#include <QFontDatabase>
#include <QIcon>
#include <QTcpServer>
#include <QTcpSocket>
#include <ztprotocol/WireProtocol.h>
#include <QSignalSpy>
#include <QtTest>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif
using namespace vusiif::ztgui::ui;
using namespace vusiif::ztgui::api;

class DeferredOperations : public SystemOperations {
public:
    using SystemOperations::SystemOperations;
    QString script;
    std::function<void(bool)> completion;
    void runElevatedScript(const QString &, const QString &value, std::function<void(bool)> finished) override { script = value; completion = std::move(finished); }
    void complete(bool ok) { auto done = std::move(completion); if (done) done(ok); }
};
class QmlTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_directory;
    QByteArray m_path, m_programData;
    QStringList m_errors;
    std::unique_ptr<AppController> m_controller;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
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
    void openWindow(const QString &pluginDirectory = {}) {
        m_controller = std::make_unique<AppController>(nullptr,pluginDirectory);
        m_engine = std::make_unique<QQmlApplicationEngine>();
        connect(m_engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) { for (const auto &error : errors) m_errors.append(error.toString()); });
        m_engine->setInitialProperties({{"backend", QVariant::fromValue(m_controller.get())}});
        m_engine->load(QUrl("qrc:/qt/qml/ZTGUI/qml/Main.qml"));
        QVERIFY2(!m_engine->rootObjects().isEmpty(), qPrintable(m_errors.join('\n')));
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
        QVERIFY(m_window); m_controller->setWindow(m_window);
        QTest::qWait(100);
        QVERIFY2(m_errors.isEmpty(), qPrintable(m_errors.join('\n')));
    }
    void click(const char *name) {
        auto *item = qobject_cast<QQuickItem *>(object(name)); QVERIFY(item);
        QTest::mouseClick(m_window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint());
    }
private slots:
    void initTestCase() {
        QVERIFY(m_directory.isValid());
        QCoreApplication::setOrganizationName("ZTGUI-QML-Tests"); QCoreApplication::setApplicationName("QmlTests");
        QCoreApplication::setApplicationVersion(ZEROTIER_GUI_VERSION);
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_directory.path());
        m_path = qgetenv("PATH"); m_programData = qgetenv("ProgramData");
        qputenv("PATH", QByteArray(ZTGUI_FAKE_PING_DIRECTORY) + QDir::listSeparator().toLatin1() + QCoreApplication::applicationDirPath().toLocal8Bit() + QDir::listSeparator().toLatin1() + m_path);
        qputenv("ProgramData", m_directory.path().toLocal8Bit());
        qputenv("ZT_TEST_DATA_DIRECTORY", (m_directory.path()+"/ZeroTier").toLocal8Bit());
        QDir().mkpath("screenshots");
    }
    void init() { qputenv("ZT_TEST_MODE", "compact-lists"); m_errors.clear(); }
    void trayAndMinimizePauseUiPollingAndRefreshOnRestore() {
        const auto path=m_directory.filePath("tray-queries.log"); QFile::remove(path);
        qputenv("ZT_TEST_QUERY_LOG",path.toUtf8());
        const auto queries=[&] { QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); };
        openWindow(); QTRY_COMPARE(m_controller->networks()->count(),2);
        const auto old=m_controller->networks()->get(0); m_controller->setExpanded(old["rowId"].toString(),true);
        m_window->hide(); QTRY_VERIFY(!m_window->isVisible()); QTest::qWait(200);
        for(auto *poller:m_controller->findChildren<vusiif::ztgui::zerotier::JsonPoller *>()) QVERIFY(!poller->isActive());
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
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,vusiif::ztgui::protocol::NetworkPort));
        int requests=0; bool respond=false; QPointer<QTcpSocket> waiting;
        connect(&server,&QTcpServer::newConnection,this,[&] {
            auto *socket=server.nextPendingConnection(); auto input=std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::readyRead,socket,[&,socket,input] {
                *input+=socket->readAll(); vusiif::ztgui::protocol::Packet packet;
                if(vusiif::ztgui::protocol::takeFrame(*input,packet)!=vusiif::ztgui::protocol::ReadResult::Packet) return;
                const auto request=QJsonDocument::fromJson(packet.payload).object(); ++requests;
                if(!respond) { waiting=socket; return; }
                const QJsonArray members{QJsonObject{{"nodeId","0123456789"},{"ipAssignments",QJsonArray{"127.0.0.3"}}}};
                socket->write(vusiif::ztgui::protocol::frame("builtin",QJsonDocument(QJsonObject{{"v",1},{"id",request["id"]},{"ok",true},{"data",members}}).toJson(QJsonDocument::Compact)));
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
        QTemporaryDir packages; const auto path=packages.filePath("invalid.ztplugin");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("invalid package"); file.close();
        openWindow(packages.path()); QTRY_COMPARE(m_controller->plugins().size(),1);
        QTRY_VERIFY(!m_controller->plugins().first().toMap()["loading"].toBool());
        QTRY_VERIFY(object("pluginNoticeDialog") && object("pluginNoticeDialog")->property("visible").toBool());
        QTest::keyClick(m_window,Qt::Key_Escape); QTRY_VERIFY(!object("pluginNoticeDialog")->property("visible").toBool());
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
#ifdef ZTGUI_UI_VOICE_PACKAGE
        QTemporaryDir packages; QVERIFY(QFile::copy(ZTGUI_UI_VOICE_PACKAGE,packages.filePath("voice.ztplugin")));
        openWindow(packages.path());
        QTRY_VERIFY_WITH_TIMEOUT(object("pluginNoticeDialog") && object("pluginNoticeDialog")->property("visible").toBool(),10000);
        QVERIFY(object("pluginNoticeDialog")->property("message").toString().contains("media"));
        QVERIFY(m_controller->pluginRegistry()->plugins().isEmpty());
        for(const auto &v:m_controller->plugins()) { QVERIFY(!v.toMap()["loaded"].toBool()); QVERIFY(!v.toMap()["loading"].toBool()); }
        QTest::keyClick(m_window,Qt::Key_Escape); QTRY_VERIFY(!object("pluginNoticeDialog")->property("visible").toBool());
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
#else
        QSKIP("Voice package required");
#endif
    }
    void serviceDependenciesBlockUnload_data() {
        QTest::addColumn<bool>("dark"); QTest::newRow("light")<<false; QTest::newRow("dark")<<true;
    }
    void serviceDependenciesBlockUnload() {
#ifndef ZTGUI_UI_MEDIA_PACKAGE
        QSKIP("Configure the independently built media and voice packages for this integration check");
#else
        QFETCH(bool,dark);
        QTemporaryDir packages; QVERIFY(QFile::copy(ZTGUI_UI_VOICE_PACKAGE,packages.filePath("voice.ztplugin")));
        QVERIFY(QFile::copy(ZTGUI_UI_MEDIA_PACKAGE,packages.filePath("media.ztplugin")));
        int ticks=0; QTimer heartbeat; heartbeat.setInterval(5); connect(&heartbeat,&QTimer::timeout,this,[&] { ++ticks; }); heartbeat.start();
        openWindow(packages.path());
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->pluginRegistry()->plugins().size(),2,80000);
        QTRY_COMPARE(m_controller->pluginRegistry()->preparationState("voice-chat"),vusiif::ztgui::plugins::PreparationState::Ready);
        click("settingsButton"); QTest::qWait(200);
        if(dark) { click("themeSelector"); QTRY_VERIFY(m_controller->dark()); QTest::qWait(250); }
        click("openPluginsButton"); QTRY_VERIFY(object("pluginDialog") && object("pluginDialog")->property("visible").toBool()); QTest::qWait(200);
        click("pluginRow-media"); QVERIFY(!object("pluginAboutDialog")); QVERIFY(object("pluginRow-media")->property("highlighted").toBool());
        auto *selected=qobject_cast<QQuickItem *>(object("pluginRow-media")); QVERIFY(selected);
        QTest::mouseClick(m_window,Qt::RightButton,{},selected->mapToScene(QPointF(20,18)).toPoint()); QTest::qWait(180);
        QVERIFY(!object("pluginLoad-media")->property("visible").toBool()); QVERIFY(!object("pluginDelete-media")->property("visible").toBool());
        click("pluginAbout-media"); QTRY_VERIFY(object("pluginAboutDialog") && object("pluginAboutDialog")->property("visible").toBool()); QTest::qWait(200);
        QVERIFY(object("pluginServicesLabel")->property("text").toString().contains("media.audio"));
        QVERIFY(m_window->grabWindow().save(dark?"screenshots/plugin-services-dark.png":"screenshots/plugin-services-light.png"));
        QTest::keyClick(m_window,Qt::Key_Escape); QTRY_VERIFY(!object("pluginAboutDialog")->property("visible").toBool());
        m_controller->loadPlugin("MEDIA"); QTRY_VERIFY(object("pluginNoticeDialog") && object("pluginNoticeDialog")->property("visible").toBool());
        QVERIFY(object("pluginNoticeDialog")->property("message").toString().contains(QStringLiteral("同 ID")));
        QCOMPARE(m_controller->pluginRegistry()->plugins().size(),2);
        QTest::keyClick(m_window,Qt::Key_Escape); QTRY_VERIFY(!object("pluginNoticeDialog")->property("visible").toBool());
        const auto unloadFromMenu=[this] {
            auto *row=qobject_cast<QQuickItem *>(object("pluginRow-media")); QVERIFY(row);
            QTest::mouseClick(m_window,Qt::RightButton,{},row->mapToScene(QPointF(row->width()/2,row->height()/2)).toPoint());
            QTest::qWait(160); click("pluginUnload-media");
        };
        unloadFromMenu(); QTRY_VERIFY(object("pluginNoticeDialog") && object("pluginNoticeDialog")->property("visible").toBool());
        QVERIFY(object("pluginNoticeDialog")->property("message").toString().contains("voice-chat"));
        QTest::qWait(180); QVERIFY(m_window->grabWindow().save(dark?"screenshots/plugin-dependency-dark.png":"screenshots/plugin-dependency-light.png"));
        QTest::keyClick(m_window,Qt::Key_Escape); QTRY_VERIFY(!object("pluginNoticeDialog")->property("visible").toBool());
        QCOMPARE(m_controller->pluginRegistry()->plugins().size(),2);
        m_controller->unloadPlugin("voice-chat"); QTRY_VERIFY_WITH_TIMEOUT(m_controller->pluginRegistry()->plugins().isEmpty(),10000);
        QVERIFY(ticks>5); QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
        QSignalSpy exited(m_controller.get(),&AppController::exitFinished); m_controller->requestQuit(); QTRY_COMPARE(exited.size(),1);
#endif
    }
    void mediaOnlyUnloadMemory() {
#if defined(Q_OS_WIN) && defined(ZTGUI_UI_MEDIA_PACKAGE)
        if(!qEnvironmentVariableIntValue("ZTGUI_TEST_MEDIA_MEMORY")) QSKIP("Set ZTGUI_TEST_MEDIA_MEMORY=1 for the repeated physical worker memory check");
        QTemporaryDir packages; openWindow(packages.path()); QTest::qWait(500);
        QFile file("screenshots/media-unload-memory.csv"); QVERIFY(file.open(QIODevice::WriteOnly));
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
                if(name.contains("multimedia") || name.startsWith("avcodec") || name.startsWith("avformat") || name.contains("ztgui_media")) mediaModules.append(name);
            } while(Module32NextW(modules,&module));
            CloseHandle(modules); QVERIFY2(mediaModules.isEmpty(),qPrintable(mediaModules.join(',')));
        };
        measure(0);
        for(int round=1;round<=6;++round) {
            if(round==1) m_controller->importPlugin(QUrl::fromLocalFile(ZTGUI_UI_MEDIA_PACKAGE)); else m_controller->loadPlugin("media");
            QTRY_COMPARE_WITH_TIMEOUT(m_controller->pluginRegistry()->plugins().size(),1,80000);
            QTRY_COMPARE_WITH_TIMEOUT(m_controller->pluginRegistry()->preparationState("media"),vusiif::ztgui::plugins::PreparationState::Ready,80000);
            qint64 pid=0; for(const auto &v:m_controller->plugins()) if(v.toMap()["id"]=="media") pid=v.toMap()["pid"].toLongLong();
            QVERIFY(pid>0); const auto process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(pid)); QVERIFY(process);
            wchar_t image[32768]; DWORD imageSize=32768; QVERIFY(QueryFullProcessImageNameW(process,0,image,&imageSize));
            const auto cache=QFileInfo(QString::fromWCharArray(image,imageSize)).dir().absoluteFilePath("../..");
            m_controller->unloadPlugin("media"); QTRY_VERIFY_WITH_TIMEOUT(m_controller->pluginRegistry()->plugins().isEmpty(),10000);
            QCOMPARE(WaitForSingleObject(process,0),DWORD(WAIT_OBJECT_0)); CloseHandle(process);
            QVERIFY(!QFileInfo::exists(cache));
            const auto runtime=vusiif::ztgui::package::runtimeStatistics(); QCOMPARE(runtime["directories"].toInt(),0); QCOMPARE(runtime["handles"].toInt(),0);
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
        m_engine.reset(); m_window = nullptr; m_controller.reset();
        QSettings().clear(); qunsetenv("ZT_TEST_MODE"); qunsetenv("ZT_TEST_COMMAND_LOG");
        qunsetenv("ZT_TEST_QUERY_LOG");
        qunsetenv("ZT_TEST_PING_MODE"); qunsetenv("ZT_TEST_PING_DELAY"); qunsetenv("ZT_TEST_PING_LOG");
        qunsetenv("ZTGUI_PLUGIN_CREATE_STEPS");
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
        QTRY_COMPARE(m_controller->networks()->count(),2); QTRY_COMPARE(m_controller->peers()->count(),4);
        QTRY_COMPARE(m_controller->info()["address"].toString(),QString("0123456789"));
        QVERIFY(m_window->grabWindow().save("screenshots/qml-light.png"));
        m_controller->setTheme("dark"); QTest::qWait(300);
        QVERIFY(m_controller->dark()); QVERIFY(m_window->grabWindow().save("screenshots/qml-dark.png"));
        m_window->resize(760,520); QTest::qWait(100);
        QVERIFY(m_window->grabWindow().save("screenshots/qml-small.png"));
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void applicationVersionLabelsMatchRootVersion() {
        QFile versionFile(QString::fromUtf8(ZTGUI_SOURCE_VERSION_FILE));
        QVERIFY(versionFile.open(QIODevice::ReadOnly));
        const auto expectedVersion = QString::fromUtf8(versionFile.readAll().trimmed());
        QVERIFY(!expectedVersion.isEmpty());
        openWindow();
        QCOMPARE(m_controller->version(), expectedVersion);
        auto *sidebarVersion = object("sidebarVersionLabel"); QVERIFY(sidebarVersion);
        QCOMPARE(sidebarVersion->property("text").toString(), "v" + expectedVersion);
        click("settingsButton"); QTest::qWait(180);
        click("aboutButton");
        QTRY_VERIFY(object("aboutDialog") && object("aboutDialog")->property("visible").toBool());
        auto *aboutVersion = object("aboutVersionLabel"); QVERIFY(aboutVersion);
        QCOMPARE(aboutVersion->property("text").toString(), "v" + expectedVersion);
        QCOMPARE(m_controller->serverVersion(), QStringLiteral(ZTGUI_SERVER_VERSION));
        QCOMPARE(m_controller->serverApiVersion(), QStringLiteral(ZTGUI_SERVER_API_VERSION));
        auto *aboutServerVersion = object("aboutServerVersionLabel"); QVERIFY(aboutServerVersion);
        QCOMPARE(aboutServerVersion->property("text").toString(),
                 "服务端 " + QStringLiteral(ZTGUI_SERVER_VERSION)
                     + "（API " + QStringLiteral(ZTGUI_SERVER_API_VERSION) + "）");
        QTest::qWait(180);
        QVERIFY(m_window->grabWindow().save("screenshots/qml-application-version.png"));
        m_controller->setTheme("dark"); QTest::qWait(200);
        QCOMPARE(sidebarVersion->property("text").toString(), "v" + expectedVersion);
        QCOMPARE(aboutVersion->property("text").toString(), "v" + expectedVersion);
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
    }
    void themeToggleAndMoonPopupHaveUsableGeometry() {
        openWindow();
        QTRY_VERIFY(m_controller->service()["available"].toBool());
        QVERIFY(object("serviceStatusDot"));
        click("settingsButton"); QTRY_VERIFY(object("settingsPage"));
        QTest::qWait(220); click("themeSelector"); QTRY_VERIFY(m_controller->dark());
        QTest::qWait(250); QVERIFY(m_window->grabWindow().save("screenshots/qml-settings-dark.png"));
        m_controller->navigate(ZeroTierMenuId,qobject_cast<QQuickItem *>(object("pluginContainer"))); QTest::qWait(180);
        const auto dir=QDir(vusiif::ztgui::zerotier::moonDirectory()); QVERIFY(QDir().mkpath(dir.path()));
        QFile file(dir.filePath("0000000123456789.moon")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("moon"); file.close();
        click("moonsButton"); QTRY_VERIFY(m_controller->moonsActive());
        QTRY_VERIFY(m_controller->moons()->count()>0); QTest::qWait(250);
        auto *records=qobject_cast<QQuickItem *>(object("moonRecords")); QVERIFY(records); QVERIFY(records->width()>100);
        QVERIFY(m_window->grabWindow().save("screenshots/qml-moons-dark.png"));
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
        QTRY_VERIFY(object("pluginNoticeDialog") && object("pluginNoticeDialog")->property("visible").toBool());
        QVERIFY(object("pluginNoticeDialog")->property("message").toString().contains(QStringLiteral("重新启动")));
        QTest::keyClick(m_window,Qt::Key_Escape); QTRY_VERIFY(!object("pluginNoticeDialog")->property("visible").toBool());
        QCOMPARE(QQuickWindow::sceneGraphBackend(), backend);
        m_engine.reset(); m_window = nullptr; m_controller.reset();
        openWindow();
        QVERIFY(!m_controller->disableGpuAcceleration());
        click("settingsButton"); QTest::qWait(220);
        QVERIFY(!object("disableGpuAccelerationSwitch")->property("checked").toBool());
        m_controller->setTheme("dark"); m_window->resize(720,480); QTest::qWait(300);
        auto *popup = object("settingsPage"); QVERIFY(popup);
        QVERIFY(popup->property("y").toReal() + popup->property("height").toReal() <= m_window->height());
        QVERIFY(m_window->grabWindow().save("screenshots/qml-gpu-settings-dark-small.png"));
        click("disableGpuAccelerationSwitch"); QTRY_VERIFY(m_controller->disableGpuAcceleration());
        QVERIFY(!object("pluginNoticeDialog") || !object("pluginNoticeDialog")->property("visible").toBool());
        m_controller->setTheme("light"); QTest::qWait(300);
        QVERIFY(m_window->grabWindow().save("screenshots/qml-gpu-settings-light-small.png"));
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
        QVERIFY(!QSettings().contains("rendering/disableGpuAcceleration"));
        QVERIFY(!object("gpuAccelerationHint"));
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
        vusiif::ztgui::zerotier::JsonPoller poller(this,"info",true);
        QSignalSpy availability(&poller,&vusiif::ztgui::zerotier::JsonPoller::availabilityChanged);
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
        QTRY_VERIFY(object("joinNetworkDialog")->property("visible").toBool());
        auto *input=object("networkIdInput"); QVERIFY(input); input->setProperty("text","invalid");
        click("confirmJoinButton"); QVERIFY(m_controller->feedback().contains(QStringLiteral("16 位")));
        qputenv("ZT_TEST_MODE","join-error"); input->setProperty("text","0123456789abcdef");
        click("confirmJoinButton"); QTRY_VERIFY(!m_controller->busy()); QVERIFY(m_controller->feedback().contains("join failed"));
        QVERIFY(object("joinNetworkDialog")->property("visible").toBool());
        qputenv("ZT_TEST_MODE","compact-lists"); click("confirmJoinButton"); QTRY_VERIFY(!object("joinNetworkDialog")->property("visible").toBool());
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
        vusiif::ztgui::plugins::PluginRegistry registry;
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
    void settingsIsMenuAndTooltipWaitsBeforeShowing() {
        openWindow(); auto *container=qobject_cast<QQuickItem *>(object("pluginContainer")); QVERIFY(container);
        QVERIFY(m_controller->navigate(SettingsMenuId,container)); QCOMPARE(m_controller->activeMenu(),QString(SettingsMenuId));
        QVERIFY(object("settingsPage")->property("visible").toBool()); QVERIFY(!object("zeroTierScroll")->property("visible").toBool());
        for(const auto &value:m_controller->menus()) QVERIFY(value.toMap()["id"].toString()!=QString(SettingsMenuId));
        auto *gear=qobject_cast<QQuickItem *>(object("settingsButton")); QVERIFY(gear);
        QTest::mouseMove(m_window,gear->mapToScene(QPointF(gear->width()/2,gear->height()/2)).toPoint());
        auto *tip=object("settingsTooltip"); QVERIFY(tip); QTest::qWait(1000); QVERIFY(!tip->property("visible").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(tip->property("visible").toBool(),1500);
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
        QVERIFY(QFile::copy(FIXTURE_CANCELLABLE,plugins.filePath("test.ztplugin")));
        qputenv("ZTGUI_PLUGIN_CREATE_STEPS","100");
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
        DeferredOperations operations; AppController controller(nullptr,{},&operations);
        const auto dir=QDir(vusiif::ztgui::zerotier::moonDirectory()); QVERIFY(QDir().mkpath(dir.path()));
        QFile file(dir.filePath("0000000123456789.moon")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("moon"); file.close();
        controller.refresh(); QTRY_VERIFY(controller.moons()->count()>0); QSignalSpy confirmation(&controller,&AppController::confirmationRequested);
        controller.deleteMoon("0000000123456789"); QCOMPARE(confirmation.size(),1); controller.confirm(false); QVERIFY(operations.script.isEmpty());
        controller.deleteMoon("0000000123456789"); controller.confirm(true); QVERIFY(!operations.script.isEmpty()); QVERIFY(controller.busy());
        operations.complete(false); QVERIFY(!controller.busy()); QVERIFY(QFileInfo::exists(file.fileName())); QVERIFY(QFile::remove(file.fileName()));
    }
    void closingWaitsForOperationsAndKeepsEventLoopAlive() {
        DeferredOperations operations; AppController controller(nullptr,{},&operations);
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
        QVERIFY(m_window->grabWindow().save("screenshots/qml-guide.png"));
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
                *input+=socket->readAll(); vusiif::ztgui::protocol::Packet packet;
                if (vusiif::ztgui::protocol::takeFrame(*input,packet)!=vusiif::ztgui::protocol::ReadResult::Packet) return;
                const auto request=QJsonDocument::fromJson(packet.payload).object(); if(request["data"].toObject()["purpose"]=="ui-members") ++requests;
                const QJsonArray members{
                    QJsonObject{{"nodeId","012345678b"},{"ipAssignments",QJsonArray{"12.34.56.100"}}},
                    QJsonObject{{"nodeId","012345678a"},{"ipAssignments",QJsonArray{"12.34.56.10"}}},
                    QJsonObject{{"nodeId","0123456789"},{"authorized",true},{"ipAssignments",QJsonArray{requests==1?"12.34.56.2":"12.34.56.3"}}}};
                socket->write(vusiif::ztgui::protocol::frame("builtin",QJsonDocument(QJsonObject{{"v",1},{"id",request["id"]},{"ok",true},{"data",members}}).toJson(QJsonDocument::Compact)));
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
        auto failed = previous[1].toMap(); failed["latency"] = QStringLiteral("不可达"); previous[1] = failed;
        m_controller->networks()->update("0123456789abcdef", {{"members", previous}});
        QTest::qWait(1500); QCOMPARE(requests, 1); // No old two-second polling cycle.
        QTest::qWait(250); m_controller->setTheme("dark"); QTest::qWait(350);
        auto *memberLine=qobject_cast<QQuickItem *>(object("networkMemberRow")); QVERIFY(memberLine);
        QTest::mouseClick(m_window,Qt::RightButton,{},memberLine->mapToScene(QPointF(100,20)).toPoint());
        QTRY_VERIFY(object("networkMemberMenu")); QTest::qWait(200);
        QVERIFY(object("networkMemberMenu")->property("width").toReal()<200);
        QVERIFY(m_window->grabWindow().save("screenshots/qml-member-menu-dark.png"));
        QTest::keyClick(m_window,Qt::Key_Escape); QTest::qWait(150);
        QTRY_VERIFY(object("networkMemberRow")); auto *member=object("networkMemberRow"); member->setProperty("editing",true);
        auto *editor=object("networkMemberRemarkEditor"); QVERIFY(editor); QTRY_VERIFY(editor->property("visible").toBool()); editor->setProperty("text",QStringLiteral("成员备注"));
        QPointer<QObject> identity=editor;
        QTRY_VERIFY_WITH_TIMEOUT(requests>=2,6000);
        QTRY_COMPARE(m_controller->networks()->record("0123456789abcdef")["members"].toList().first().toMap()["ip"].toString(), QString("12.34.56.3"));
        QCOMPARE(latencyAt(0), QString("37 ms"));
        QCOMPARE(latencyAt(1), QStringLiteral("不可达"));
        QCOMPARE(object("networkMemberRemarkEditor"),identity.data()); QCOMPARE(editor->property("text").toString(),QStringLiteral("成员备注"));
        QTRY_COMPARE(latencyAt(0), QString("12 ms"));
        QTRY_COMPARE(latencyAt(1), QString("12 ms"));
        QTest::keyClick(m_window,Qt::Key_Return);
        QTRY_COMPARE(QSettings().value("names/peers/0123456789").toString(),QStringLiteral("成员备注"));
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
        const auto marker=directory.filePath("started"); qputenv("ZTGUI_PLUGIN_PREPARATION_STARTED_FILE",marker.toUtf8()); qputenv("ZTGUI_PLUGIN_CREATE_STEPS","1000");
        openWindow(directory.path()); QTRY_VERIFY(QFileInfo::exists(marker));
        QSignalSpy exited(m_controller.get(),&AppController::exitFinished);
        int heartbeats=0; QTimer timer; connect(&timer,&QTimer::timeout,this,[&] { ++heartbeats; }); timer.start(10);
        QElapsedTimer elapsed; elapsed.start(); m_controller->requestQuit(); QVERIFY(elapsed.elapsed()<50); QTRY_COMPARE_WITH_TIMEOUT(exited.size(),1,2000); if(library==QString::fromUtf8(FIXTURE_CANCELLABLE_BLOCKED)) QVERIFY(heartbeats>0); QVERIFY(m_controller->closing());
        qunsetenv("ZTGUI_PLUGIN_PREPARATION_STARTED_FILE"); qunsetenv("ZTGUI_PLUGIN_CREATE_STEPS");
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
    void windowsCaptionTrayAndRestore() {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName()=="offscreen") QSKIP("Native Windows tray requires the windows platform");
        openWindow(); QSignalSpy requested(m_controller.get(),&AppController::trayRequested);
        auto *tray=object("systemTrayIcon"); QVERIFY(tray);
        QVERIFY(tray->property("visible").toBool());
        QVERIFY(tray->property("available").toBool());
        QVERIFY(!QIcon(":/assets/app-icon.png").isNull());
        SendMessageW(reinterpret_cast<HWND>(m_window->winId()),WM_SYSCOMMAND,SC_CLOSE,1);
        QTRY_COMPARE(requested.size(),1); QTRY_VERIFY(!m_window->isVisible() || m_window->windowState()==Qt::WindowMinimized);
        m_controller->restoreWindow(); QTRY_VERIFY(m_window->isVisible()); QVERIFY(m_window->windowState()!=Qt::WindowMinimized);
        QSignalSpy exited(m_controller.get(),&AppController::exitFinished); m_controller->requestQuit(); QTRY_COMPARE(exited.size(),1);
#else
        QSKIP("Windows-only caption behavior");
#endif
    }
    void configurationImportOnlyJoinsAfterSuccessfulCopy() {
        DeferredOperations operations; AppController controller(nullptr,{},&operations);
        const auto bytes=vusiif::ztgui::zerotier::bundle::encode({"0123456789abcdef","0000000123456789.moon",QByteArray("signed-moon-bytes")});
        QVERIFY(!bytes.isEmpty()); QFile source(m_directory.filePath("network.ztbundle")); QVERIFY(source.open(QIODevice::WriteOnly)); source.write(bytes); source.close();
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
        QVERIFY(!QSettings().contains("names/networks/"+id));
        QVERIFY(QMetaObject::invokeMethod(records,"edit",Q_ARG(QVariant,id),Q_ARG(QVariant,"Saved")));
        QTRY_VERIFY(object("remarkEditor")); QTest::keyClick(m_window,Qt::Key_Return);
        QTRY_COMPARE(QSettings().value("names/networks/"+id).toString(),QString("Saved"));
        click("joinNetworkButton"); QTRY_VERIFY(object("joinNetworkDialog")); QTest::keyClick(m_window,Qt::Key_Escape);
        click("joinNetworkButton"); QTRY_VERIFY(object("joinNetworkDialog")->property("visible").toBool());
        QVERIFY2(m_errors.isEmpty(),qPrintable(m_errors.join('\n')));
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
