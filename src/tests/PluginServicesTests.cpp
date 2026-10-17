#include "../client/main/plugins/PluginLoader.h"
#include "../client/main/plugins/PluginRegistry.h"
#include "PluginDependencies.h"
#include "PluginRuntime.h"
#include <ztgui/PluginInterface.h>
#include <ztprotocol/ServiceWire.h>
#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include <QQmlEngine>
#include <QQuickItem>
#include <QFile>
#include <QGuiApplication>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace vusiif::ztgui;
class PluginServicesTests:public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    static QJsonObject node(const QString &id,const QJsonArray &deps={}) {
        return {{"id",id},{"version","1.0.0"},{"host","client"},{"formatVersion",3},{"entry","plugin/plugin.dll"},
            {"execution",QJsonObject{{"kind","in-process"},{"entry","plugin/plugin.dll"}}},{"dependencies",deps},{"provides",QJsonArray{}}};
    }
    static QJsonObject dependency(const QString &id) { return {{"pluginId",id},{"versionRange",">=1.0.0 <2.0.0"},{"services",QJsonArray{}}}; }
    static QString provider(const QString &directory) {
        QJsonArray files{QJsonObject{{"path","runtime/bin/fixture.exe"},{"role","entry"},{"source",SERVICE_WORKER}}};
        for(const auto &path:{QString(QT_CORE_LIBRARY),QString(QT_NETWORK_LIBRARY)}) files.append(QJsonObject{{"path","runtime/bin/"+QFileInfo(path).fileName()},{"role","runtime-library"},{"source",path}});
        const auto path=directory+"/provider.ztplugin";
        const QJsonObject manifest{{"files",files},{"execution",QJsonObject{{"kind","worker-process"},{"protocol",protocol::service::Protocol}}},
            {"dependencies",QJsonArray{}},{"provides",QJsonArray{QJsonObject{{"service","fixture.audio"},{"api","1"}},QJsonObject{{"service","fixture.video"},{"api","1"}}}}};
        const auto error=package::writeBundle(manifest,path,{{"host","client"},{"id","fixture-provider"},{"name","Fixture provider"},{"version","1.0.0"},{"apiVersion",api::ApiVersion},{"iid",protocol::service::Protocol}});
        if(!error.isEmpty()) qWarning()<<error; return error.isEmpty()?path:QString();
    }
    static QString copy(const QString &source,const QString &dir) { const auto path=dir+"/"+QFileInfo(source).fileName(); return QFile::copy(source,path)?path:QString(); }
    static bool ready(const plugins::PluginRegistry &r,const QString &id) {
        for(const auto &p:r.plugins()) if(p.id==id) return r.preparationState(id)==plugins::PreparationState::Ready; return false;
    }
#ifdef Q_OS_WIN
    static HANDLE processHandle(qint64 pid) { return OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid)); }
#endif
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("ZTGUI-Service-Tests"); QCoreApplication::setApplicationName("Services");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    }
    void cleanup() { QSettings().clear(); qunsetenv("ZTGUI_SERVICE_TRACE"); qunsetenv("ZTGUI_SERVICE_INIT_FAIL"); qunsetenv("ZTGUI_SERVICE_STOP_FAIL"); qunsetenv("ZTGUI_WORKER_READY_DELAY"); qunsetenv("ZTGUI_WORKER_BAD_CAPABILITY"); qunsetenv("ZTGUI_WORKER_SHUTDOWN_DELAY"); qunsetenv("ZTGUI_FIXTURE_LOG_STARTUP"); qunsetenv("ZTGUI_WORKER_PREPARATION_DELAY"); }
    void versionsAndGraph() {
        QVERIFY(package::versionMatches("1.3.0",">=1.0.0 <2.0.0")); QVERIFY(!package::versionMatches("2.0.0",">=1.0.0 <2.0.0"));
        for(const auto &range:{"^1.0.0","1.x","01.0.0",">=1.0","","1.0.0 || 2.0.0"}) { QString error; QVERIFY(!package::versionMatches("1.0.0",range,&error)); QVERIFY(!error.isEmpty()); }
        QMap<QString,QJsonObject> nodes{{"a",node("a",{dependency("b")})},{"b",node("b",{dependency("c")})},{"c",node("c")}};
        QCOMPARE(package::dependencyPlan(nodes,"A").order,QStringList({"c","b","a"}));
        auto serviceGraph=nodes; auto d=dependency("b"); d["services"]=QJsonArray{QJsonObject{{"service","fixture.audio"},{"api","1"}}}; serviceGraph["a"]=node("a",{d});
        QVERIFY(package::dependencyPlan(serviceGraph,"a").error.contains("Missing service"));
        serviceGraph["a"]=node("a",{dependency("a")}); QVERIFY(package::dependencyPlan(serviceGraph,"a").error.contains("a -> a"));
        QVERIFY(package::dependencyPlan(nodes,"a",{"c"}).error.contains("disabled"));
        nodes["c"]=node("c",{dependency("a")}); QVERIFY(package::dependencyPlan(nodes,"a").error.contains("a -> b -> c -> a"));
        nodes.remove("c"); QVERIFY(package::dependencyPlan(nodes,"a").error.contains("a -> b -> c"));
        nodes["b"]["version"]="2.0.0"; QVERIFY(package::dependencyPlan(nodes,"a").error.contains("version"));
    }
    void boundedBinaryWire() {
        QByteArray buffer=protocol::service::frame({{"test",true}},QByteArray("x\0y",3)); protocol::service::Message m;
        auto partial=buffer.left(10); QCOMPARE(protocol::service::take(partial,m),0); QCOMPARE(protocol::service::take(buffer,m),1); QCOMPARE(m.payload,QByteArray("x\0y",3)); QVERIFY(buffer.isEmpty());
        QVERIFY(protocol::service::frame({},QByteArray(protocol::service::MaxPayload+1,'x')).isEmpty());
        buffer=QByteArray(8,char(0xff)); QCOMPARE(protocol::service::take(buffer,m),-1);
    }
    void missingDisabledAndDuplicateNeverExecuteConsumer() {
        QTemporaryDir dir; const auto path=copy(SERVICE_CONSUMER_A,dir.path()); QVERIFY(!path.isEmpty());
        const auto log=dir.filePath("trace"); qputenv("ZTGUI_SERVICE_TRACE",log.toUtf8());
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QSignalSpy finished(&loader,&plugins::PluginLoader::loadFinished);
        QVERIFY(loader.loadFile(path,registry).ok()); QTRY_COMPARE(finished.size(),1); QVERIFY(!finished.last()[1].toBool()); QVERIFY(!QFileInfo::exists(log));
        QVERIFY(!provider(dir.path()).isEmpty()); QSettings().setValue("plugins/disabled/fixture-provider",true);
        finished.clear(); QVERIFY(loader.loadFile(path,registry).ok()); QTRY_COMPARE(finished.size(),1); QVERIFY(finished.last()[2].toString().contains("disabled")); QVERIFY(!QFileInfo::exists(log));
        QSettings().clear();
        QVERIFY(QFile::copy(dir.filePath("provider.ztplugin"),dir.filePath("duplicate.ztplugin")));
        finished.clear(); QVERIFY(loader.loadFile(path,registry).ok()); QTRY_COMPARE(finished.size(),1); QVERIFY(!finished.last()[1].toBool()); QVERIFY(!QFileInfo::exists(log));
    }
    void sharedProviderAndReverseStop() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path()),b=copy(SERVICE_CONSUMER_B,dir.path());
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QQmlEngine engine; QQuickItem parent;
        int ticks=0; QTimer heartbeat; heartbeat.setInterval(5); connect(&heartbeat,&QTimer::timeout,this,[&] { ++ticks; }); heartbeat.start();
        QVERIFY(loader.loadFile(dir.filePath("provider.ztplugin"),registry).ok());
        QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-provider"),20000);
        QVERIFY(loader.loadFile(a,registry).ok()); QVERIFY(loader.loadFile(b,registry).ok());
        QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-a") && ready(registry,"fixture-b"),20000);
        qint64 pid=0; QString generation; for(const auto &v:loader.catalog()) { const auto row=v.toMap(); if(row["id"]=="fixture-provider") { pid=row["pid"].toLongLong(); generation=row["generation"].toString(); QCOMPARE(row["usedBy"].toStringList().size(),2); } }
        QVERIFY(pid>0); QVERIFY(!generation.isEmpty()); QVERIFY(ticks>2);
        const auto page=registry.page("fixture-b/test",&engine,&parent); QVERIFY(page.result.ok()); auto *fixture=page.page->property("fixture").value<QObject *>(); QVERIFY(fixture);
        QTRY_VERIFY(fixture->property("replies").toInt()>0);
        QVERIFY(loader.unload("fixture-a",registry).ok()); QTRY_COMPARE(registry.plugins().size(),2);
        QVERIFY(QMetaObject::invokeMethod(fixture,"ping")); QTRY_VERIFY(fixture->property("replies").toInt()>1);
        QCOMPARE(loader.unloadImpact("fixture-provider").size(),2);
        const auto blocked=loader.unload("FIXTURE-PROVIDER",registry);
        QVERIFY(!blocked.ok()); QVERIFY(blocked.message.contains("fixture-b")); QCOMPARE(registry.plugins().size(),2);
        QVERIFY(ready(registry,"fixture-provider")); QVERIFY(ready(registry,"fixture-b"));
        QVERIFY(!QSettings().value("plugins/disabled/fixture-provider",false).toBool());
        QVERIFY(QMetaObject::invokeMethod(fixture,"ping")); QTRY_VERIFY(fixture->property("replies").toInt()>2);
        // Already explicitly retained; the UI policy rejects duplicate activation.
        QVERIFY(!loader.loadFile(dir.filePath("provider.ztplugin"),registry).ok());
        QVERIFY(loader.unload("fixture-b",registry).ok()); QTRY_COMPARE(registry.plugins().size(),1);
        QVERIFY(loader.unload("fixture-provider",registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),10000);
#ifdef Q_OS_WIN
        HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid)); if(process) { QCOMPARE(WaitForSingleObject(process,0),DWORD(WAIT_OBJECT_0)); CloseHandle(process); }
#endif
        // Explicitly unloading a provider persistently disables it. Only a
        // direct user load may clear that setting before consumers can restart.
        QVERIFY(loader.loadFile(dir.filePath("provider.ztplugin"),registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-provider"),20000);
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-a"),20000);
        for(const auto &v:loader.catalog()) if(v.toMap()["id"]=="fixture-provider") QVERIFY(v.toMap()["generation"].toString()!=generation);
        loader.requestShutdown(); QTRY_VERIFY_WITH_TIMEOUT(loader.shutdownComplete(),10000);
    }
    void failedConsumerRollsBackAutomaticProvider() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path());
        qputenv("ZTGUI_SERVICE_INIT_FAIL","fixture-a"); plugins::PluginRegistry registry; plugins::PluginLoader loader;
        QSignalSpy finished(&loader,&plugins::PluginLoader::loadFinished);
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(finished.size()>=2,20000); QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),10000);
    }
    void explicitProviderSurvivesFailedConsumer() {
        QTemporaryDir dir; const auto p=provider(dir.path()); const auto a=copy(SERVICE_CONSUMER_A,dir.path());
        plugins::PluginRegistry registry; plugins::PluginLoader loader;
        QVERIFY(loader.loadFile(p,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-provider"),15000);
        QSignalSpy finished(&loader,&plugins::PluginLoader::loadFinished);
        qputenv("ZTGUI_SERVICE_INIT_FAIL","fixture-a"); QVERIFY(loader.loadFile(a,registry).ok());
        QTRY_COMPARE(finished.size(),1); QVERIFY(!finished[0][1].toBool());
        QTRY_COMPARE(registry.plugins().size(),1); QVERIFY(ready(registry,"fixture-provider"));
        loader.requestShutdown(); QTRY_VERIFY(loader.shutdownComplete());
    }
    void stopFailureRetainsProviderAndCanRetry() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path());
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QSignalSpy finished(&loader,&plugins::PluginLoader::unloadFinished);
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-a"),15000);
        qputenv("ZTGUI_SERVICE_STOP_FAIL","fixture-a");
        QVERIFY(!loader.unload("fixture-provider",registry).ok()); QVERIFY(finished.isEmpty());
        QVERIFY(loader.unload("fixture-a",registry).ok()); QTRY_COMPARE(finished.size(),1); QVERIFY(!finished[0][1].toBool());
        QTest::qWait(150); QCOMPARE(registry.plugins().size(),2);
        qunsetenv("ZTGUI_SERVICE_STOP_FAIL"); QVERIFY(loader.unload("fixture-a",registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),10000);
    }
    void crashInvalidatesAndNewInstanceHasNewGeneration() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path());
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QQmlEngine engine; QQuickItem parent;
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-a"),15000);
        QString generation; for(const auto &v:loader.catalog()) if(v.toMap()["id"]=="fixture-provider") generation=v.toMap()["generation"].toString();
        const auto page=registry.page("fixture-a/test",&engine,&parent); QVERIFY(page.result.ok()); auto *fixture=page.page->property("fixture").value<QObject *>(); QVERIFY(fixture);
        QVERIFY(QMetaObject::invokeMethod(fixture,"crashProvider")); QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),10000);
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-a"),15000);
        for(const auto &v:loader.catalog()) if(v.toMap()["id"]=="fixture-provider") QVERIFY(v.toMap()["generation"].toString()!=generation);
        loader.requestShutdown(); QTRY_VERIFY(loader.shutdownComplete());
    }
    void cancellationWhileWaitingForHandshakeRollsBack() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path());
        const auto log=dir.filePath("trace"); qputenv("ZTGUI_SERVICE_TRACE",log.toUtf8()); qputenv("ZTGUI_WORKER_READY_DELAY","400");
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QSignalSpy finished(&loader,&plugins::PluginLoader::loadFinished);
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_COMPARE(registry.plugins().size(),1);
        QVERIFY(loader.unload("fixture-a",registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),10000); QVERIFY(!QFileInfo::exists(log));
        int cancelled=0; for(const auto &event:finished) if(event[0]=="fixture-a" && !event[1].toBool()) ++cancelled; QCOMPARE(cancelled,1);
    }
    void cancellationBeforeIdentityIsKnownNeverExecutesEntry() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path());
        const auto log=dir.filePath("trace"); qputenv("ZTGUI_SERVICE_TRACE",log.toUtf8());
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QSignalSpy finished(&loader,&plugins::PluginLoader::loadFinished);
        QVERIFY(loader.loadFile(a,registry).ok()); QVERIFY(loader.unload(a,registry).ok());
        QTRY_COMPARE(finished.size(),1); QVERIFY(!finished[0][1].toBool()); QVERIFY(registry.plugins().isEmpty()); QVERIFY(!QFileInfo::exists(log));
    }
    void serviceDeclarationsAreAuthenticatedAndCannotDowngrade() {
        QTemporaryDir dir; const auto path=provider(dir.path()); const auto pkg=package::inspect(path); QVERIFY2(pkg.ok(),qPrintable(pkg.error));
        auto downgraded=pkg.info; downgraded["formatVersion"]=2;
        QVERIFY(package::validate(downgraded,"client",api::ApiVersion,ZTGUI_PLUGIN_IID).contains("ZTPK3"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); auto bytes=file.readAll(); QVERIFY(bytes.startsWith("ZTPK3\n"));
        QVERIFY(bytes.contains("fixture.audio")); bytes.replace("fixture.audio","fixture.badio"); QVERIFY(file.seek(0)); QCOMPARE(file.write(bytes),bytes.size()); file.close();
        QVERIFY(package::inspect(path).error.contains("authentication"));
    }
    void workerCapabilitiesMustMatchAuthenticatedManifest() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path());
        const auto log=dir.filePath("trace"); qputenv("ZTGUI_SERVICE_TRACE",log.toUtf8()); qputenv("ZTGUI_WORKER_BAD_CAPABILITY","1");
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QSignalSpy finished(&loader,&plugins::PluginLoader::loadFinished);
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(finished.size()>=2,15000);
        QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),10000); QVERIFY(!QFileInfo::exists(log));
        for(const auto &event:finished) QVERIFY(!event[1].toBool());
    }
    void newConsumerWaitsForAutomaticProviderStop() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path()),b=copy(SERVICE_CONSUMER_B,dir.path());
        qputenv("ZTGUI_WORKER_SHUTDOWN_DELAY","250");
        plugins::PluginRegistry registry; plugins::PluginLoader loader;
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-a"),15000);
        QString generation; for(const auto &v:loader.catalog()) if(v.toMap()["id"]=="fixture-provider") generation=v.toMap()["generation"].toString();
        QVERIFY(loader.unload("FIXTURE-A",registry).ok()); QTRY_COMPARE(registry.plugins().size(),1); QTest::qWait(40);
        QVERIFY(loader.loadFile(b,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-b"),15000);
        QCOMPARE(registry.plugins().size(),2);
        for(const auto &v:loader.catalog()) if(v.toMap()["id"]=="fixture-provider") QVERIFY(v.toMap()["generation"].toString()!=generation);
        loader.requestShutdown(); QTRY_VERIFY(loader.shutdownComplete());
    }
    void rejectedCallbackWithHostContextKeepsDllUntilReturn() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path());
        const auto log=dir.filePath("trace"); qputenv("ZTGUI_SERVICE_TRACE",log.toUtf8());
        const auto text=[&] { QFile f(log); return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray(); };
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QQmlEngine engine; QQuickItem parent;
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-a"),15000);
        const auto page=registry.page("fixture-a/test",&engine,&parent); QVERIFY(page.result.ok()); auto *fixture=page.page->property("fixture").value<QObject *>(); QVERIFY(fixture);
        QTRY_VERIFY(fixture->property("replies").toInt()>0);
        QTimer request; request.setInterval(5); connect(&request,&QTimer::timeout,this,[&] { if(text().contains("rejection-start")) { request.stop(); QVERIFY(loader.unload("fixture-a",registry).ok()); } }); request.start();
        QVERIFY(QMetaObject::invokeMethod(fixture,"nestedRejections")); QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),10000);
        const auto trace=text(); QVERIFY(trace.contains("rejection-end")); QVERIFY(trace.indexOf("rejection-end")<trace.indexOf("destroy:fixture-a"));
    }
    void closeCancelsDeferredRejectionsEvenWithHostContext() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path());
        const auto log=dir.filePath("trace"); qputenv("ZTGUI_SERVICE_TRACE",log.toUtf8());
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QQmlEngine engine; QQuickItem parent;
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-a"),15000);
        const auto page=registry.page("fixture-a/test",&engine,&parent); QVERIFY(page.result.ok()); auto *fixture=page.page->property("fixture").value<QObject *>(); QVERIFY(fixture);
        QTRY_VERIFY(fixture->property("replies").toInt()>0);
        QVERIFY(QMetaObject::invokeMethod(fixture,"canceledRejections")); QVERIFY(loader.unload("fixture-a",registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),10000); QTest::qWait(50);
        QFile file(log); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(!file.readAll().contains("rejection-start"));
    }
    void stopTimeoutTerminatesOwnedTree() {
#ifdef Q_OS_WIN
        QTemporaryDir dir; const auto path=provider(dir.path()); const auto pkg=package::inspect(path); QVERIFY(pkg.ok());
        std::unique_ptr<QTemporaryDir> cache; std::unique_ptr<QLockFile> lock; QString entry;
        QVERIFY(package::extract(pkg,cache,entry,lock).isEmpty());
        qputenv("ZTGUI_WORKER_SHUTDOWN_DELAY","10000");
        plugins::ServiceBroker broker; broker.start(entry,cache->path(),pkg.info); QTRY_VERIFY_WITH_TIMEOUT(broker.ready(),15000);
        auto lease=broker.acquire("fixture.audio","1"); QVERIFY(lease); qint64 childPid=0;
        lease->call("spawn-child",{}, {},this,[&](const util::ServiceResult &r) { childPid=r.data["pid"].toString().toLongLong(); });
        QTRY_VERIFY(childPid>0); HANDLE child=processHandle(childPid),worker=processHandle(broker.pid()); QVERIFY(child); QVERIFY(worker);
        lease->close(nullptr,{}); QTRY_VERIFY(broker.idle()); int ticks=0; QTimer heartbeat;
        connect(&heartbeat,&QTimer::timeout,this,[&] { ++ticks; }); heartbeat.start(10); broker.stop();
        QTRY_VERIFY_WITH_TIMEOUT(broker.exited(),8000); QVERIFY(broker.forced()); QVERIFY(ticks>20);
        QCOMPARE(WaitForSingleObject(worker,0),DWORD(WAIT_OBJECT_0)); QCOMPARE(WaitForSingleObject(child,0),DWORD(WAIT_OBJECT_0));
        CloseHandle(worker); CloseHandle(child); lock.reset(); package::discardCache(cache);
#else
        QSKIP("Windows Job ownership verification");
#endif
    }
    void hostDeathTerminatesOwnedTree() {
#ifdef Q_OS_WIN
        QTemporaryDir dir; const auto path=provider(dir.path()); QVERIFY(!path.isEmpty());
        QProcess host; host.start(QCoreApplication::applicationFilePath(),{"--ownership-host",path});
        QTRY_COMPARE(host.state(),QProcess::Running); QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output+=host.readAllStandardOutput()).contains('\n'),15000);
        const auto pids=output.trimmed().split(' '); QCOMPARE(pids.size(),2);
        HANDLE worker=processHandle(pids[0].toLongLong()),child=processHandle(pids[1].toLongLong()); QVERIFY(worker); QVERIFY(child);
        host.kill(); QTRY_COMPARE(host.state(),QProcess::NotRunning);
        QTRY_COMPARE_WITH_TIMEOUT(WaitForSingleObject(worker,0),DWORD(WAIT_OBJECT_0),5000);
        QTRY_COMPARE_WITH_TIMEOUT(WaitForSingleObject(child,0),DWORD(WAIT_OBJECT_0),5000);
        CloseHandle(worker); CloseHandle(child);
        package::cleanStaleCache();
#else
        QSKIP("Windows Job ownership verification");
#endif
    }
    void workerUsesApplicationLocalRuntime() {
#ifdef Q_OS_WIN
        QTemporaryDir dir; const auto pkg=package::inspect(provider(dir.path())); QVERIFY(pkg.ok());
        std::unique_ptr<QTemporaryDir> cache; std::unique_ptr<QLockFile> lock; QString entry;
        QVERIFY(package::extract(pkg,cache,entry,lock).isEmpty());
        // PATH is searched after the working directory. A runtime next to the
        // executable must win over an unrelated DLL in the working directory.
        QFile poison(cache->filePath("Qt6Core.dll")); QVERIFY(poison.open(QIODevice::WriteOnly)); poison.write("invalid PE"); poison.close();
        const auto previous=SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
        plugins::ServiceBroker broker; QSignalSpy failed(&broker,&plugins::ServiceBroker::failed);
        broker.start(entry,cache->path(),pkg.info);
        QTRY_VERIFY_WITH_TIMEOUT(broker.ready() || !failed.isEmpty(),15000);
        SetErrorMode(previous);
        QVERIFY2(broker.ready(),failed.isEmpty()?"not ready":qPrintable(failed.first()[0].toString()));
        broker.stop(); QTRY_VERIFY_WITH_TIMEOUT(broker.exited(),8000); QVERIFY(!broker.forced());
        lock.reset(); package::discardCache(cache);
#endif
    }
    void workerFailureSurvivesForcedCleanup() {
        QTemporaryDir dir; const auto path=provider(dir.path()); QVERIFY(!path.isEmpty());
        qputenv("ZTGUI_WORKER_BAD_CAPABILITY","1"); qputenv("ZTGUI_WORKER_SHUTDOWN_DELAY","10000");
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QSignalSpy finished(&loader,&plugins::PluginLoader::loadFinished);
        QVERIFY(loader.loadFile(path,registry).ok());
        for(const auto &value:loader.catalog()) {
            const auto row=value.toMap();
            for(const auto &key:{"loadable","loaded","loading","stopping"}) QCOMPARE(row.value(key).metaType(),QMetaType::fromType<bool>());
        }
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(),15000); QVERIFY(!finished.first()[1].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),8000);
        const auto row=loader.catalog().first().toMap();
        QVERIFY(row["forcedTermination"].toBool());
        QVERIFY2(row["message"].toString().contains("capability mismatch"),qPrintable(row["message"].toString()));
        QVERIFY(row["message"].toString().contains("stop timeout"));
    }
    void workerDiagnosticsReachHostWithoutConsole() {
        QTemporaryDir dir; const auto pkg=package::inspect(provider(dir.path())); QVERIFY(pkg.ok());
        std::unique_ptr<QTemporaryDir> cache; std::unique_ptr<QLockFile> lock; QString entry;
        QVERIFY(package::extract(pkg,cache,entry,lock).isEmpty());
        qputenv("ZTGUI_FIXTURE_LOG_STARTUP","1");
        QTest::ignoreMessage(QtWarningMsg,QRegularExpression("\\[worker\\].*fixture-startup"));
        plugins::ServiceBroker broker; broker.start(entry,cache->path(),pkg.info); QTRY_VERIFY_WITH_TIMEOUT(broker.ready(),15000);
        broker.stop(); QTRY_VERIFY_WITH_TIMEOUT(broker.exited(),8000);
        lock.reset(); package::discardCache(cache);
    }
    void slowPreparationDoesNotPublishOrRunConsumerEarly() {
        QTemporaryDir dir; QVERIFY(!provider(dir.path()).isEmpty()); const auto a=copy(SERVICE_CONSUMER_A,dir.path());
        const auto trace=dir.filePath("trace"); qputenv("ZTGUI_SERVICE_TRACE",trace.toUtf8());
        qputenv("ZTGUI_WORKER_PREPARATION_DELAY","18000");
        plugins::PluginRegistry registry; plugins::PluginLoader loader; QSignalSpy finished(&loader,&plugins::PluginLoader::loadFinished);
        QElapsedTimer elapsed; elapsed.start(); int ticks=0; QTimer heartbeat; heartbeat.setInterval(10);
        connect(&heartbeat,&QTimer::timeout,this,[&] { ++ticks; }); heartbeat.start();
        const auto preparing=[&] { for(const auto &v:loader.catalog()) if(v.toMap()["message"]=="Preparing service") return true; return false; };
        QVERIFY(loader.loadFile(a,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(preparing(),5000);
        QTest::qWait(15500); QVERIFY(preparing()); QVERIFY(finished.isEmpty()); QVERIFY(!QFileInfo::exists(trace));
        QVERIFY(!ready(registry,"fixture-a")); QVERIFY(ticks>100);
        QTRY_VERIFY_WITH_TIMEOUT(ready(registry,"fixture-a"),6000);
        QVERIFY(elapsed.elapsed()>=18000); for(const auto &event:finished) QVERIFY(event[1].toBool());
        loader.requestShutdown(); QTRY_VERIFY_WITH_TIMEOUT(loader.shutdownComplete(),10000);
    }
    void preparationRemainsCancellableAfterHandshake() {
        QTemporaryDir dir; const auto path=provider(dir.path()); QVERIFY(!path.isEmpty());
        qputenv("ZTGUI_WORKER_PREPARATION_DELAY","18000");
        plugins::PluginRegistry registry; plugins::PluginLoader loader;
        const auto preparing=[&] { for(const auto &v:loader.catalog()) if(v.toMap()["message"]=="Preparing service") return true; return false; };
        QVERIFY(loader.loadFile(path,registry).ok()); QTRY_VERIFY_WITH_TIMEOUT(preparing(),5000);
        QElapsedTimer elapsed; elapsed.start(); QVERIFY(loader.unload("fixture-provider",registry).ok());
        QTRY_VERIFY_WITH_TIMEOUT(registry.plugins().isEmpty(),2500); QVERIFY(elapsed.elapsed()<2500);
        for(const auto &row:loader.catalog()) QVERIFY(!row.toMap()["forcedTermination"].toBool());
    }
};
int main(int argc,char **argv) {
    QGuiApplication app(argc,argv);
    if(app.arguments().contains("--ownership-host")) {
        const auto pkg=package::inspect(app.arguments().last()); if(!pkg.ok()) return 2;
        std::unique_ptr<QTemporaryDir> cache; std::unique_ptr<QLockFile> lock; QString entry;
        if(!package::extract(pkg,cache,entry,lock).isEmpty()) return 3;
        plugins::ServiceBroker broker;
        std::shared_ptr<util::ServiceLease> lease;
        QObject::connect(&broker,&plugins::ServiceBroker::becameReady,&app,[&] {
            lease=broker.acquire("fixture.audio","1"); if(!lease) { app.exit(4); return; }
            lease->call("spawn-child",{}, {},&app,[&](const util::ServiceResult &r) {
                const auto line=QString::number(broker.pid())+' '+r.data["pid"].toString()+'\n';
                const auto bytes=line.toLatin1(); std::fwrite(bytes.constData(),1,size_t(bytes.size()),stdout); std::fflush(stdout);
            });
        });
        QTimer::singleShot(20000,&app,[]{ QCoreApplication::exit(5); });
        broker.start(entry,cache->path(),pkg.info); return app.exec();
    }
    PluginServicesTests tests; return QTest::qExec(&tests,argc,argv);
}
#include "PluginServicesTests.moc"
