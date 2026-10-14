#include "../../client/main/plugins/PluginLoader.h"
#include "../../client/main/plugins/PluginRegistry.h"
#include "PluginPackage.h"
#include <ztgui/PluginInterface.h>
#include <QFile>
#include <QQuickItem>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QSettings>
#include <QTimer>
#include <QtTest>
using namespace vusiif::ztgui;
using namespace api;
using namespace plugins;
namespace vusiif::ztgui::package { QString writeLegacy(const QString &,const QString &,QJsonObject); }
class LoaderTests : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    QString copy(const QString &source,QTemporaryDir &directory) {
        const auto path=directory.filePath(QFileInfo(source).fileName());
        if(!QFile::copy(source,path)) return {}; return path;
    }
    QByteArray trace(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); }
private slots:
    void api03PackageIsRejectedBeforeEntry() {
        const auto p=package::inspect(FIXTURE_GOOD); QVERIFY(p.ok());
        std::unique_ptr<QTemporaryDir> extracted; std::unique_ptr<QLockFile> lock; QString library;
        QVERIFY(package::extract(p,extracted,library,lock).isEmpty());
        QTemporaryDir temp; const auto path=temp.filePath("api03.ztplugin"),log=temp.filePath("trace");
        auto oldInfo=p.info; oldInfo["apiVersion"]="0.3";
        QVERIFY(package::write(library,path,oldInfo).isEmpty()); qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8());
        PluginRegistry registry; PluginLoader loader; const auto result=loader.loadFile(path,registry);
        QCOMPARE(result.error,Error::InvalidPlugin); QVERIFY(result.message.contains("API version"));
        QVERIFY(registry.plugins().isEmpty()); QVERIFY(trace(log).isEmpty());
    }
    void ordinaryV1PackageLoadsOnNewHost() {
        auto p=package::inspect(FIXTURE_GOOD); QVERIFY(p.ok());
        std::unique_ptr<QTemporaryDir> extracted; std::unique_ptr<QLockFile> lock; QString library;
        QVERIFY(package::extract(p,extracted,library,lock).isEmpty());
        QTemporaryDir temp; const auto path=temp.filePath("legacy-v1.ztplugin");
        QVERIFY(package::writeLegacy(library,path,p.info).isEmpty());
        PluginRegistry registry; PluginLoader loader; QVERIFY(loader.loadFile(path,registry).ok());
        QCOMPARE(registry.plugins().size(),1); QVERIFY(loader.unload("test-dynamic",registry).ok()); QTRY_VERIFY(registry.plugins().isEmpty());
    }
    void initTestCase() {
        QCoreApplication::setOrganizationName("ZTGUI-Package-Tests"); QCoreApplication::setApplicationName("LoaderTests");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    }
    void cleanup() { QSettings().clear(); qunsetenv("ZTGUI_PLUGIN_DESTROY_THROWS"); qunsetenv("ZTGUI_PLUGIN_NESTED_CALLBACK"); qunsetenv("ZTGUI_PLUGIN_LIFETIME_FILE"); qunsetenv("ZTGUI_PLUGIN_CREATE_STEPS"); qunsetenv("ZTGUI_PLUGIN_PREPARATION_STARTED_FILE"); }
    void validPackageRoundTripAndReload() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_GOOD,directory); QVERIFY(!path.isEmpty());
        PluginRegistry registry; PluginLoader loader; QQmlEngine engine; QQuickItem parent;
        QVERIFY(loader.loadFile(path,registry).ok()); QCOMPARE(registry.plugins().size(),1);
        auto page=registry.page("test-dynamic/overview",&engine,&parent); QVERIFY(page.result.ok()); QPointer<QQuickItem> alive=page.page;
        QCOMPARE(loader.loadFile(path,registry).error,Error::DuplicatePlugin);
        QSignalSpy finished(&loader,&PluginLoader::unloadFinished); QVERIFY(loader.unload("test-dynamic",registry).ok());
        QTRY_COMPARE(finished.size(),1); QVERIFY(finished.first()[1].toBool()); QVERIFY(alive.isNull()); QVERIFY(registry.plugins().isEmpty());
        QVERIFY(QSettings().value("plugins/disabled/test-dynamic").toBool());
        loader.scan(directory.path(),registry); QVERIFY(registry.plugins().isEmpty());
        QVERIFY(loader.loadFile(path,registry).ok()); QCOMPARE(registry.plugins().size(),1);
        QVERIFY(!QSettings().contains("plugins/disabled/test-dynamic"));
        loader.requestShutdown(); QTRY_VERIFY(loader.shutdownComplete()); QVERIFY(registry.plugins().isEmpty());
    }
    void rawLibrariesAreIgnoredAndOldApiRejected() {
        QTemporaryDir directory; QFile raw(directory.filePath("raw.dll")); QVERIFY(raw.open(QIODevice::WriteOnly)); raw.write("raw"); raw.close();
        PluginRegistry registry; PluginLoader loader; QVERIFY(loader.scan(directory.path(),registry).isEmpty());
        const auto bad=copy(FIXTURE_BAD,directory); QVERIFY(!bad.isEmpty()); QVERIFY(!loader.loadFile(bad,registry).ok());
        const auto old=copy(FIXTURE_OBSOLETE,directory); QVERIFY(!old.isEmpty()); QVERIFY(!loader.loadFile(old,registry).ok());
        const auto legacy=copy(FIXTURE_LEGACY,directory); QVERIFY(!legacy.isEmpty()); QVERIFY(!loader.loadFile(legacy,registry).ok()); QVERIFY(registry.plugins().isEmpty());
    }
    void metadataAndCiphertextTamperingRejected() {
        QTemporaryDir directory; auto path=copy(FIXTURE_GOOD,directory); auto pkg=package::inspect(path); QVERIFY(pkg.ok());
        QCOMPARE(pkg.info["id"].toString(),QString("test-dynamic"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(file.size()-1));
        const auto byte=file.read(1); QVERIFY(file.seek(file.size()-1)); file.write(QByteArray(1,byte[0]^1)); file.close();
        PluginRegistry registry; PluginLoader loader; QVERIFY(!loader.loadFile(path,registry).ok()); QVERIFY(registry.plugins().isEmpty());
        QFile::remove(path); path=copy(FIXTURE_GOOD,directory); QVERIFY(file.open(QIODevice::ReadWrite));
        auto bytes=file.readAll(); bytes.replace("Dynamic","Altered"); QVERIFY(file.seek(0)); file.write(bytes); file.close();
        QVERIFY(!loader.loadFile(path,registry).ok()); QVERIFY(registry.plugins().isEmpty());
    }
    void incompatibleBuildAndInvalidIdsRejectedBeforeEntry() {
        const auto pkg=package::inspect(FIXTURE_GOOD); QVERIFY(pkg.ok()); auto info=pkg.info;
        for(const auto &key:{"host","platform","architecture","compilerAbi","qtVersion","iid","apiVersion"}) {
            auto changed=info; changed[key]="invalid"; QVERIFY(!package::validate(changed,"client",ApiVersion,ZTGUI_PLUGIN_IID).isEmpty());
        }
        for(const auto &id:{"with.dot","builtin","../escape","中文","with space"}) {
            auto changed=info; changed["id"]=id; QVERIFY(!package::validate(changed,"client",ApiVersion,ZTGUI_PLUGIN_IID).isEmpty());
        }
        PluginRegistry registry; MenuContribution menu; menu.id="overview"; menu.title="Title"; menu.createPage=[](QQmlEngine *,QQuickItem *p) { auto *item=new QQuickItem(p);item->setParent(p);return item; };
        QVERIFY(registry.registerPlugin({{"Mixed-Id","First","1"},{menu}}).ok());
        QCOMPARE(registry.registerPlugin({{"mixed-id","Second","1"},{menu}}).error,Error::DuplicatePlugin);
    }
    void runtimeUnloadDuringInitializationIsResponsive() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_CANCELLABLE_BLOCKED,directory); QVERIFY(!path.isEmpty());
        const auto marker=directory.filePath("started"),log=directory.filePath("trace");
        qputenv("ZTGUI_PLUGIN_PREPARATION_STARTED_FILE",marker.toUtf8()); qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8());
        PluginRegistry registry; PluginLoader loader; QVERIFY(loader.loadFile(path,registry).ok()); QTRY_VERIFY(QFileInfo::exists(marker));
        int ticks=0; QTimer timer; connect(&timer,&QTimer::timeout,this,[&] { ++ticks; }); timer.start(10);
        QElapsedTimer elapsed; elapsed.start(); QVERIFY(loader.unload("test-cancellable_blocked",registry).ok()); QVERIFY(elapsed.elapsed()<50);
        QCOMPARE(loader.loadFile(path,registry).error,Error::DuplicatePlugin);
        QTRY_VERIFY(registry.plugins().isEmpty()); QVERIFY(ticks>2);
        const auto text=trace(log); QCOMPARE(text.count("destroy\n"),1); QCOMPARE(text.count("entry\n"),1);
        QVERIFY(text.indexOf("cancelled:test-cancellable_blocked")<text.indexOf("destroy"));
        QVERIFY(loader.loadFile(path,registry).ok()); loader.requestShutdown(); QTRY_VERIFY(loader.shutdownComplete());
    }
    void activePageAndFactoriesDieBeforeDestroy() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_LIFECYCLE,directory); const auto log=directory.filePath("trace");
        qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8()); PluginRegistry registry; PluginLoader loader; QQmlEngine engine; QQuickItem parent;
        QVERIFY(loader.loadFile(path,registry).ok()); QTRY_COMPARE(registry.preparationState("test-lifecycle"),PreparationState::Ready);
        QVERIFY(registry.page("test-lifecycle/overview",&engine,&parent).result.ok()); QVERIFY(registry.activateMenu("test-lifecycle/overview").ok());
        QVERIFY(registry.activateMenu(SettingsMenuId).ok()); QVERIFY(registry.activateMenu("test-lifecycle/overview").ok());
        QVERIFY(loader.unload("test-lifecycle",registry).ok()); QTRY_VERIFY(registry.plugins().isEmpty());
        const auto text=trace(log); QVERIFY(text.contains("exited:builtin/settings"));
        QVERIFY(text.indexOf("exited:builtin/zerotier")<text.indexOf("page\n"));
        QVERIFY(text.indexOf("page\n")<text.indexOf("factory\n")); QVERIFY(text.indexOf("factory\n")<text.indexOf("destroy\n")); QVERIFY(text.indexOf("destroy\n")<text.indexOf("entry\n"));
    }
    void nestedDataCallbackKeepsLibraryAliveUntilReturn() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_LIFECYCLE,directory); const auto log=directory.filePath("trace");
        qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8()); qputenv("ZTGUI_PLUGIN_NESTED_CALLBACK","1");
        PluginRegistry registry; PluginLoader loader; QVERIFY(loader.loadFile(path,registry).ok()); QTRY_COMPARE(registry.preparationState("test-lifecycle"),PreparationState::Ready);
        QTimer trigger; trigger.setInterval(5); connect(&trigger,&QTimer::timeout,this,[&] {
            if(trace(log).contains("callback-start")) { trigger.stop(); QVERIFY(loader.unload("test-lifecycle",registry).ok()); }
        }); trigger.start();
        QVERIFY(registry.activateMenu("test-lifecycle/overview").ok()); QTRY_VERIFY(registry.plugins().isEmpty());
        const auto text=trace(log); QVERIFY(text.contains("callback-end")); QVERIFY(text.indexOf("callback-end")<text.indexOf("destroy"));
    }
    void cleanupFailureRetainsLibraryAndPreventsReload() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_LIFECYCLE,directory);
        PluginRegistry registry; PluginLoader loader; QVERIFY(loader.loadFile(path,registry).ok()); QTRY_COMPARE(registry.preparationState("test-lifecycle"),PreparationState::Ready);
        qputenv("ZTGUI_PLUGIN_DESTROY_THROWS","1"); QSignalSpy finished(&loader,&PluginLoader::unloadFinished);
        QVERIFY(loader.unload("test-lifecycle",registry).ok()); QTRY_COMPARE(finished.size(),1);
        QVERIFY(!finished.first()[1].toBool()); QVERIFY(finished.first()[2].toString().contains("cleanup")); QVERIFY(registry.plugins().isEmpty());
        QCOMPARE(loader.loadFile(path,registry).error,Error::DuplicatePlugin); loader.requestShutdown(); QTRY_VERIFY(loader.shutdownComplete());
    }
    void initializationFailureCleansOnce() {
        QTemporaryDir directory; const auto path=copy(FIXTURE_LIFECYCLE_FAILED,directory); const auto log=directory.filePath("trace"); qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8());
        PluginRegistry registry; PluginLoader loader; QVERIFY(loader.loadFile(path,registry).ok());
        QTRY_VERIFY(!loader.catalog().isEmpty() && !loader.catalog().first().toMap()["loaded"].toBool());
        QVERIFY(registry.plugins().isEmpty()); QVERIFY(loader.catalog().first().toMap()["message"].toString().contains("failed"));
        const auto text=trace(log); QCOMPARE(text.count("destroy\n"),1); QCOMPARE(text.count("entry\n"),1);
    }
    void shutdownCancelsQueuedTasksAndUnloadsBeforeCompletion() {
        QTemporaryDir directory; const auto log=directory.filePath("trace"); qputenv("ZTGUI_PLUGIN_LIFETIME_FILE",log.toUtf8()); qputenv("ZTGUI_PLUGIN_CREATE_STEPS","1000");
        QVERIFY(QFile::copy(FIXTURE_CANCELLABLE,directory.filePath("a.ztplugin"))); QVERIFY(QFile::copy(FIXTURE_CANCELLABLE_SECOND,directory.filePath("b.ztplugin"))); QVERIFY(QFile::copy(FIXTURE_CANCELLABLE_QUEUED,directory.filePath("c.ztplugin")));
        PluginRegistry registry; PluginLoader loader; loader.scan(directory.path(),registry);
        QTRY_VERIFY(trace(log).contains("started:test-cancellable\n")); QTRY_VERIFY(trace(log).contains("started:test-cancellable_second\n"));
        QSignalSpy finished(&loader,&PluginLoader::shutdownFinished); loader.requestShutdown(); loader.requestShutdown(); QCOMPARE(finished.size(),0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,2000); QVERIFY(registry.plugins().isEmpty());
        const auto text=trace(log); QVERIFY(!text.contains("started:test-cancellable_queued")); QCOMPARE(text.count("destroy\n"),3); QCOMPARE(text.count("entry\n"),3);
    }
    void emptyShutdownIsAsynchronousAndIdempotent() {
        PluginLoader loader; QSignalSpy finished(&loader,&PluginLoader::shutdownFinished); loader.requestShutdown(); loader.requestShutdown(); QCOMPARE(finished.size(),0); QTRY_COMPARE(finished.size(),1);
    }
};
QTEST_MAIN(LoaderTests)
#include "LoaderTests.moc"
