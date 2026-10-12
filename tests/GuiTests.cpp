#include "AppStyle.h"
#include "DataTable.h"
#include "MainWindow.h"
#include "ZeroTierClient.h"
#include "tabs/NetworkTab.h"
#include "tabs/MoonTab.h"
#include "AppLog.h"
#include "UiTransitions.h"
#include "NetworkBundle.h"
#include "MacPlatform.h"
#include "tabs/PeersTab.h"
#include "tabs/ServiceTab.h"
#include "plugins/PluginRegistry.h"
#include "plugins/HostServicesImpl.h"

#include <QApplication>
#include <QAbstractButton>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QLabel>
#include <QToolButton>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QPainter>
#include <QStyleOption>
#include <QWheelEvent>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QUrl>
#include <QtTest>
#include <stdexcept>
#include <thread>

// Only these mocks run service commands; file operations use a temporary ProgramData.
using namespace vusiif::ztgui::api;
using namespace vusiif::ztgui::util;
using namespace vusiif::ztgui::plugins;
using namespace vusiif::ztgui::ui;
using vusiif::ztgui::zerotier::JsonPoller;
static QString serviceMocks()
{
#ifdef Q_OS_MACOS
    return "launchctl() { if [ \"$1\" = bootout ]; then return 1; fi; "
        "if [ \"$1\" = kickstart ] && [ \"${ZT_TEST_START_FAILURE:-0}\" = 1 ]; then return 1; fi; return 0; }\n";
#else
    return "function Stop-Service { throw 'stop error to ignore' }\n"
        "function Start-Service { if ($env:ZT_TEST_START_FAILURE -eq '1') { throw 'start failed' }; Write-Output 'start executed' }\n"
        "function Get-Service { $s = New-Object PSObject; Add-Member -InputObject $s -MemberType ScriptMethod -Name WaitForStatus -Value {}; return $s }\n";
#endif
}

class ScriptWindow : public MainWindow {
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
        if (finished) finished(succeeded);
    }
};

class DeferredScriptWindow : public MainWindow {
public:
    vusiif::ztgui::zerotier::ServiceStatus status {true, false, true};
    vusiif::ztgui::zerotier::ServiceStatus serviceStatus() const override { return status; }
    std::function<void(bool)> completion;
    int operations = 0;
    void runElevatedScript(const QString &, const QString &, std::function<void(bool)> finished) override
    {
        ++operations;
        completion = std::move(finished);
    }
    void complete(bool ok)
    {
        auto finished = std::move(completion);
        if (finished) finished(ok);
    }
};

class GuiTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_settings;
    QByteArray m_oldPath;
    QByteArray m_oldProgramData;
    static QList<TableRow> rows(int count = 60) {
        QList<TableRow> result;
        for (int i = 0; i < count; ++i) {
            const auto id = QString("%1").arg(i, 16, 16, QChar('0'));
            result.append({id, {id, "已连接", QString("10.0.0.%1").arg(i)},
                {{"id", id}, {"routes", QJsonArray{QJsonObject{{"target", "10.0.0.0/24"}}}}, {"unknown", 42}}});
        }
        return result;
    }
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
        vusiif::ztgui::ui::applyAppStyle(*qApp);
    }
    void cleanup() {
        qunsetenv("ZT_TEST_MODE");
        qunsetenv("ZT_TEST_WINGET_CODE");
        qunsetenv("ZT_TEST_START_FAILURE");
        qunsetenv("ZT_TEST_COMMAND_LOG");
        QSettings().clear();
        vusiif::ztgui::ui::applyAppStyle(*qApp);
    }
    void cleanupTestCase() {
        qputenv("PATH", m_oldPath); qputenv("ProgramData", m_oldProgramData);
        qunsetenv("ZT_TEST_DATA_DIRECTORY");
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

    void closingDuringPluginPreparationKeepsUiResponsive_data() {
        QTest::addColumn<QString>("library"); QTest::addColumn<QString>("startedMarker");
        QTest::newRow("cooperative") << QString::fromUtf8(FIXTURE_CANCELLABLE) << QString("started:test.cancellable");
        QTest::newRow("blocking-step") << QString::fromUtf8(FIXTURE_CANCELLABLE_BLOCKED) << QString("started:test.cancellable_blocked");
    }
    void closingDuringPluginPreparationKeepsUiResponsive() {
        QFETCH(QString, library); QFETCH(QString, startedMarker);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto tracePath = directory.filePath("shutdown.txt");
        qputenv("ZTGUI_PLUGIN_LIFETIME_FILE", tracePath.toUtf8());
        qputenv("ZTGUI_PLUGIN_CREATE_STEPS", "1000");
        const bool oldQuit = QApplication::quitOnLastWindowClosed();
        QApplication::setQuitOnLastWindowClosed(false);
        const auto restore = qScopeGuard([&] {
            QApplication::setQuitOnLastWindowClosed(oldQuit);
            qunsetenv("ZTGUI_PLUGIN_LIFETIME_FILE");
            qunsetenv("ZTGUI_PLUGIN_CREATE_STEPS");
        });
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        const auto trace = [&] { QFile file(tracePath); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
        {
            MainWindow window(nullptr, directory.path()); window.show();
            QTRY_VERIFY(trace().contains(startedMarker.toUtf8()));
            int ticks = 0; QTimer heartbeat; heartbeat.setInterval(10);
            connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; }); heartbeat.start();
            QElapsedTimer elapsed; elapsed.start();
            QVERIFY(!window.close());
            QVERIFY(elapsed.elapsed() < 100);
            QVERIFY(window.isVisible());
            QVERIFY(!window.centralWidget()->isEnabled());
            auto *status = window.findChild<QStatusBar *>("shutdownStatus");
            QVERIFY(status); QVERIFY(status->currentMessage().contains(QStringLiteral("正在退出")));
            QVERIFY(!window.close()); // Repeated close never starts a second cleanup.
            QVERIFY(!trace().contains("entry"));
            QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 1500);
            if (startedMarker != "started:test.cancellable") QVERIFY(ticks > 2);
        }
        QCOMPARE(trace().count("entry\n"), 1);
        QVERIFY(trace().contains("cancelled:"));
        QCOMPARE(trace().count("destroy\n"), 1);
    }

    void pluginLifecycleNavigationIsOrderedAndRejectsReentry() {
        using namespace vusiif::ztgui::api;
        struct Lifecycle : PluginLifecycleInterface {
            QString name;
            QStringList *events;
            std::function<void()> onEnter;
            std::function<void()> onExit;
            MenuLocation lastPrevious, lastNext;
            void onEnteredFrom(const MenuLocation &previous) override {
                QVERIFY(QThread::currentThread() == qApp->thread());
                lastPrevious = previous;
                events->append(name + ":enter:" + previous.menu.id);
                if (onEnter) onEnter();
            }
            void onExitedTo(const MenuLocation &next) override {
                QVERIFY(QThread::currentThread() == qApp->thread());
                lastNext = next;
                events->append(name + ":exit:" + next.menu.id);
                if (onExit) onExit();
            }
        } alpha, beta;
        QStringList events;
        alpha.name = "A"; beta.name = "B"; alpha.events = beta.events = &events;
        ScriptWindow window;
        auto *registry = window.pluginRegistry();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        MenuContribution menu;
        menu.id = "overview"; menu.title = "Overview";
        menu.createPage = [](QWidget *parent) { return new QLabel("Overview", parent); };
        auto second = menu; second.id = "second"; second.order = 101;
        auto failing = menu; failing.id = "failed"; failing.order = 102;
        failing.createPage = [](QWidget *) -> QWidget * { return nullptr; };
        QVERIFY(registry->registerPlugin({{"test.alpha", "Alpha", "1"}, {menu, second}}, &alpha).ok());
        QVERIFY(registry->registerPlugin({{"test.beta", "Beta", "2"}, {menu, failing}}, &beta).ok());
        auto select = [&](const QString &id) {
            for (int row = 0; row < navigation->count(); ++row)
                if (navigation->item(row)->data(Qt::UserRole) == id) navigation->setCurrentRow(row);
        };
        select("test.alpha/overview");
        QVERIFY(alpha.lastPrevious.builtin);
        QCOMPARE(events, QStringList{"A:enter:builtin/zerotier"});
        alpha.onExit = [&] {
            QCOMPARE(registry->unregisterPlugin("test.beta").error, Error::Busy);
            QCOMPARE(registry->activateMenu(ZeroTierMenuId).error, Error::Busy);
            select(ZeroTierMenuId); // UI reentry must not interrupt A -> B.
        };
        auto *stack = window.findChild<QStackedWidget *>("pages");
        auto *betaPage = registry->page("test.beta/overview", stack).page;
        QVERIFY(betaPage);
        beta.onEnter = [&] { QCOMPARE(stack->currentWidget(), betaPage); };
        select("test.beta/overview");
        QCOMPARE(events.mid(1), (QStringList{"A:exit:test.beta/overview", "B:enter:test.alpha/overview"}));
        QCOMPARE(beta.lastPrevious.plugin.name, QString("Alpha"));
        QCOMPARE(alpha.lastNext.plugin.version, QString("2"));
        QCOMPARE(navigation->currentItem()->data(Qt::UserRole).toString(), QString("test.beta/overview"));
        const auto count = events.size();
        select("test.beta/overview");
        select("test.beta/failed"); // Failed page leaves A/B callbacks untouched.
        QCOMPARE(events.size(), count);
        beta.onExit = [] { throw std::runtime_error("exit failure"); };
        select("test.alpha/overview");
        QCOMPARE(events.last(), QString("A:enter:test.beta/overview"));
        alpha.onExit = {};
        select("test.alpha/second");
        QCOMPARE(events.mid(events.size() - 2),
            (QStringList{"A:exit:test.alpha/second", "A:enter:test.alpha/overview"}));
        QVERIFY(registry->unregisterPlugin("test.alpha").ok());
        QCOMPARE(events.last(), QString("A:exit:builtin/zerotier"));
        select("test.beta/overview");
        beta.onExit = {};
        select(ZeroTierMenuId);
        QVERIFY(beta.lastNext.builtin);
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

    void pluginRegistrationIsAtomicAndVersioned() {
        using namespace vusiif::ztgui::api;
        QCOMPARE(ApiMajor, 0);
        QCOMPARE(ApiMinor, 3);
        QCOMPARE(QString::fromLatin1(ApiVersion), QStringLiteral(ZEROTIER_GUI_VERSION).section('.', 0, 1));
        PluginRegistry registry;
        QSignalSpy changes(&registry, &PluginRegistry::menusChanged);
        MenuContribution menu;
        menu.id = "overview";
        menu.title = QStringLiteral("概览");
        menu.createPage = [](QWidget *parent) { return new QWidget(parent); };
        PluginDefinition plugin {{"example.alpha", "Alpha", "1.0"}, {menu}};
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
        invalid.info.id = "example.alpha\n";
        QCOMPARE(registry.registerPlugin(invalid).error, Error::InvalidPlugin);
        invalid = plugin;
        invalid.menus[0].createPage = {};
        QCOMPARE(registry.registerPlugin(invalid).error, Error::InvalidMenu);
        QVERIFY(registry.registerPlugin(plugin).ok());
        QCOMPARE(registry.registerPlugin(plugin).error, Error::DuplicatePlugin);
        QCOMPARE(registry.plugins().size(), 1);
        QCOMPARE(registry.menus().first().id, QString("example.alpha/overview"));
        QCOMPARE(changes.count(), 1);
        plugin.info.id = "example.beta";
        plugin.menus[0].order = 10;
        QVERIFY(registry.registerPlugin(plugin).ok());
        QCOMPARE(registry.menus().first().id, QString("example.beta/overview"));
        QCOMPARE(registry.unregisterPlugin("missing").error, Error::NotFound);
        QCOMPARE(registry.unregisterPlugin("builtin").error, Error::NotFound);
        QCOMPARE(registry.menus().size(), 2);
    }

    void pluginPreparationDisablesNavigationUntilReady() {
        using namespace vusiif::ztgui::api;
        MainWindow window;
        auto *registry = window.pluginRegistry();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        int creations = 0;
        MenuContribution menu;
        menu.id = "overview"; menu.title = "Preparing";
        menu.createPage = [&](QWidget *parent) { ++creations; return new QLabel("Ready", parent); };
        QVERIFY(registry->registerPlugin({{"example.preparing", "Preparing", "1"}, {menu}}).ok());
        QVERIFY(registry->setPreparationState("example.preparing", PreparationState::Preparing, "Preparing...").ok());
        QVERIFY(!(navigation->item(1)->flags() & Qt::ItemIsEnabled));
        QCOMPARE(navigation->item(1)->toolTip(), QString("Preparing..."));
        QCOMPARE(creations, 0);
        QVERIFY(registry->setPreparationState("example.preparing", PreparationState::Failed, "Failed").ok());
        QVERIFY(!(navigation->item(1)->flags() & Qt::ItemIsEnabled));
        QCOMPARE(navigation->item(1)->toolTip(), QString("Failed"));
        QVERIFY(registry->setPreparationState("example.preparing", PreparationState::Ready).ok());
        QVERIFY(navigation->item(1)->flags() & Qt::ItemIsEnabled);
        navigation->setCurrentRow(1);
        QCOMPARE(creations, 1);
    }

    void pluginPagesAreLazyAndNavigationSurvivesRemoval() {
        using namespace vusiif::ztgui::api;
        MainWindow window;
        auto *registry = window.pluginRegistry();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        auto *pages = window.findChild<QStackedWidget *>("pages");
        auto *builtin = pages->currentWidget();
        int creations = 0;
        MenuContribution menu;
        menu.id = "overview";
        menu.title = "Alpha";
        menu.order = 50;
        menu.createPage = [&](QWidget *parent) {
            ++creations;
            auto *label = new QLabel("State", parent);
            return label;
        };
        PluginDefinition plugin {{"example.alpha", "Alpha", "1.0"}, {menu}};
        QVERIFY(registry->registerPlugin(plugin).ok());
        QCOMPARE(creations, 0);
        QCOMPARE(pages->count(), 1);
        QCOMPARE(navigation->count(), 2);
        navigation->setCurrentRow(1);
        QCOMPARE(creations, 1);
        QPointer<QWidget> alpha = pages->currentWidget();
        QCOMPARE(qobject_cast<QLabel *>(alpha.data())->text(), QString("State"));
        qobject_cast<QLabel *>(alpha.data())->setText("Edited");
        navigation->setCurrentRow(0);
        navigation->setCurrentRow(1);
        QCOMPARE(creations, 1);
        QCOMPARE(pages->currentWidget(), alpha.data());
        QCOMPARE(qobject_cast<QLabel *>(alpha.data())->text(), QString("Edited"));
        // Insert a menu before the selected menu. Its row changes, its identity does not.
        plugin.info.id = "example.beta";
        plugin.menus[0].order = 0;
        QVERIFY(registry->registerPlugin(plugin).ok());
        QCOMPARE(navigation->currentRow(), 2);
        QCOMPARE(pages->currentWidget(), alpha.data());
        navigation->setCurrentRow(1);
        QPointer<QWidget> beta = pages->currentWidget();
        QVERIFY(beta != alpha);
        QVERIFY(registry->unregisterPlugin("example.alpha").ok());
        QVERIFY(alpha.isNull());
        QCOMPARE(pages->currentWidget(), beta.data());
        QVERIFY(registry->unregisterPlugin("example.beta").ok());
        QVERIFY(beta.isNull());
        QCOMPARE(pages->currentWidget(), builtin);
        QCOMPARE(navigation->currentRow(), 0);
        QCOMPARE(navigation->count(), 1);
        QCOMPARE(pages->count(), 1);
    }

    void pluginFactoryFailuresCanBeRetriedAndRejectReentry() {
        using namespace vusiif::ztgui::api;
        MainWindow window;
        auto *registry = window.pluginRegistry();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        auto *pages = window.findChild<QStackedWidget *>("pages");
        auto *builtin = pages->currentWidget();
        QSignalSpy failures(registry, &PluginRegistry::pageCreationFailed);
        int attempts = 0;
        Error reentry = Error::None;
        MenuContribution menu;
        menu.id = "overview";
        menu.title = "Retry";
        menu.createPage = [&](QWidget *parent) -> QWidget * {
            reentry = registry->unregisterPlugin("example.retry").error;
            if (++attempts == 1) return nullptr;
            if (attempts == 2) throw std::runtime_error("test failure");
            return new QLabel("Recovered", parent);
        };
        QVERIFY(registry->registerPlugin({{"example.retry", "Retry", "1.0"}, {menu}}).ok());
        for (int attempt = 1; attempt <= 2; ++attempt) {
            navigation->setCurrentRow(1);
            QCOMPARE(reentry, Error::Busy);
            QCOMPARE(attempts, attempt);
            QCOMPARE(navigation->currentRow(), 0);
            QCOMPARE(pages->currentWidget(), builtin);
            QCOMPARE(pages->count(), 1);
            QCOMPARE(failures.count(), attempt);
        }
        navigation->setCurrentRow(1);
        QCOMPARE(attempts, 3);
        QCOMPARE(pages->count(), 2);
        QCOMPARE(registry->page("missing/page", pages).result.error, Error::NotFound);
        QCOMPARE(registry->page("example.retry/overview", nullptr).result.error, Error::PageCreationFailed);
    }

    void pluginPagesDieBeforeFactoryResources() {
        using namespace vusiif::ztgui::api;
        QWidget host;
        QPointer<QWidget> page;
        bool pageWasDestroyedFirst = false;
        const auto addPlugin = [&](PluginRegistry &registry) {
            auto resource = std::shared_ptr<int>(new int(1), [&](int *value) {
                pageWasDestroyedFirst = page.isNull();
                delete value;
            });
            MenuContribution menu;
            menu.id = "overview";
            menu.title = "Lifetime";
            menu.createPage = [resource](QWidget *parent) { return new QWidget(parent); };
            QVERIFY(registry.registerPlugin({{"example.lifetime", "Lifetime", "1.0"}, {menu}}).ok());
            page = registry.page("example.lifetime/overview", &host).page;
            QVERIFY(page);
        };
        {
            PluginRegistry registry;
            addPlugin(registry);
            QVERIFY(!pageWasDestroyedFirst);
            QVERIFY(registry.unregisterPlugin("example.lifetime").ok());
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

    void pluginFactoriesCannotBorrowHostWidgets() {
        using namespace vusiif::ztgui::api;
        PluginRegistry registry;
        QWidget host;
        QWidget otherHost;
        QPointer<QWidget> existing = new QLabel("Existing", &host);
        QWidget *returned = &host;
        MenuContribution menu;
        menu.id = "overview";
        menu.title = "Ownership";
        menu.createPage = [&](QWidget *) { return returned; };
        QVERIFY(registry.registerPlugin({{"example.ownership", "Ownership", "1.0"}, {menu}}).ok());
        QCOMPARE(registry.page("example.ownership/overview", &host).result.error, Error::PageCreationFailed);
        returned = existing;
        QCOMPARE(registry.page("example.ownership/overview", &host).result.error, Error::PageCreationFailed);
        QVERIFY(existing);
        returned = new QWidget(&host);
        // It was created before the factory ran, so it is still an existing host widget.
        QCOMPARE(registry.page("example.ownership/overview", &host).result.error, Error::PageCreationFailed);
        QVERIFY(registry.unregisterPlugin("example.ownership").ok());
        QVERIFY(existing);

        menu.createPage = [](QWidget *parent) { return new QWidget(parent); };
        QVERIFY(registry.registerPlugin({{"example.ownership", "Ownership", "1.0"}, {menu}}).ok());
        QPointer<QWidget> created = registry.page("example.ownership/overview", &host).page;
        QVERIFY(created);
        QCOMPARE(registry.page("example.ownership/overview", &otherHost).result.error, Error::PageCreationFailed);
        QVERIFY(created);
        delete created.data();
        QVERIFY(registry.page("example.ownership/overview", &host).result.ok());
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
            creation = registry.page("missing/page", nullptr);
        });
        worker.join();
        QCOMPARE(registration.error, Error::WrongThread);
        QCOMPARE(removal.error, Error::WrongThread);
        QCOMPARE(creation.result.error, Error::WrongThread);
        QVERIFY(registry.plugins().isEmpty());
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

    void unreachableMembersAreFilteredAndRecover() {
        qputenv("ZT_TEST_MODE", "peer-reachable");
        PeersTab peers(nullptr);
        peers.show();
        auto *tree = peers.tree();
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 5, 2500);
        QCOMPARE(tree->topLevelItem(tree->topLevelItemCount() - 1)->data(0, Qt::UserRole).toString(), QString("0000000004"));
        qputenv("ZT_TEST_MODE", "peer-filter");
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 4, 3000);
        QStringList ids;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) ids << tree->topLevelItem(i)->data(0, Qt::UserRole).toString();
        QVERIFY(!ids.contains("0000000001"));
        QVERIFY(ids.contains("0000000002")); // A zero latency LEAF remains visible.
        QVERIFY(ids.contains("0000000003"));
        QVERIFY(ids.contains("0000000004")); // Unreachable PLANET/MOON remain visible.
        QVERIFY(ids.contains("0000000005"));
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            const auto *item = tree->topLevelItem(i);
            QCOMPARE(item->text(2), item->data(0, Qt::UserRole).toString() == "0000000003" ? QStringLiteral("直链")
                : item->data(0, Qt::UserRole).toString() == "0000000002" ? QStringLiteral("中转") : QStringLiteral("不可达"));
        }
        // An active path must not override the negative latency of PLANET 4.
        QVERIFY(QDir().mkpath("screenshots"));
        peers.resize(900, 340);
        tree->clearSelection();
        for (const QString &theme : {QString("light"), QString("dark")}) {
            QSettings().setValue("appearance/theme", theme);
            vusiif::ztgui::ui::applyAppStyle(*qApp);
            QTest::qWait(50);
            const auto image = tree->viewport()->grab().toImage();
            bool foundRed = false;
            const int left = tree->header()->sectionViewportPosition(2);
            for (int y = 0; y < image.height() && !foundRed; ++y)
                for (int x = qMax(0, left); x < qMin(image.width(), left + tree->columnWidth(2)); ++x) {
                    const QColor color = image.pixelColor(x, y);
                    if (color.red() > color.green() + 45 && color.red() > color.blue() + 45) { foundRed = true; break; }
                }
            QVERIFY(foundRed);
            QVERIFY(peers.grab().save("screenshots/unreachable-" + theme + ".png"));
        }
        qputenv("ZT_TEST_MODE", "peer-reachable");
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 5, 3000);
    }

    void peerMenuCopiesIdAndEditsRemark() {
        PeersTab peers(nullptr);
        peers.show();
        QTRY_COMPARE_WITH_TIMEOUT(peers.tree()->topLevelItemCount(), 35, 2500);
        auto *item = peers.tree()->topLevelItem(0);
        const QString id = item->data(0, Qt::UserRole).toString();
        QTimer::singleShot(80, &peers, [&] {
            auto *menu = peers.findChild<QMenu *>("peerMenu");
            QVERIFY(menu);
            QCOMPARE(menu->actions().size(), 2);
            QCOMPARE(menu->actions()[0]->text(), id);
            QCOMPARE(menu->actions()[1]->text(), QStringLiteral("修改备注"));
            menu->actions()[0]->trigger(); menu->close();
        });
        peers.tree()->customContextMenuRequested(peers.tree()->visualItemRect(item).center());
        QCOMPARE(QApplication::clipboard()->text(), id);
        peers.editRemark();
        auto *editor = peers.findChild<QLineEdit *>("remarkEditor");
        QVERIFY(editor);
        QCOMPARE(editor->text(), id);
        editor->setText(QStringLiteral("测试成员"));
        QTest::qWait(2200);
        QCOMPARE(editor->text(), QStringLiteral("测试成员"));
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(item->text(0), QStringLiteral("测试成员"));
        QTest::qWait(2200);
        QCOMPARE(item->text(0), QStringLiteral("测试成员"));
        QCOMPARE(item->text(2), QStringLiteral("直链"));
    }

    void joinDialogImportsMoonWithoutJoining() {
        ScriptWindow window;
        window.show();
        window.findChild<QListWidget *>("navigation")->setCurrentRow(0);
        auto *network = window.findChild<NetworkTab *>();
        QTemporaryDir sources;
        const QString source = sources.filePath("000000abcdef1234.moon");
        QFile fixture(source);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("signed import fixture"); fixture.close();
        network->showJoinDialog();
        auto *join = network->findChild<QDialog *>("joinNetworkDialog");
        auto *input = join->findChild<QLineEdit *>("networkIdInput");
        input->setText("abcdef1234000001");
        QTimer::singleShot(80, &window, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->selectFile(source);
            QMetaObject::invokeMethod(dialog, "accept");
        });
        join->findChild<QPushButton *>("importMoonButton")->click();
        QVERIFY(window.succeeded);
        QCOMPARE(window.operations, 1);
        const QString installed = QDir(vusiif::ztgui::zerotier::homeDirectory()).filePath("moons.d/000000abcdef1234.moon");
        QFile output(installed);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), QByteArray("signed import fixture"));
        output.close();
        QCOMPARE(input->text(), QString("abcdef1234000001"));
        QVERIFY(join->isVisible());
        QVERIFY(join->isEnabled());
        QCOMPARE(vusiif::ztgui::zerotier::matchingMoonFile(input->text()), QFileInfo(installed).absoluteFilePath());
        QVERIFY(QFile::remove(installed));
        join->reject();
        QTRY_VERIFY(!network->findChild<QDialog *>("joinNetworkDialog"));
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

    void bundleImportConfiguresAndJoins_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("success") << QString("success");
        QTest::newRow("service-failure") << QString("service-failure");
        QTest::newRow("join-failure") << QString("join-failure");
        QTest::newRow("replace-canceled") << QString("replace-canceled");
    }

    void bundleImportConfiguresAndJoins() {
        QFETCH(QString, mode);
        ScriptWindow window;
        window.show();
        window.findChild<QListWidget *>("navigation")->setCurrentRow(0);
        auto *network = window.findChild<NetworkTab *>();
        QTemporaryDir sources;
        const QString id = "abcdef5678000001";
        const QString moonName = "000000abcdef5678.moon";
        const QByteArray signedMoon("signed\0moon contents", 20);
        const QString package = sources.filePath("network.ztbundle");
        QFile fixture(package);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write(vusiif::ztgui::zerotier::bundle::encode({id, moonName, signedMoon})); fixture.close();
        const QString directory = QDir(vusiif::ztgui::zerotier::homeDirectory()).filePath("moons.d");
        QVERIFY(QDir().mkpath(directory));
        const QString installed = QDir(directory).filePath(moonName);
        const auto cleanupMoon = qScopeGuard([installed] { QFile::remove(installed); });
        if (mode == "replace-canceled") {
            QFile existing(installed);
            QVERIFY(existing.open(QIODevice::WriteOnly));
            existing.write("existing moon"); existing.close();
        }
        const QString logPath = sources.filePath("joins.log");
        qputenv("ZT_TEST_COMMAND_LOG", logPath.toLocal8Bit());
        if (mode == "service-failure") qputenv("ZT_TEST_START_FAILURE", "1");
        if (mode == "join-failure") qputenv("ZT_TEST_MODE", "join-error");
        network->showJoinDialog();
        auto *join = network->findChild<QDialog *>("joinNetworkDialog");
        QString warning;
        QTimer dismissMessages;
        connect(&dismissMessages, &QTimer::timeout, &window, [&] {
            if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                warning = message->text();
                const auto answer = message->standardButtons().testFlag(QMessageBox::Yes)
                    ? (mode == "replace-canceled" ? QMessageBox::No : QMessageBox::Yes) : QMessageBox::Ok;
                if (auto *button = message->button(answer)) button->click();
            }
        });
        dismissMessages.start(30);
        network->importConfigurationFile(package);
        QFile moonFile(installed);
        QVERIFY(moonFile.open(QIODevice::ReadOnly));
        QCOMPARE(moonFile.readAll(), mode == "replace-canceled" ? QByteArray("existing moon") : signedMoon);
        moonFile.close();
        if (mode == "success") {
            QTRY_VERIFY_WITH_TIMEOUT(!network->findChild<QDialog *>("joinNetworkDialog"), 2500);
        } else {
            if (mode == "join-failure") QTRY_VERIFY_WITH_TIMEOUT(!warning.isEmpty(), 2500);
            QVERIFY(join->isVisible());
            QVERIFY(join->isEnabled());
            QCOMPARE(join->findChild<QLineEdit *>("networkIdInput")->text(), id);
            if (mode == "join-failure") QVERIFY(warning.contains(QStringLiteral("中转站已配置，但加入网络失败")));
            join->reject();
        }
        QVERIFY(network->isEnabled());
        QVERIFY(network->moonTab()->isEnabled());
        QCOMPARE(window.operations, mode == "replace-canceled" ? 0 : 1);
        QFile log(logPath);
        if (mode == "success" || mode == "join-failure") {
            QVERIFY(log.open(QIODevice::ReadOnly));
            QCOMPARE(log.readAll().trimmed(), QByteArray("join ") + id.toLatin1());
        } else QVERIFY(!log.exists());
        QVERIFY(QFile::remove(installed));
    }

    void allColumnsRetainManualWidths() {
        DataTable table("enum-sizing", {"ID", "状态", "IP"}, nullptr, false);
        table.setBadgeColumns({1});
        table.resize(700, 250);
        table.show();
        auto *tree = table.tree();
        for (int column = 0; column < tree->columnCount(); ++column)
            QCOMPARE(tree->header()->sectionResizeMode(column), QHeaderView::Interactive);
        tree->setColumnWidth(1, 250);
        table.setRows({{"node", {"node", QStringLiteral("已连接"), "192.0.2.1"}, {}}});
        table.setRows({{"node", {"node", QStringLiteral("客户端版本过旧"), "192.0.2.1"}, {}}});
        QCOMPARE(tree->columnWidth(1), 250);
        DataTable reopened("enum-sizing", {"ID", "状态", "IP"}, nullptr, false);
        reopened.setBadgeColumns({1});
        QCOMPARE(reopened.tree()->columnWidth(1), 250);
        {
            PeersTab peers(nullptr);
            peers.tree()->setColumnWidth(2, 230);
        }
        PeersTab peers(nullptr);
        QCOMPARE(peers.tree()->columnWidth(2), 230);
        {
            NetworkTab networks(nullptr);
            networks.tree()->setColumnWidth(2, 240);
        }
        NetworkTab networks(nullptr);
        QCOMPARE(networks.tree()->columnWidth(2), 240);
    }

    void loadingOverlayCoversPageAndRestoresInteraction() {
        DeferredScriptWindow window;
        window.show();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        auto *service = window.findChild<ServiceTab *>();
        QTRY_COMPARE_WITH_TIMEOUT(service->findChild<QPushButton *>("serviceAction")->text(), QStringLiteral("启动"), 2500);
        auto *start = service->findChild<QPushButton *>("serviceAction");
        start->click();
        auto *overlay = service->findChild<QWidget *>("operationOverlay", Qt::FindDirectChildrenOnly);
        QVERIFY(overlay && overlay->isVisible());
        QVERIFY(!service->isEnabled());
        QVERIFY(navigation->isEnabled());
        start->click();
        QCOMPARE(window.operations, 1);
        QTest::qWait(230);
        const auto before = overlay->grab().toImage();
        QTest::qWait(100);
        QVERIFY(overlay->grab().toImage() != before);

        window.complete(false);
        QTRY_VERIFY(!overlay->isVisible());
        QVERIFY(service->isEnabled());
        QVERIFY(start->isEnabled());
        navigation->setCurrentRow(0);
        auto *network = window.findChild<NetworkTab *>();
        network->showJoinDialog();
        auto *dialog = network->findChild<QDialog *>("joinNetworkDialog");
        auto *input = dialog->findChild<QLineEdit *>("networkIdInput");
        input->setText("8056c2e21c000001");
        qputenv("ZT_TEST_MODE", "slow-command");
        QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(!input->isEnabled());
        auto *networkOverlay = network->findChild<QWidget *>("operationOverlay", Qt::FindDirectChildrenOnly);
        QVERIFY(networkOverlay && networkOverlay->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(network->isEnabled(), 2500);
        QTRY_VERIFY(!networkOverlay->isVisible());
        QTRY_VERIFY(!network->findChild<QDialog *>("joinNetworkDialog"));
    }

    void transitionsHandleRapidSwitchesAndDialogResults() {
        MainWindow window;
        window.show();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        auto *pages = window.findChild<QStackedWidget *>("pages");
        QVERIFY(!window.findChild<QCheckBox *>("animationsToggle"));
        QSettings().setValue("appearance/animations", false);
        QVERIFY(vusiif::ztgui::ui::enabled());
        auto *theme = window.findChild<QAbstractButton *>("themeSwitch");
        theme->click();
        theme->click();
        theme->click();
        QCOMPARE(pages->count(), 1);
        QCOMPARE(pages->currentIndex(), 0);
        auto *central = window.centralWidget();
        QCOMPARE(central->findChildren<QWidget *>("uiTransitionOverlay", Qt::FindDirectChildrenOnly).size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(central->findChildren<QWidget *>("uiTransitionOverlay", Qt::FindDirectChildrenOnly).isEmpty(), 1500);
        AnimatedDialog dialog(&window);
        dialog.show();
        QTest::qWait(220);
        QVERIFY(!dialog.graphicsEffect());
        QCOMPARE(dialog.windowOpacity(), 1.0);
        QCOMPARE(dialog.grab().toImage().pixelColor(2, 2), dialog.palette().color(QPalette::Window));
        QSignalSpy finished(&dialog, &QDialog::finished);
        dialog.accept();
        dialog.reject(); // Repeated actions must not replace the accepted result.
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first()[0].toInt(), int(QDialog::Accepted));
        QVERIFY(!dialog.isVisible());
        dialog.show();
        dialog.close(); // Closing during its opening transition must also finish.
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last()[0].toInt(), int(QDialog::Rejected));
    }

    void pluginPopupListsStatesAndClosesWithoutBlockingInput() {
        MainWindow window;
        window.show();
        auto *gear = window.findChild<QPushButton *>("settingsButton");
        auto *settings = window.findChild<QDialog *>("settingsPopup");
        auto *plugins = window.findChild<QPushButton *>("pluginsButton");
        auto *popup = window.findChild<QDialog *>("pluginsPopup");
        auto *list = popup->findChild<QTreeWidget *>("pluginList");
        auto *empty = popup->findChild<QLabel *>("emptyPluginsHint");
        QSignalSpy finished(popup, &QDialog::finished);
        const auto open = [&] {
            gear->click();
            QVERIFY(settings->isVisible());
            plugins->click();
            QVERIFY(!settings->isVisible());
            QVERIFY(popup->isVisible());
            QCOMPARE(QApplication::activePopupWidget(), popup);
        };
        open();
        QCOMPARE(list->topLevelItemCount(), 0);
        QVERIFY(empty->isVisible());
        popup->findChild<QPushButton *>("closePluginsButton")->click();
        QVERIFY(!popup->isVisible());
        QVERIFY(!QApplication::activePopupWidget());
        QCOMPARE(finished.size(), 1);

        int creations = 0;
        MenuContribution menu;
        menu.id = "overview"; menu.title = "Test";
        menu.createPage = [&](QWidget *parent) { ++creations; return new QLabel("Test", parent); };
        auto *registry = window.pluginRegistry();
        QVERIFY(registry->registerPlugin({{"example.alpha", QStringLiteral("测试插件"), "1.2.3"}, {menu}}).ok());
        QVERIFY(registry->setPreparationState("example.alpha", PreparationState::Preparing).ok());
        open();
        QCOMPARE(list->topLevelItemCount(), 1);
        QVERIFY(!empty->isVisible());
        QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("测试插件"));
        QCOMPARE(list->topLevelItem(0)->text(1), QString("1.2.3"));
        QCOMPARE(list->topLevelItem(0)->text(2), QStringLiteral("初始化中"));
        QCOMPARE(list->topLevelItem(0)->data(0, Qt::UserRole).toString(), QString("example.alpha"));
        QVERIFY(registry->setPreparationState("example.alpha", PreparationState::Failed, "Test failure").ok());
        QCOMPARE(list->topLevelItem(0)->text(2), QStringLiteral("初始化失败"));
        QCOMPARE(list->topLevelItem(0)->toolTip(2), QString("Test failure"));
        QVERIFY(registry->setPreparationState("example.alpha", PreparationState::Ready).ok());
        QCOMPARE(list->topLevelItem(0)->text(2), QStringLiteral("已就绪"));
        QCOMPARE(creations, 0); // Inspecting plugins must not construct their pages.
        QTest::qWait(220);
        QVERIFY(popup->grab().save("screenshots/plugins.png"));
        QTest::keyClick(list, Qt::Key_Escape);
        QVERIFY(!popup->isVisible());
        QVERIFY(!QApplication::activePopupWidget());
        QCOMPARE(finished.size(), 2);
        QTRY_VERIFY(window.findChildren<QWidget *>("dialogClosingFade").isEmpty());

        open();
        QTest::qWait(220);
        // Send through the QWindow path used by native outside-click dismissal.
        const QPoint outside = popup->mapFromGlobal(window.mapToGlobal(QPoint(20, 20)));
        QTest::mouseClick(popup->windowHandle(), Qt::LeftButton, Qt::NoModifier, outside);
        QVERIFY(!popup->isVisible());
        QVERIFY(!QApplication::activePopupWidget());
        QCOMPARE(finished.size(), 3);
        QVERIFY(registry->unregisterPlugin("example.alpha").ok());
        open();
        QCOMPARE(list->topLevelItemCount(), 0);
        popup->close();

        AnimatedDialog modal(&window);
        modal.setModal(true);
        modal.show();
        QCOMPARE(QApplication::activeModalWidget(), &modal);
        QTest::qWait(220);
        modal.accept();
        QVERIFY(!QApplication::activeModalWidget());
        QCOMPARE(modal.result(), int(QDialog::Accepted));
        // Inspect synchronously; QTest keyboard helpers may process the whole fade.
        auto *fade = window.findChild<QWidget *>("dialogClosingFade");
        QVERIFY(fade && fade->isVisible());
        QVERIFY(fade->windowFlags().testFlag(Qt::WindowTransparentForInput));
        QVERIFY(fade->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QTRY_VERIFY(window.findChildren<QWidget *>("dialogClosingFade").isEmpty());
    }

    void pluginListSelectionAndContextAbout() {
        MainWindow window;
        window.show();
        MenuContribution contribution;
        contribution.id = "overview"; contribution.title = "Test";
        contribution.createPage = [](QWidget *parent) { return new QLabel("Test", parent); };
        QVERIFY(window.pluginRegistry()->registerPlugin({{"example.alpha", QStringLiteral("测试插件"), "1.2.3"}, {contribution}}).ok());
        window.findChild<QPushButton *>("settingsButton")->click();
        window.findChild<QPushButton *>("pluginsButton")->click();
        auto *popup = window.findChild<QDialog *>("pluginsPopup");
        auto *list = popup->findChild<QTreeWidget *>("pluginList");
        const auto textPositions = [list] {
            const QImage image = list->viewport()->grab().toImage();
            QList<int> positions;
            for (int column = 0; column < list->columnCount(); ++column) {
                const QRect cell = list->visualRect(list->model()->index(0, column)).intersected(image.rect());
                const QColor background = image.pixelColor(cell.right() - 2, cell.center().y());
                int left = -1;
                for (int x = cell.left(); x <= cell.right() && left < 0; ++x)
                    for (int y = cell.top(); y <= cell.bottom(); ++y)
                        {
                            const QColor pixel = image.pixelColor(x, y);
                            const int red = pixel.red() - background.red(), green = pixel.green() - background.green(), blue = pixel.blue() - background.blue();
                            if (red * red + green * green + blue * blue > 1600) { left = x; break; }
                        }
                positions.append(left);
            }
            return positions;
        };
        for (const QString &theme : {QString("light"), QString("dark")}) {
            QSettings().setValue("appearance/theme", theme);
            applyAppStyle(*qApp);
            list->clearSelection();
            QTest::qWait(220);
            QVERIFY(list->viewport()->grab().save(QStringLiteral("screenshots/plugin-selection-before-%1.png").arg(theme)));
            const auto before = textPositions();
            for (int x : before) QVERIFY(x >= 0);
            QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->topLevelItem(0)).center());
            QVERIFY(list->viewport()->grab().save(QStringLiteral("screenshots/plugin-selection-after-%1.png").arg(theme)));
            const auto after = textPositions();
            for (int column = 0; column < before.size(); ++column)
                QVERIFY2(qAbs(after[column] - before[column]) <= 1, qPrintable(QStringLiteral("%1 column %2: %3 -> %4").arg(theme).arg(column).arg(before[column]).arg(after[column])));
        }
        const auto openMenu = [list, popup] {
            const QPoint point = list->visualItemRect(list->topLevelItem(0)).center();
            const QPoint windowPoint = popup->mapFromGlobal(list->viewport()->mapToGlobal(point));
            QTest::mouseClick(popup->windowHandle(), Qt::RightButton, Qt::NoModifier, windowPoint);
        };
        openMenu();
        QTRY_VERIFY(popup->findChild<QMenu *>("pluginContextMenu"));
        auto *menu = popup->findChild<QMenu *>("pluginContextMenu");
        QVERIFY(menu && menu->isVisible());
        QCOMPARE(menu->actions().size(), 2);
        QCOMPARE(menu->actions().first()->text(), QStringLiteral("卸载"));
        QVERIFY(!menu->actions().first()->isEnabled());
        QCOMPARE(menu->actions().last()->text(), QStringLiteral("关于"));
        QTest::qWait(180);
        QVERIFY(menu->grab().save("screenshots/plugin-context-menu.png"));
        QSignalSpy aboutTriggered(menu->actions().last(), &QAction::triggered);
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(menu->actions().last()).center());
        QCOMPARE(aboutTriggered.size(), 1);
        QTRY_VERIFY(popup->findChild<QDialog *>("pluginAboutPopup"));
        auto *about = popup->findChild<QDialog *>("pluginAboutPopup");
        QVERIFY(about->isVisible());
        QVERIFY(popup->isVisible());
        QCOMPARE(about->findChild<QLabel *>("pluginAboutName")->text(), QStringLiteral("测试插件"));
        QCOMPARE(about->findChild<QLabel *>("pluginAboutId")->text(), QString("example.alpha"));
        QCOMPARE(about->findChild<QLabel *>("pluginAboutVersion")->text(), QString("1.2.3"));
        QCOMPARE(about->findChild<QLabel *>("pluginAboutApiVersion")->text(), QString::fromLatin1(vusiif::ztgui::api::ApiVersion));
        QCOMPARE(about->findChild<QLabel *>("pluginAboutLoadState")->text(), QStringLiteral("已加载"));
        QTest::qWait(220);
        QVERIFY(about->grab().save("screenshots/plugin-about.png"));
        QTest::keyClick(about, Qt::Key_Escape);
        QVERIFY(!about->isVisible());
        QVERIFY(popup->isVisible());
        QCOMPARE(QApplication::activePopupWidget(), popup);
        QTRY_VERIFY(!popup->findChild<QDialog *>("pluginAboutPopup"));
        QTRY_VERIFY(!popup->findChild<QMenu *>("pluginContextMenu"));
        popup->close();
    }

    void moonFilesDropContextMenuAndDeletion() {
        ScriptWindow window;
        window.show();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        navigation->setCurrentRow(0);
        auto *network = window.findChild<NetworkTab *>();
        network->showMoons();
        auto *moon = network->moonTab();
        auto *add = moon->findChild<QPushButton *>("addMoonButton");
        QVERIFY(add->isVisible());
        QCOMPARE(moon->tree()->columnCount(), 1);
        QTemporaryDir sources;
        QVERIFY(sources.isValid());
        const QString source = sources.filePath("0000000123456789.moon");
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("fixture moon file");
        file.close();
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(source)});
        auto *moonDialog = network->findChild<QDialog *>("moonDialog");
        moonDialog->close();
        QTest::qWait(220);
        QDragEnterEvent wrongPage(QPoint(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &wrongPage);
        QVERIFY(!wrongPage.isAccepted());
        network->showMoons();
        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(moon, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(moon, &drop);
        QVERIFY(drop.isAccepted());
        QVERIFY(window.succeeded);
        QCOMPARE(window.operations, 1);
        QCOMPARE(moon->tree()->topLevelItemCount(), 1);
        const QString destination = QDir(vusiif::ztgui::zerotier::homeDirectory()).filePath("moons.d/0000000123456789.moon");
        QVERIFY(QFileInfo::exists(destination));
        QVERIFY(moon->tree()->isVisible());
        QVERIFY(add->parentWidget() == moon);
        QVERIFY(!moon->findChild<QPushButton *>("renameButton"));
        QVERIFY(!moon->findChild<QPushButton *>("detailsButton"));
        moon->tree()->setCurrentItem(moon->tree()->topLevelItem(0));
        const QPoint position = moon->tree()->visualItemRect(moon->tree()->topLevelItem(0)).center();
        moon->tree()->itemDoubleClicked(moon->tree()->topLevelItem(0), 0);
        auto *details = moon->findChild<QDialog *>("detailsPopup");
        QVERIFY(details);
        details->close();
        QTimer::singleShot(80, &window, [&] {
            auto *menu = moon->findChild<QMenu *>("moonMenu");
            QVERIFY(menu);
            QCOMPARE(menu->actions()[0]->text(), QStringLiteral("备注"));
            QCOMPARE(menu->actions()[1]->text(), QStringLiteral("详细"));
            QTimer::singleShot(250, &window, [&] {
                auto *dialog = window.findChild<QDialog *>("moonDeleteConfirmation");
                QVERIFY(dialog);
                QVERIFY(dialog->isModal());
                QVERIFY(dialog->grab().save("screenshots/moon-delete.png"));
                dialog->reject();
            });
            menu->findChild<QPushButton *>("deleteMoonAction")->click();
        });
        moon->tree()->customContextMenuRequested(position);
        QVERIFY(QFileInfo::exists(destination));
        QCOMPARE(window.operations, 1);
        QTimer::singleShot(80, &window, [&] {
            auto *menu = moon->findChild<QMenu *>("moonMenu");
            QVERIFY(menu);
            QTimer::singleShot(80, &window, [&] {
                auto *dialog = window.findChild<QDialog *>("moonDeleteConfirmation");
                QVERIFY(dialog);
                dialog->findChild<QPushButton *>("confirmDelete")->click();
            });
            menu->findChild<QPushButton *>("deleteMoonAction")->click();
        });
        moon->tree()->customContextMenuRequested(position);
        QVERIFY(window.succeeded);
        QCOMPARE(window.operations, 2);
        QVERIFY(!QFileInfo::exists(destination));
        QCOMPARE(moon->tree()->topLevelItemCount(), 0);
        QVERIFY(moon->tree()->isVisible());
        QVERIFY(add->isVisible());
        QVERIFY(window.grab().save("screenshots/moons-empty.png"));
    }

    void reconcilePreservesInteraction() {
        DataTable table("test", {"名称", "ID", "状态", "IP"});
        table.resize(620, 360);
        table.show();
        auto snapshot = rows();
        table.setRows(snapshot);
        auto *tree = table.tree();
        QTest::qWait(50);
        auto *selected = tree->topLevelItem(20);
        tree->setCurrentItem(selected);
        selected->setSelected(true);
        const QString selectedId = table.selectedId();
        tree->header()->moveSection(3, 2);
        tree->setColumnWidth(0, 235);
        tree->horizontalScrollBar()->setValue(80);
        tree->verticalScrollBar()->setValue(320);
        const int horizontal = tree->horizontalScrollBar()->value();
        const int vertical = tree->verticalScrollBar()->value();
        snapshot[20].cells[1] = "等待授权";
        snapshot.removeLast();
        snapshot.prepend({"ffffffffffffffff", {"ffffffffffffffff", "已连接", "10.0.0.99"}, {{"id", "ffffffffffffffff"}}});
        table.setRows(snapshot);
        QTest::qWait(50);
        QCOMPARE(table.selectedId(), selectedId);
        QCOMPARE(tree->currentItem(), selected);
        QCOMPARE(tree->columnWidth(0), 235);
        QCOMPARE(tree->header()->visualIndex(3), 2);
        QCOMPARE(tree->horizontalScrollBar()->value(), horizontal);
        QCOMPARE(tree->verticalScrollBar()->value(), vertical);
        QCOMPARE(selected->text(2), QStringLiteral("等待授权"));
        const int count = tree->topLevelItemCount();
        table.setAvailability(false, "服务未响应");
        QCOMPARE(tree->topLevelItemCount(), count);
        QCOMPARE(table.selectedId(), selectedId);
    }

    void selectingRowDoesNotMoveText() {
        DataTable table("alignment", {"名称", "ID", "状态", "IP"});
        table.resize(850, 220);
        table.setRows({{"node", {"node", "已连接", "192.0.2.1"}, {{"id", "node"}}}});
        table.show();
        QTest::qWait(50);
        auto *tree = table.tree();
        tree->clearSelection();
        const QRect row = tree->visualItemRect(tree->topLevelItem(0));
        const auto before = tree->viewport()->grab().toImage();
        const QColor text = table.palette().color(QPalette::Text);
        const auto bounds = [&](const QImage &image) {
            int left = image.width(), right = -1;
            for (int y = row.top(); y <= row.bottom(); ++y)
                for (int x = 0; x < tree->columnWidth(0); ++x)
                    if (image.pixelColor(x, y) == text) { left = qMin(left, x); right = qMax(right, x); }
            return qMakePair(left, right);
        };
        const auto original = bounds(before);
        QVERIFY(original.first < original.second);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(30, row.center().y()));
        QTest::qWait(30);
        QCOMPARE(bounds(tree->viewport()->grab().toImage()), original);
    }

    void renameSurvivesPollingAndRecreation() {
        const auto snapshot = rows(2);
        {
            DataTable table("names", {"名称", "ID", "状态", "IP"});
            table.setRows(snapshot);
            QCOMPARE(table.tree()->topLevelItem(0)->text(0), snapshot[0].id);
            table.tree()->setCurrentItem(table.tree()->topLevelItem(0));
            QTimer::singleShot(80, &table, [] {
                auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                dialog->setTextValue(QStringLiteral("办公室"));
                dialog->accept();
            });
            table.renameSelected();
            table.setRows(snapshot);
            QCOMPARE(table.tree()->topLevelItem(0)->text(0), QStringLiteral("办公室"));
        }
        DataTable reopened("names", {"名称", "ID", "状态", "IP"});
        reopened.setRows(snapshot);
        QCOMPARE(reopened.tree()->topLevelItem(0)->text(0), QStringLiteral("办公室"));
    }

    void detailsKeepAllFieldsAndStayNonModal() {
        DataTable table("details", {"名称", "ID", "状态", "IP"});
        table.setRows(rows(1));
        table.tree()->setCurrentItem(table.tree()->topLevelItem(0));
        table.showDetails();
        auto *dialog = table.findChild<QDialog *>("detailsPopup");
        QVERIFY(dialog);
        QVERIFY(!dialog->isModal());
        auto *json = dialog->findChild<QPlainTextEdit *>("detailsJson");
        QVERIFY(json);
        const auto object = QJsonDocument::fromJson(json->toPlainText().toUtf8()).object();
        QCOMPARE(object["unknown"].toInt(), 42);
        QCOMPARE(object["routes"].toArray()[0].toObject()["target"].toString(), QString("10.0.0.0/24"));
        QCOMPARE(vusiif::ztgui::zerotier::translate("PLANET"), QStringLiteral("根节点"));
        QCOMPARE(vusiif::ztgui::zerotier::translate("MOON"), QStringLiteral("中转站"));
        QCOMPARE(vusiif::ztgui::zerotier::translate("LEAF"), QStringLiteral("成员"));
        QCOMPARE(vusiif::ztgui::zerotier::translate("DIRECT"), QStringLiteral("直链"));
        QCOMPARE(vusiif::ztgui::zerotier::translate("RELAY"), QStringLiteral("中转"));
    }

    void mouseDragScrollsBothAxes() {
        DataTable table("drag", {"名称", "ID", "状态", "IP"});
        table.resize(510, 320);
        table.setRows(rows());
        table.show();
        QTest::qWait(50);
        auto *viewport = table.tree()->viewport();
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(300, 180));
        QTest::mouseMove(viewport, QPoint(180, 70), 50);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(180, 70));
        QVERIFY(table.tree()->horizontalScrollBar()->value() > 0);
        QVERIFY(table.tree()->verticalScrollBar()->value() > 0);
    }

    void asynchronousPollAndRecovery() {
        QWidget owner;
        owner.show();
        JsonPoller poller(&owner, "listnetworks");
        QSignalSpy updates(&poller, &JsonPoller::updated);
        QSignalSpy availability(&poller, &JsonPoller::availabilityChanged);
        QTRY_VERIFY_WITH_TIMEOUT(updates.size() >= 1, 2000);
        qputenv("ZT_TEST_MODE", "slow");
        poller.refresh();
        bool responsive = false;
        QTimer::singleShot(100, &owner, [&] { responsive = true; });
        QTRY_VERIFY_WITH_TIMEOUT(responsive, 500);
        const int before = updates.size();
        poller.refresh(); // Must not start a second process.
        QTest::qWait(200);
        QCOMPARE(updates.size(), before);
        QTRY_VERIFY_WITH_TIMEOUT(!availability.last()[0].toBool(), 6000);
        qunsetenv("ZT_TEST_MODE");
        QTRY_VERIFY_WITH_TIMEOUT(updates.size() > before, 4000);
        QVERIFY(availability.last()[0].toBool());
        qputenv("ZT_TEST_MODE", "malformed");
        poller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!availability.last()[0].toBool(), 2000);
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

    void serviceActionFollowsActualStatus() {
        DeferredScriptWindow window;
        window.show();
        auto *action = window.findChild<QPushButton *>("serviceAction");
        auto *dropdown = window.findChild<QToolButton *>("serviceDropdown");
        auto *menu = window.findChild<QMenu *>("serviceMenu");
        window.status = {false, false, true};
        QTRY_COMPARE_WITH_TIMEOUT(action->text(), QStringLiteral("安装"), 2500);
        QVERIFY(!dropdown->isVisible());
        QVERIFY(menu->actions().isEmpty());
        window.status = {true, false, true};
        QTRY_COMPARE_WITH_TIMEOUT(action->text(), QStringLiteral("启动"), 2500);
        QVERIFY(dropdown->isVisible());
        QCOMPARE(menu->actions().size(), 1);
        QCOMPARE(menu->actions()[0]->text(), QStringLiteral("卸载"));
        window.status = {true, true, true};
        QTRY_COMPARE_WITH_TIMEOUT(action->text(), QStringLiteral("停止"), 2500);
        QCOMPARE(menu->actions().size(), 2);
        QCOMPARE(menu->actions()[0]->text(), QStringLiteral("重启"));
        QCOMPARE(menu->actions()[1]->text(), QStringLiteral("卸载"));
        action->click();
        QCOMPARE(window.operations, 1);
        window.status = {true, false, true};
        window.complete(true);
        QCOMPARE(action->text(), QStringLiteral("启动"));
    }

    void networkMenusExpansionAndRemarks() {
        MainWindow window;
        window.show();
        window.findChild<QListWidget *>("navigation")->setCurrentRow(0);
        auto *network = window.findChild<NetworkTab *>();
        QTRY_COMPARE_WITH_TIMEOUT(network->rowCount(), 40, 2500);
        auto *item = network->tree()->topLevelItem(0);
        network->tree()->setCurrentItem(item);
        const QString id = network->selectedId();
        const QString directory = QDir(vusiif::ztgui::zerotier::homeDirectory()).filePath("moons.d");
        QVERIFY(QDir().mkpath(directory));
        const QString moonPath = QDir(directory).filePath("000000" + id.left(10) + ".moon");
        QFile moonFile(moonPath);
        QVERIFY(moonFile.open(QIODevice::WriteOnly));
        moonFile.write("signed moon fixture"); moonFile.close();
        QCOMPARE(vusiif::ztgui::zerotier::matchingMoonFile(id.toUpper()), QFileInfo(moonPath).absoluteFilePath());
        QVERIFY(vusiif::ztgui::zerotier::matchingMoonFile("0123456789000000").isEmpty());
        auto context = [&](int actionIndex, const QStringList &expected) {
            QTimer::singleShot(80, &window, [&] {
                auto *menu = network->findChild<QMenu *>("networkMenu");
                QVERIFY(menu);
                QStringList labels;
                for (auto *action : menu->actions()) labels.append(action->text());
                QCOMPARE(labels, expected);
                menu->actions()[actionIndex]->trigger();
                menu->close();
            });
            network->tree()->customContextMenuRequested(network->tree()->visualItemRect(item).center());
        };
        const QStringList withMoon = {QStringLiteral("修改备注"), QStringLiteral("复制ID"), QStringLiteral("本机ip"), QStringLiteral("导出中转站"), QStringLiteral("退出网络")};
        context(1, withMoon);
        QCOMPARE(QApplication::clipboard()->text(), id);
        context(2, withMoon);
        QCOMPARE(QApplication::clipboard()->text(), QString("10.24.0.1"));
        const auto exported = m_settings.filePath("exported.ztbundle");
        QTimer::singleShot(150, &window, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->selectFile(exported);
            QMetaObject::invokeMethod(dialog, "accept");
        });
        context(3, withMoon);
        QFile exportFile(exported);
        QVERIFY(exportFile.open(QIODevice::ReadOnly));
        vusiif::ztgui::zerotier::bundle::Configuration configuration;
        QString error;
        QVERIFY(vusiif::ztgui::zerotier::bundle::decode(exportFile.readAll(), configuration, error));
        QCOMPARE(configuration.networkId, id);
        QCOMPARE(configuration.moonFileName, QFileInfo(moonPath).fileName());
        QCOMPARE(configuration.moonContents, QByteArray("signed moon fixture"));
        network->tree()->itemDoubleClicked(item, 0);
        QVERIFY(item->isExpanded());
        QCOMPARE(item->childCount(), 1);
        QCOMPARE(item->child(0)->text(0), QString());
        network->editRemark();
        auto *editor = network->findChild<QLineEdit *>("remarkEditor");
        QVERIFY(editor);
        QCOMPARE(editor->text(), QStringLiteral("团队网络"));
        editor->setText(QStringLiteral("办公室"));
        QTest::qWait(2200);
        QCOMPARE(editor->text(), QStringLiteral("办公室"));
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(item->text(0), QStringLiteral("办公室"));
        QTest::qWait(2200);
        QCOMPARE(item->text(0), QStringLiteral("办公室"));
        QVERIFY(item->isExpanded());
        network->editRemark();
        editor = network->findChild<QLineEdit *>("remarkEditor");
        QVERIFY(editor);
        editor->setText(QStringLiteral("新的备注"));
        QTest::mouseClick(network->findChild<QPushButton *>("joinNetworkButton"), Qt::LeftButton);
        QCOMPARE(item->text(0), QStringLiteral("新的备注"));
        auto *join = network->findChild<QDialog *>("joinNetworkDialog");
        QVERIFY(join);
        QVERIFY(join->findChild<QPushButton *>("importMoonButton"));
        QTest::keyClick(join, Qt::Key_Escape);
        QTRY_VERIFY(!network->findChild<QDialog *>("joinNetworkDialog"));
        QVERIFY(QFile::remove(moonPath));
        const QStringList withoutMoon = {QStringLiteral("修改备注"), QStringLiteral("复制ID"), QStringLiteral("本机ip"), QStringLiteral("退出网络")};
        context(1, withoutMoon);
    }

    void scrollingPageAndJoinRowSurviveRefresh() {
        MainWindow window;
        window.resize(800, 500);
        window.show();
        auto *network = window.findChild<NetworkTab *>();
        auto *peers = window.findChild<PeersTab *>();
        auto *scroll = window.findChild<QScrollArea *>("zeroTierScroll");
        QVERIFY(scroll);
        QTRY_COMPARE_WITH_TIMEOUT(network->rowCount(), 40, 2500);
        QTRY_COMPARE_WITH_TIMEOUT(peers->rowCount(), 35, 2500);
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QCOMPARE(network->tree()->verticalScrollBar()->maximum(), 0);
        QCOMPARE(peers->tree()->verticalScrollBar()->maximum(), 0);
        const QPoint point(40, 60);
        QWheelEvent wheel(point, network->tree()->viewport()->mapToGlobal(point), QPoint(), QPoint(0, -120),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(network->tree()->viewport(), &wheel);
        QVERIFY(scroll->verticalScrollBar()->value() > 0);
        auto *footer = network->inlineActionItem();
        auto *button = network->findChild<QPushButton *>("joinNetworkButton");
        QVERIFY(footer && button);
        QVERIFY(network->tree()->viewport()->isAncestorOf(button));
        QCOMPARE(network->tree()->topLevelItem(network->rowCount()), footer);
        network->tree()->sortByColumn(1, Qt::DescendingOrder);
        QCOMPARE(network->tree()->topLevelItem(network->rowCount()), footer);
        network->tree()->sortByColumn(0, Qt::AscendingOrder);
        QCOMPARE(network->tree()->topLevelItem(network->rowCount()), footer);
        const int previousHeight = network->height();
        auto *first = network->tree()->topLevelItem(0);
        network->tree()->itemDoubleClicked(first, 0);
        QTRY_VERIFY(network->height() > previousHeight);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        const int position = scroll->verticalScrollBar()->value();
        QTest::qWait(2200);
        QCOMPARE(network->inlineActionItem(), footer);
        QCOMPARE(network->findChild<QPushButton *>("joinNetworkButton"), button);
        QCOMPARE(network->tree()->topLevelItem(network->rowCount()), footer);
        QCOMPARE(scroll->verticalScrollBar()->value(), position);
        QVERIFY(window.grab().save("screenshots/zerotier-scrolled.png"));
        network->setRows({});
        QCOMPARE(network->rowCount(), 0);
        QCOMPARE(network->tree()->topLevelItem(0), footer);
        QVERIFY(button->isVisible());
        scroll->ensureWidgetVisible(button);
        QTest::mouseClick(button, Qt::LeftButton);
        auto *join = network->findChild<QDialog *>("joinNetworkDialog");
        QVERIFY(join && join->isVisible());
        auto *input = join->findChild<QLineEdit *>("networkIdInput");
        input->setFocus();
        QTest::keyClicks(input, "abcdef1234000001");
        QCOMPARE(input->text(), QString("abcdef1234000001"));
        QTest::keyClick(input, Qt::Key_Escape);
        QTRY_VERIFY(!network->findChild<QDialog *>("joinNetworkDialog"));
    }

    void systemFocusFramesAreNotPainted() {
        for (const QString &theme : {QString("light"), QString("dark")}) {
            QSettings().setValue("appearance/theme", theme);
            vusiif::ztgui::ui::applyAppStyle(*qApp);
            QImage image(120, 40, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QStyleOptionFocusRect option;
            option.rect = QRect(2, 2, 116, 36);
            option.state = QStyle::State_Enabled | QStyle::State_HasFocus | QStyle::State_KeyboardFocusChange;
            QPainter painter(&image);
            qApp->style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter);
            qApp->style()->drawPrimitive(QStyle::PE_FrameDefaultButton, &option, &painter);
            painter.end();
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) QCOMPARE(image.pixelColor(x, y).alpha(), 0);
        }
    }

    void windowLayoutAndVisuals() {
        MainWindow window;
        window.show();
        auto *navigation = window.findChild<QListWidget *>("navigation");
        QCOMPARE(navigation->count(), 1);
        QCOMPARE(navigation->item(0)->text(), QStringLiteral("ZeroTier"));
        QCOMPARE(window.windowTitle(), QStringLiteral("ZTGUI"));
        QCOMPARE(window.findChild<QLabel *>("brand")->text(), QStringLiteral("ZTGUI"));
        QCOMPARE(window.findChild<QLabel *>("sidebarVersion")->text(), QStringLiteral("v" ZEROTIER_GUI_VERSION));
        QCOMPARE(window.findChild<QLabel *>("serviceTitle")->text(), QStringLiteral("ZeroTier"));
        QVERIFY(!window.findChild<QSplitter *>("connectionsSplitter"));
        QVERIFY(!window.findChild<QWidget *>("themeSelector"));
        QVERIFY(!window.findChild<QWidget *>("animationsToggle"));
        QVERIFY(!window.findChild<QWidget *>("infoTable"));
        auto *indicator = window.findChild<QLabel *>("onlineIndicator");
        QTRY_VERIFY_WITH_TIMEOUT(indicator->property("online").toBool(), 2500);
        auto *node = window.findChild<QLabel *>("nodeId");
        node->linkActivated("0123456789");
        QCOMPARE(QApplication::clipboard()->text(), QString("0123456789"));
        QCOMPARE(window.findChild<QLabel *>("nodeVersion")->text(), QString("v1.16.0"));
        QTest::qWait(250);
        QVERIFY(window.grab().save("screenshots/service.png"));
        auto *gear = window.findChild<QPushButton *>("settingsButton");
        gear->click();
        QTest::qWait(220);
        auto *settings = window.findChild<QDialog *>("settingsPopup");
        QVERIFY(settings->isVisible());
        QVERIFY(settings->pos().x() >= gear->mapToGlobal(QPoint(0, 0)).x());
        QVERIFY(settings->pos().y() < gear->mapToGlobal(QPoint(0, 0)).y());
        QVERIFY(window.findChild<QPushButton *>("pluginsButton")->isEnabled());
        QVERIFY(window.findChild<QCheckBox *>("followSystemTheme")->isChecked());
        QVERIFY(settings->grab().save("screenshots/settings.png"));
        auto *theme = window.findChild<QAbstractButton *>("themeSwitch");
        const bool initiallyDark = qApp->property("darkTheme").toBool();
        theme->click();
        QCOMPARE(qApp->property("darkTheme").toBool(), !initiallyDark);
        QCOMPARE(QSettings().value("appearance/theme").toString(), initiallyDark ? QString("light") : QString("dark"));
        QVERIFY(!window.findChild<QCheckBox *>("followSystemTheme")->isChecked());
        if (initiallyDark) theme->click();
        QTest::qWait(250);
        QVERIFY(settings->grab().save("screenshots/settings-dark.png"));
        window.findChild<QPushButton *>("aboutButton")->click();
        QTRY_VERIFY(!settings->isVisible());
        auto *about = window.findChild<QDialog *>("aboutDialog");
        QVERIFY(about && about->isVisible());
        QCOMPARE(about->findChild<QLabel *>("applicationVersion")->text(), QStringLiteral("v" ZEROTIER_GUI_VERSION));
        QCOMPARE(about->findChild<QLabel *>("pluginApiVersion")->text(),
                 QStringLiteral("API %1").arg(QString::fromLatin1(vusiif::ztgui::api::ApiVersion)));
        QTest::qWait(220);
        QVERIFY(about->grab().save("screenshots/about.png"));
        about->close();
        navigation->setCurrentRow(0);
        auto *network = window.findChild<NetworkTab *>();
        auto *peers = window.findChild<PeersTab *>();
        QTRY_COMPARE_WITH_TIMEOUT(network->rowCount(), 40, 2500);
        QTRY_COMPARE_WITH_TIMEOUT(peers->tree()->topLevelItemCount(), 35, 2500);
        QCOMPARE(peers->tree()->columnCount(), 5);
        QCOMPARE(peers->tree()->topLevelItem(34)->text(1), QStringLiteral("根节点"));
        QCOMPARE(peers->tree()->topLevelItem(0)->text(2), QStringLiteral("直链"));
        QVERIFY(network->mapTo(&window, QPoint()).y() < peers->mapTo(&window, QPoint()).y());
        qputenv("ZT_TEST_MODE", "compact-lists");
        network->findChild<JsonPoller *>()->refresh();
        peers->findChild<JsonPoller *>()->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(network->rowCount(), 2, 2500);
        QTRY_COMPARE_WITH_TIMEOUT(peers->rowCount(), 4, 2500);
        QTest::qWait(250);
        QVERIFY(window.grab().save("screenshots/connections-dark.png"));
        for (auto *table : {network->tree(), peers->tree()})
            for (int column = 0; column < table->columnCount(); ++column)
                QCOMPARE(table->header()->sectionResizeMode(column), QHeaderView::Interactive);
        gear->click();
        window.findChild<QCheckBox *>("followSystemTheme")->setChecked(true);
        QCOMPARE(QSettings().value("appearance/theme").toString(), QString("system"));
        settings->close();
        QTest::qWait(250);
        QVERIFY(window.grab().save("screenshots/connections.png"));
        network->showJoinDialog();
        auto *join = window.findChild<QDialog *>("joinNetworkDialog");
        QTest::qWait(220);
        QVERIFY(join->grab().save("screenshots/join.png"));
        join->close();
        QTest::qWait(220);
        window.appendOutput(QStringLiteral("文件日志测试"));
        QFile log(vusiif::ztgui::util::log::path());
        QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(log.readAll().contains(QStringLiteral("文件日志测试").toUtf8()));
    }

};

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR", (qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts").toLocal8Bit());
#endif
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QDir().mkpath("screenshots");
    GuiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "GuiTests.moc"
