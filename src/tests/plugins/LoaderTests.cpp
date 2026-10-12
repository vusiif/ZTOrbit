#include "../../client/main/plugins/PluginLoader.h"
#include "../../client/main/plugins/PluginRegistry.h"
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
using namespace vusiif::ztgui::api;
using namespace vusiif::ztgui::util;
using namespace vusiif::ztgui::plugins;
class LoaderTests : public QObject {
    Q_OBJECT
private slots:
    void previousApiLayoutStillUsesHostServices() {
        QTemporaryDir directory;
        const auto library = QString::fromUtf8(FIXTURE_LEGACY);
        QVERIFY(QFile::copy(library,directory.filePath(QFileInfo(library).fileName())));
        PluginRegistry registry; PluginLoader loader;
        loader.scan(directory.path(),registry);
        QTRY_COMPARE(registry.preparationState("test.lifecycle"),PreparationState::Ready);
        QCOMPARE(registry.plugins().size(),1);
        loader.requestShutdown();
        QTRY_VERIFY(loader.shutdownComplete());
    }
    void cleanup() {
        qunsetenv("ZTGUI_PLUGIN_LIFETIME_FILE");
        qunsetenv("ZTGUI_PLUGIN_CREATE_STEPS");
        qunsetenv("ZTGUI_PLUGIN_PREPARATION_STARTED_FILE");
    }
    void cooperativeShutdownSkipsQueuedTasksAndKeepsLibrariesAlive() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto tracePath = directory.filePath("shutdown.txt");
        qputenv("ZTGUI_PLUGIN_LIFETIME_FILE", tracePath.toUtf8());
        qputenv("ZTGUI_PLUGIN_CREATE_STEPS", "1000");
        const auto copy = [&](const QString &library, const QString &name) {
            return QFile::copy(library, directory.filePath(name + '.' + QFileInfo(library).suffix()));
        };
        QVERIFY(copy(QString::fromUtf8(FIXTURE_CANCELLABLE), "a"));
        QVERIFY(copy(QString::fromUtf8(FIXTURE_CANCELLABLE_SECOND), "b"));
        QVERIFY(copy(QString::fromUtf8(FIXTURE_CANCELLABLE_QUEUED), "c"));
        const auto trace = [&] { QFile file(tracePath); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
        auto registry = std::make_unique<PluginRegistry>();
        auto loader = std::make_unique<PluginLoader>();
        loader->scan(directory.path(), *registry);
        QTRY_VERIFY(trace().contains("started:test.cancellable\n"));
        QTRY_VERIFY(trace().contains("started:test.cancellable_second\n"));
        QVERIFY(!trace().contains("started:test.cancellable_queued"));
        QCOMPARE(trace().count("create\n"), 3); // One lifecycle creation per plugin.
        QSignalSpy finished(loader.get(), &PluginLoader::shutdownFinished);
        QSignalSpy progress(loader.get(), &PluginLoader::shutdownProgress);
        loader->requestShutdown(); loader->requestShutdown();
        QCOMPARE(finished.size(), 0);
        QVERIFY(progress.size() > 0);
        QVERIFY(loader->scan(directory.path(), *registry).isEmpty());
        QCOMPARE(loader->unload("test.cancellable", *registry).error, Error::Busy);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1500);
        QVERIFY(loader->shutdownComplete());
        QVERIFY(loader->pendingInitializations().isEmpty());
        QVERIFY(trace().contains("cancelled:test.cancellable\n"));
        QVERIFY(trace().contains("cancelled:test.cancellable_second\n"));
        QVERIFY(!trace().contains("started:test.cancellable_queued"));
        QVERIFY(!trace().contains("destroy"));
        QVERIFY(!trace().contains("entry"));
        QCOMPARE(registry->preparationState("test.cancellable"), PreparationState::Preparing);
        loader.reset();
        QVERIFY(registry->plugins().isEmpty());
        QCOMPARE(trace().count("destroy\n"), 3);
        QCOMPARE(trace().count("entry\n"), 3);
        QVERIFY(trace().indexOf("cancelled:test.cancellable_second") < trace().indexOf("destroy"));
        registry.reset();
    }
    void shutdownWaitsAsynchronouslyForNonCooperatingTask() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto started = directory.filePath("started.txt");
        qputenv("ZTGUI_PLUGIN_PREPARATION_STARTED_FILE", started.toUtf8());
        const auto library = QString::fromUtf8(FIXTURE_LIFECYCLE);
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        PluginRegistry registry; PluginLoader loader;
        loader.scan(directory.path(), registry);
        QTRY_VERIFY(QFileInfo::exists(started));
        QSignalSpy finished(&loader, &PluginLoader::shutdownFinished);
        int ticks = 0; QTimer heartbeat; heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; }); heartbeat.start();
        QElapsedTimer elapsed; elapsed.start();
        loader.requestShutdown();
        QVERIFY(elapsed.elapsed() < 50);
        QCOMPARE(finished.size(), 0);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(ticks > 2);
        QCOMPARE(registry.preparationState("test.lifecycle"), PreparationState::Preparing);
    }
    void emptyShutdownIsAsynchronousAndIdempotent() {
        PluginLoader loader;
        QSignalSpy finished(&loader, &PluginLoader::shutdownFinished);
        loader.requestShutdown(); loader.requestShutdown();
        QCOMPARE(finished.size(), 0);
        QTRY_COMPARE(finished.size(), 1);
        loader.requestShutdown();
        QTest::qWait(40);
        QCOMPARE(finished.size(), 1);
    }
    void lifecycleTeardown_data() {
        QTest::addColumn<bool>("unloadFirst");
        QTest::newRow("exit") << false;
        QTest::newRow("unload") << true;
    }
    void lifecycleTeardown() {
        QFETCH(bool, unloadFirst);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto file = directory.filePath("lifecycle.txt");
        qputenv("ZTGUI_PLUGIN_LIFETIME_FILE", file.toUtf8());
        const QString library = QString::fromUtf8(FIXTURE_LIFECYCLE);
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        auto loader = std::make_unique<PluginLoader>();
        auto registry = std::make_unique<PluginRegistry>();
        QWidget parent;
        loader->scan(directory.path(), *registry);
        QCOMPARE(registry->preparationState("test.lifecycle"), PreparationState::Preparing);
        QTRY_COMPARE(registry->preparationState("test.lifecycle"), PreparationState::Ready);
        QVERIFY(registry->page("test.lifecycle/overview", &parent).result.ok());
        QVERIFY(registry->activateMenu("test.lifecycle/overview").ok());
        if (unloadFirst) {
            QVERIFY(loader->unload("test.lifecycle", *registry).ok());
            QVERIFY(registry->plugins().isEmpty());
            QCOMPARE(loader->unload("test.lifecycle", *registry).error, Error::NotFound);
        }
        registry.reset(); loader.reset();
        QFile trace(file); QVERIFY(trace.open(QIODevice::ReadOnly));
        QCOMPARE(trace.readAll(), QByteArray("create\nprepared\nentered:builtin/zerotier\n"
            "exited:builtin/zerotier\npage\nfactory\ndestroy\nentry\n"));
        qunsetenv("ZTGUI_PLUGIN_LIFETIME_FILE");
    }
    void destroyWaitsForCreateEvenOnFailure_data() {
        QTest::addColumn<QString>("library");
        QTest::newRow("running") << QString::fromUtf8(FIXTURE_LIFECYCLE);
        QTest::newRow("failed") << QString::fromUtf8(FIXTURE_LIFECYCLE_FAILED);
    }
    void destroyWaitsForCreateEvenOnFailure() {
        QFETCH(QString, library);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto file = directory.filePath("lifecycle.txt");
        qputenv("ZTGUI_PLUGIN_LIFETIME_FILE", file.toUtf8());
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        const auto started = directory.filePath("started.txt");
        qputenv("ZTGUI_PLUGIN_PREPARATION_STARTED_FILE", started.toUtf8());
        auto loader = std::make_unique<PluginLoader>();
        auto registry = std::make_unique<PluginRegistry>();
        loader->scan(directory.path(), *registry);
        QTRY_VERIFY(QFileInfo::exists(started));
        QVERIFY(loader->unload("test.lifecycle", *registry).ok());
        registry.reset(); loader.reset();
        QCoreApplication::processEvents();
        QFile trace(file); QVERIFY(trace.open(QIODevice::ReadOnly));
        QCOMPARE(trace.readAll(), QByteArray("create\nprepared\nfactory\ndestroy\nentry\n"));
        qunsetenv("ZTGUI_PLUGIN_LIFETIME_FILE");
    }
    void preparationIsAsyncAndGatesPages_data() {
        QTest::addColumn<QString>("library"); QTest::addColumn<bool>("success");
        QTest::addColumn<QString>("pluginId");
        QTest::newRow("lifecycle-ready") << QString::fromUtf8(FIXTURE_LIFECYCLE) << true << QString("test.lifecycle");
        QTest::newRow("lifecycle-failed") << QString::fromUtf8(FIXTURE_LIFECYCLE_FAILED) << false << QString("test.lifecycle");
    }
    void preparationIsAsyncAndGatesPages() {
        QFETCH(QString, library); QFETCH(bool, success);
        QFETCH(QString, pluginId);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        auto loader = std::make_unique<PluginLoader>(); auto registry = std::make_unique<PluginRegistry>(); QWidget parent;
        loader->scan(directory.path(), *registry);
        QCOMPARE(registry->preparationState(pluginId), PreparationState::Preparing);
        QCOMPARE(registry->page(pluginId + "/overview", &parent).result.error, Error::Busy);
        int ticks = 0; QTimer heartbeat; heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&ticks] { ++ticks; }); heartbeat.start();
        QTRY_VERIFY(registry->preparationState(pluginId) != PreparationState::Preparing);
        QVERIFY(ticks > 2);
        if (success) {
            QCOMPARE(registry->preparationState(pluginId), PreparationState::Ready);
            QVERIFY(registry->page(pluginId + "/overview", &parent).result.ok());
        } else {
            QCOMPARE(registry->preparationState(pluginId), PreparationState::Failed);
            QVERIFY(registry->preparationMessage(pluginId).contains("failed"));
            QCOMPARE(registry->page(pluginId + "/overview", &parent).result.error, Error::PageCreationFailed);
        }
        registry.reset(); loader.reset();
    }
    void exitingDuringPreparationWaitsSafely() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString library = QString::fromUtf8(FIXTURE_LIFECYCLE);
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        auto loader = std::make_unique<PluginLoader>(); auto registry = std::make_unique<PluginRegistry>();
        loader->scan(directory.path(), *registry);
        registry.reset(); loader.reset(); // worker finishes before unloading its code
        QCoreApplication::processEvents(); // canceled completion cannot access the destroyed registry
    }
    void preparationCompletionSurvivesNestedPageEvents() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString library = QString::fromUtf8(FIXTURE_LIFECYCLE);
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        auto loader = std::make_unique<PluginLoader>(); auto registry = std::make_unique<PluginRegistry>();
        QWidget parent;
        MenuContribution menu;
        menu.id = "overview"; menu.title = "Other";
        menu.createPage = [](QWidget *owner) { QTest::qWait(300); return new QLabel("Other", owner); };
        QVERIFY(registry->registerPlugin({{"test.other", "Other", "1"}, {menu}}).ok());
        loader->scan(directory.path(), *registry);
        QVERIFY(registry->page("test.other/overview", &parent).result.ok());
        QTRY_COMPARE(registry->preparationState("test.lifecycle"), PreparationState::Ready);
        registry.reset(); loader.reset();
    }
    void scanAndLifetime() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto file = directory.filePath("order.txt"); qputenv("ZTGUI_PLUGIN_LIFETIME_FILE", file.toUtf8());
        auto loader = std::make_unique<PluginLoader>();
        auto registry = std::make_unique<PluginRegistry>();
        QWidget parent;
        QVERIFY(loader->scan(directory.filePath("missing"), *registry).isEmpty());
        const QString extension = QFileInfo(QString::fromUtf8(FIXTURE_GOOD)).suffix();
        QVERIFY(QFile::copy(QString::fromUtf8(FIXTURE_GOOD), directory.filePath("a." + extension)));
        QVERIFY(QFile::copy(QString::fromUtf8(FIXTURE_GOOD), directory.filePath("b." + extension)));
        QVERIFY(QFile::copy(QString::fromUtf8(FIXTURE_BAD), directory.filePath("c." + extension)));
        const auto log = loader->scan(directory.path(), *registry);
        QCOMPARE(log.size(), 3); QCOMPARE(registry->plugins().size(), 1);
        QVERIFY(log.last().contains("API version mismatch"));
        // The duplicate's root/resource are destroyed; start a fresh lifecycle trace.
        QFile trace(file); QVERIFY(trace.open(QIODevice::WriteOnly | QIODevice::Truncate)); trace.close();
        QVERIFY(parent.findChildren<QLabel *>().isEmpty());
        const auto page = registry->page("test.dynamic/overview", &parent);
        QVERIFY(page.result.ok()); QCOMPARE(page.page->parentWidget(), &parent);
        registry.reset(); loader.reset();
        QVERIFY(trace.open(QIODevice::ReadOnly)); QCOMPARE(trace.readAll(), QByteArray("page\nfactory\nentry\n"));
        qunsetenv("ZTGUI_PLUGIN_LIFETIME_FILE");
    }
    void obsoletePluginAbiIsNotInstantiated() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto tracePath = directory.filePath("obsolete.txt");
        qputenv("ZTGUI_PLUGIN_LIFETIME_FILE", tracePath.toUtf8());
        const auto library = QString::fromUtf8(FIXTURE_OBSOLETE);
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        PluginRegistry registry; PluginLoader loader;
        QVERIFY(loader.scan(directory.path(), registry).isEmpty());
        QVERIFY(registry.plugins().isEmpty());
        QVERIFY(!QFileInfo::exists(tracePath));
    }
    void unloadCancelsDeferredPreparationBeforeReload() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString library = QString::fromUtf8(FIXTURE_LIFECYCLE);
        QVERIFY(QFile::copy(library, directory.filePath(QFileInfo(library).fileName())));
        PluginRegistry registry;
        PluginLoader loader;
        QWidget parent;
        MenuContribution menu;
        menu.id = "overview"; menu.title = "Other";
        menu.createPage = [](QWidget *owner) {
            // Completion is delivered while the page factory guards reentry;
            // the loader schedules a deferred readiness update.
            QTest::qWait(300);
            return new QLabel("Other", owner);
        };
        QVERIFY(registry.registerPlugin({{"test.other", "Other", "1"}, {menu}}).ok());
        loader.scan(directory.path(), registry);
        QVERIFY(registry.page("test.other/overview", &parent).result.ok());
        QCOMPARE(registry.preparationState("test.lifecycle"), PreparationState::Preparing);
        QVERIFY(loader.unload("test.lifecycle", registry).ok());
        loader.scan(directory.path(), registry);
        QTest::qWait(40); // Old completion must not enable the new incarnation.
        QCOMPARE(registry.preparationState("test.lifecycle"), PreparationState::Preparing);
        QTRY_COMPARE(registry.preparationState("test.lifecycle"), PreparationState::Ready);
    }
};
QTEST_MAIN(LoaderTests)
#include "LoaderTests.moc"
