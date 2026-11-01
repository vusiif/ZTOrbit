#include "PluginPackage.h"
#include "PluginRuntime.h"
#include <zto/PluginInterface.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QtEndian>
#include <QtTest>
#include <QQuickItem>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonObject>
namespace vusiif::zto::package { QString writeLegacy(const QString &,const QString &,QJsonObject); }
using namespace vusiif::zto::package;
class PluginPackageTests : public QObject {
    Q_OBJECT
    QJsonObject info() const {
        return {{"host","client"},{"id","test-dynamic"},{"name","Dynamic"},{"version","1"},{"apiVersion",QString::fromLatin1(vusiif::zto::api::ApiVersion)},{"iid","vusiif.zto.api.PluginInterface/3.0"}};
    }
    QJsonObject manifest(const QString &resource) const {
        return {{"files",QJsonArray{
            QJsonObject{{"path","plugin/plugin.dll"},{"role","entry"},{"source",QString(FIXTURE_LIBRARY)}},
            QJsonObject{{"path","assets/data.bin"},{"role","resource"},{"source",resource}},
            QJsonObject{{"path","licenses/license.txt"},{"role","license"},{"source",resource}}
        }},{"runtime",QJsonObject{}},{"requiresHost",QJsonArray{}}};
    }
    void save(const QString &path,const QByteArray &data) {
        QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data),data.size());
    }
    QByteArray read(const QString &path) { QFile f(path); if(!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
private slots:
    void packageExtensionDoesNotChangeAuthenticatedContents() {
        QTemporaryDir temp;
        const auto resource=temp.filePath("data"),path=temp.filePath("test.z2plugin");
        save(resource,"license");
        QVERIFY(writeBundle(manifest(resource),path,info()).isEmpty());
        QVERIFY(inspect(path).ok());
        const auto oldPath=temp.filePath("test.ztplugin");
        QVERIFY(QFile::copy(path,oldPath));
        QVERIFY(!inspect(oldPath).ok());
        const auto renamed=temp.filePath("renamed.Z2PLUGIN");
        QVERIFY(QFile::rename(oldPath,renamed));
        QVERIFY(inspect(renamed).ok());
    }
    void linkedInputsAndLicenseDestinationsAreRejected() {
        QTemporaryDir temp; const auto real=temp.filePath("real"),link=temp.filePath("linked");
        QVERIFY(QDir().mkpath(real)); QVERIFY(QFile::copy(FIXTURE_LIBRARY,QDir(real).filePath("entry.dll")));
#ifdef Q_OS_WIN
        const int linked=QProcess::execute("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(real)});
        if(linked!=0) QSKIP("Cannot create an NTFS junction in this environment");
#else
        if(!QFile::link(real,link)) QSKIP("Cannot create a symlink");
#endif
        const auto data=temp.filePath("data"),path=temp.filePath("linked-test.z2plugin"); save(data,"license");
        auto bundle=manifest(data); auto files=bundle["files"].toArray(); auto item=files[0].toObject();
        item["source"]=QDir(link).filePath("entry.dll"); files[0]=item; bundle["files"]=files;
        QVERIFY(writeBundle(bundle,path,info()).contains("Linked"));
        QVERIFY(writeBundle(manifest(data),path,info()).isEmpty()); const auto p=inspect(path); QVERIFY(p.ok());
        QVERIFY(extractLicenses(p,link).contains("Linked")); QVERIFY(!QFileInfo(QDir(real).filePath("licenses")).exists());
        QVERIFY(QDir().rmdir(link)); // Remove the junction itself, never recursively follow it.
    }
    void dependencyClosureReuseAndConflicts() {
#ifdef Q_OS_WIN
        QQuickItem hostQuickItem; // Model a host that actually links its declared Quick base runtime.
        QTemporaryDir temp; const auto resource=temp.filePath("data"),path=temp.filePath("runtime.z2plugin"); save(resource,"x");
        auto bundle=manifest(resource); auto files=bundle["files"].toArray();
        files.append(QJsonObject{{"path","runtime/bin/zt_runtime_probe.dll"},{"role","runtime-library"},{"source",QString(RUNTIME_LIBRARY)}});
        bundle["files"]=files;
        QVERIFY(writeBundle(bundle,path,info()).isEmpty()); auto p=inspect(path); QVERIFY(p.ok());
        std::unique_ptr<QTemporaryDir> directory; std::unique_ptr<QLockFile> lock; QString entry;
        QVERIFY(extract(p,directory,entry,lock).isEmpty()); const auto error=activateRuntime(p,directory->path()); QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto initial=runtimeStatistics(); QCOMPARE(initial["directories"].toInt(),1); QCOMPARE(initial["handles"].toInt(),1);
        // Business/resource updates must reuse exactly the same runtime directory.
        save(resource,"updated business resource"); QVERIFY(writeBundle(bundle,path,info()).isEmpty()); p=inspect(path);
        lock.reset(); discardCache(directory); QVERIFY(extract(p,directory,entry,lock).isEmpty());
        QVERIFY(activateRuntime(p,directory->path()).isEmpty()); QCOMPARE(runtimeStatistics(),initial);
        auto caseBundle=bundle; auto caseFiles=caseBundle["files"].toArray(); auto caseItem=caseFiles.last().toObject();
        caseItem["path"]="runtime/bin/ZT_RUNTIME_PROBE.DLL"; caseFiles[caseFiles.size()-1]=caseItem; caseBundle["files"]=caseFiles;
        QVERIFY(writeBundle(caseBundle,path,info()).isEmpty()); p=inspect(path); QVERIFY(p.ok());
        lock.reset(); discardCache(directory); QVERIFY(extract(p,directory,entry,lock).isEmpty());
        QVERIFY(activateRuntime(p,directory->path()).isEmpty()); QCOMPARE(runtimeStatistics(),initial);
        const auto altered=temp.filePath("altered.dll"); save(altered,read(RUNTIME_LIBRARY)+"different build");
        auto item=files.last().toObject(); item["source"]=altered; files[files.size()-1]=item; bundle["files"]=files;
        QVERIFY(writeBundle(bundle,path,info()).isEmpty()); p=inspect(path); QVERIFY(p.ok());
        lock.reset(); discardCache(directory); QVERIFY(extract(p,directory,entry,lock).isEmpty());
        QVERIFY(activateRuntime(p,directory->path()).contains("conflict")); QCOMPARE(runtimeStatistics(),initial);
        // An import absent from host/system/bundle fails before loading the entry.
        const auto missing=temp.filePath("missing.dll"); auto bytes=read(FIXTURE_LIBRARY);
        QVERIFY(bytes.contains("Qt6Core.dll")); bytes.replace("Qt6Core.dll","NoCoreX.dll"); save(missing,bytes);
        auto missingBundle=manifest(resource); auto missingFiles=missingBundle["files"].toArray(); item=missingFiles[0].toObject(); item["source"]=missing; missingFiles[0]=item; missingBundle["files"]=missingFiles;
        QVERIFY(writeBundle(missingBundle,path,info()).isEmpty()); p=inspect(path); QVERIFY(p.ok());
        lock.reset(); discardCache(directory); QVERIFY(extract(p,directory,entry,lock).isEmpty());
        QVERIFY(activateRuntime(p,directory->path()).contains("Missing runtime dependency")); QVERIFY(!businessLibraryMapped(entry));
#endif
    }
    void chunksAndEmptyFilesRoundTrip() {
        for(int size:{0,1,1024*1024,1024*1024+57}) {
            QTemporaryDir temp; const auto resource=temp.filePath("data"),package=temp.filePath("test.z2plugin");
            const QByteArray bytes(size,'x'); save(resource,bytes);
            const auto error=writeBundle(manifest(resource),package,info()); QVERIFY2(error.isEmpty(),qPrintable(error));
            const auto p=inspect(package); QVERIFY2(p.ok(),qPrintable(p.error)); QCOMPARE(p.info["formatVersion"].toInt(),2); QVERIFY(p.encrypted.isEmpty());
            std::unique_ptr<QTemporaryDir> directory; std::unique_ptr<QLockFile> lock; QString entry;
            QVERIFY(extract(p,directory,entry,lock).isEmpty()); QVERIFY(QFileInfo(entry).isFile());
            QCOMPARE(read(directory->filePath("assets/data.bin")),bytes);
            QVERIFY(extractLicenses(p,temp.filePath("export")).isEmpty());
            QCOMPARE(read(temp.filePath("export/licenses/license.txt")),bytes);
            QVERIFY(!QFileInfo(temp.filePath("export/plugin/plugin.dll")).exists());
        }
    }
    void authenticationAndExactSize() {
        QTemporaryDir temp; const auto resource=temp.filePath("data"),path=temp.filePath("test.z2plugin");
        save(resource,QByteArray(2*1024*1024+20,'x')); QVERIFY(writeBundle(manifest(resource),path,info()).isEmpty());
        const auto original=read(path); const auto length=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(original.constData()+6)); const int body=10+length;
        for(int index:{11,body,body+25,int(original.size()-1)}) {
            auto changed=original; changed[index]=changed[index]^1; save(path,changed); QVERIFY(!inspect(path).ok());
        }
        save(path,original.left(original.size()-1)); QVERIFY(!inspect(path).ok());
        save(path,original+"extra"); QVERIFY(!inspect(path).ok());
        // Swapping whole equal-length chunks must fail indexed AAD.
        auto changed=original; const auto firstFile=inspect(path).info["files"].toArray()[0].toObject()["size"].toDouble();
        const int resourceStart=body+int(firstFile)+40*qMax(1,(int(firstFile)+1024*1024-1)/(1024*1024));
        const int blockBytes=1024*1024+40;
        const auto firstBlock=changed.mid(resourceStart,blockBytes),secondBlock=changed.mid(resourceStart+blockBytes,blockBytes);
        changed.replace(resourceStart,blockBytes,secondBlock); changed.replace(resourceStart+blockBytes,blockBytes,firstBlock);
        save(path,changed); QVERIFY(!inspect(path).ok());
        save(path,original); auto p=inspect(path); QVERIFY(p.ok()); save(path,original+"extra");
        std::unique_ptr<QTemporaryDir> directory; std::unique_ptr<QLockFile> lock; QString entry;
        QVERIFY(!extract(p,directory,entry,lock).isEmpty()); QVERIFY(!directory);
    }
    void unsafePathsAndLimits() {
        for(const auto &path:{"../x","/x","C:/x","a//b","a/./b","a/../b","a\\b","a:b","con","NUL.txt","x.","x ","a/LPT1.dll","a/<x"}) QVERIFY2(!safeRelativePath(path).isEmpty(),path);
        QVERIFY(safeRelativePath("licenses/组件.txt").isEmpty());
        QTemporaryDir temp; const auto resource=temp.filePath("data"),path=temp.filePath("test.z2plugin");
        save(resource,"x"); QVERIFY(writeBundle(manifest(resource),path,info()).isEmpty()); auto p=inspect(path); QVERIFY(p.ok());
        for(const auto &bad:{"../escape","assets/DATA.bin","assets/data.bin/child"}) {
            auto info=p.info; auto files=info["files"].toArray(); auto item=files[2].toObject(); item["path"]=bad; item["role"]="resource"; files[2]=item; info["files"]=files;
            QVERIFY(!validBundle(info).isEmpty());
        }
        for(double size:{-1.,0.5,256.*1024*1024+1}) {
            auto info=p.info; auto files=info["files"].toArray(); auto item=files[1].toObject(); item["size"]=size; files[1]=item; info["files"]=files; QVERIFY(!validBundle(info).isEmpty());
        }
        auto info=p.info; auto files=info["files"].toArray();
        while(files.size()<513) files.append(files[1]); info["files"]=files; QVERIFY(!validBundle(info).isEmpty());
    }
    void legacyStillLoads() {
        QTemporaryDir temp; const auto path=temp.filePath("legacy.z2plugin"); const auto error=writeLegacy(FIXTURE_LIBRARY,path,info()); QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto p=inspect(path); QVERIFY2(p.ok(),qPrintable(p.error)); QCOMPARE(p.info["formatVersion"].toInt(),1);
        std::unique_ptr<QTemporaryDir> directory; std::unique_ptr<QLockFile> lock; QString entry;
        QVERIFY(extract(p,directory,entry,lock).isEmpty()); QCOMPARE(read(entry),read(FIXTURE_LIBRARY));
    }
    void legacyIgnoresBundleFieldsAndUsesProvidedHostFiles() {
#ifdef Q_OS_WIN
        QTemporaryDir temp; const auto data=temp.filePath("data"),path=temp.filePath("legacy-deps.z2plugin"); save(data,"license");
        QVERIFY(writeBundle(manifest(data),path,info()).isEmpty()); auto p=inspect(path); QVERIFY(p.ok());
        auto legacyInfo=p.info; auto legacyManifest=legacyInfo["files"].toArray();
        legacyManifest.append(QJsonObject{{"path","../../outside.dll"},{"role","runtime-library"},{"size",1},{"sha256",QString(64,'0')}});
        legacyInfo["files"]=legacyManifest;
        QVERIFY(writeLegacy(FIXTURE_LIBRARY,path,legacyInfo).isEmpty()); p=inspect(path); QVERIFY(p.ok());
        std::unique_ptr<QTemporaryDir> directory; std::unique_ptr<QLockFile> lock; QString entry;
        QVERIFY(extract(p,directory,entry,lock).isEmpty()); const auto before=runtimeStatistics();
        QVERIFY(activateRuntime(p,directory->path()).isEmpty()); QCOMPARE(runtimeStatistics(),before);
        // An old package may use an extra library already deployed beside the host.
        auto bytes=read(FIXTURE_LIBRARY); QVERIFY(bytes.contains("Qt6Core.dll")); bytes.replace("Qt6Core.dll","hostlib.dll");
        const auto input=temp.filePath("legacy.dll"); save(input,bytes); QVERIFY(writeLegacy(input,path,info()).isEmpty());
        p=inspect(path); QVERIFY(p.ok()); lock.reset(); discardCache(directory); QVERIFY(extract(p,directory,entry,lock).isEmpty());
        const auto provided=QCoreApplication::applicationDirPath()+"/hostlib.dll";
        if(!QFileInfo(provided).exists()) QVERIFY(QFile::copy(RUNTIME_LIBRARY,provided));
        const auto error=activateRuntime(p,directory->path()); QVERIFY2(error.isEmpty(),qPrintable(error));
#endif
    }
    void cacheSessionOwnershipAndCrashRecovery() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("ZTOrbit-Package-Tests");
        QCoreApplication::setApplicationName("PluginPackageTests");
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto root = [&directory](const QString &run) {
            return QDir(directory.path()).filePath(QStringLiteral("plugin-packages/user/") + run);
        };
        const auto indexFile = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                   .filePath("cache-index.json");
        QFile::remove(indexFile);
        const auto writeOwner = [](const QString &path, const QString &run, qint64 pid, const QString &token,
                                   const QString &processStart = {}) {
            QDir().mkpath(path);
            QFile workers(QDir(path).filePath("workers.json"));
            if (!workers.open(QIODevice::WriteOnly) || workers.write("[]") != 2) return false;
            QFile file(QDir(path).filePath("owner.json"));
            if (!file.open(QIODevice::WriteOnly)) return false;
            const auto bytes = QJsonDocument(QJsonObject{{"format",1},{"runId",run},{"userKey","user"},
                {"installKey","install"},{"root",path},{"pid",double(pid)},
                {"processStart",processStart},{"token",token}, {"startedAt","2026-01-01T00:00:00Z"}}).toJson();
            return file.write(bytes) == bytes.size();
        };
        const auto writeIndexEntry = [&indexFile](const QString &run, const QString &path, const QString &token) {
            QJsonArray entries;
            QFile input(indexFile);
            if (input.open(QIODevice::ReadOnly)) {
                const auto document = QJsonDocument::fromJson(input.readAll());
                if (document.isArray()) entries = document.array();
            }
            QJsonArray kept;
            for (const auto &value : entries)
                if (value.toObject().value("runId").toString() != run) kept.append(value);
            kept.append(QJsonObject{{"runId",run},{"root",path},{"userKey","user"},
                {"installKey","install"},{"token",token},{"startedAt","2026-01-01T00:00:00Z"}});
            QDir().mkpath(QFileInfo(indexFile).absolutePath());
            QFile output(indexFile);
            if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            const auto bytes = QJsonDocument(kept).toJson(QJsonDocument::Compact);
            return output.write(bytes) == bytes.size();
        };
        const auto indexedRoot = [&indexFile](const QString &run) {
            QFile input(indexFile);
            if (!input.open(QIODevice::ReadOnly)) return QString();
            const auto document = QJsonDocument::fromJson(input.readAll());
            if (!document.isArray()) return QString();
            for (const auto &value : document.array()) {
                const auto object = value.toObject();
                if (object.value("runId").toString() == run) return object.value("root").toString();
            }
            return QString();
        };
        CacheContext context;
        context.root = root("run-a"); context.userKey = "user"; context.runId = "run-a";
        context.installKey = "install";
        const auto firstPrepare = prepareCacheSession(context);
        QVERIFY2(firstPrepare.isEmpty(), qPrintable(firstPrepare));
        QVERIFY(!context.token.isEmpty());
        QVERIFY(QFileInfo::exists(QDir(context.root).filePath("owner.json")));
        QVERIFY(QFileInfo::exists(QDir(context.root).filePath("active.lock")));
        QFile ownerFile(QDir(context.root).filePath("owner.json"));
        QVERIFY(ownerFile.open(QIODevice::ReadOnly));
        const auto owner = QJsonDocument::fromJson(ownerFile.readAll()).object();
        QCOMPARE(owner.value("token").toString(), context.token);
        QVERIFY(!owner.value("processStart").toString().isEmpty());
        std::unique_ptr<QTemporaryDir> cache; std::unique_ptr<QLockFile> lock;
        QVERIFY(createCache("load",context,cache,lock).isEmpty());
        QVERIFY(cache->path().startsWith(context.root));
        lock.reset(); discardCache(cache);
        // A dead owner is reclaimed; a live owner and this process are kept.
        const auto dead = root("run-dead");
        QVERIFY(writeOwner(dead,"run-dead",999999,"tok-dead"));
        cleanStaleCache(context);
        QVERIFY(!QFileInfo::exists(dead));
        QVERIFY(QFileInfo::exists(context.root));
        // A session without the worker marker may predate worker tracking; its
        // worker state is unknown and the directory must be retained.
        const auto legacySession = root("run-legacy");
        QVERIFY(writeOwner(legacySession,"run-legacy",999999,"tok-legacy"));
        QVERIFY(QFile::remove(QDir(legacySession).filePath("workers.json")));
        cleanStaleCache(context);
        QVERIFY(QFileInfo::exists(legacySession));
        const auto live = root("run-live");
        QVERIFY(writeOwner(live,"run-live",QCoreApplication::applicationPid(),"tok-live",
            processStartIdentity(QCoreApplication::applicationPid())));
        cleanStaleCache(context);
        QVERIFY(QFileInfo::exists(live));
        // The same PID with a different creation identity is a recycled PID and
        // must not protect an abandoned session.
        const auto reused = root("run-reused");
        QVERIFY(writeOwner(reused,"run-reused",QCoreApplication::applicationPid(),"tok-reused","win:recycled"));
        cleanStaleCache(context);
        QVERIFY(!QFileInfo::exists(reused));
        // A live worker recorded by a dead host blocks reclaiming its session.
        const auto workerSession = root("run-worker");
        QVERIFY(writeOwner(workerSession,"run-worker",999999,"tok-worker"));
        const auto workerLoad = QDir(workerSession).filePath("load-worker");
        QVERIFY(QDir().mkpath(workerLoad));
        QVERIFY2(registerCacheWorker(workerLoad,QCoreApplication::applicationPid()).isEmpty(),
                 "worker registration");
        cleanStaleCache(context);
        QVERIFY(QFileInfo::exists(workerSession));
        unregisterCacheWorker(workerLoad,QCoreApplication::applicationPid());
        cleanStaleCache(context);
        QVERIFY(!QFileInfo::exists(workerSession));
        // The hidden cleanup entry point accepts only an indexed identity.
        const auto helper = root("run-helper");
        QVERIFY(writeOwner(helper,"run-helper",999999,"tok-helper"));
        QVERIFY(writeIndexEntry("run-helper",helper,"tok-helper"));
        QCOMPARE(runCacheCleanupHelper({"--cache-run","run-helper","--cache-user","user","--cache-token","tok-helper"}),0);
        QVERIFY(!QFileInfo::exists(helper));
        QVERIFY(indexedRoot("run-helper").isEmpty());
        const auto unregistered = root("run-unregistered");
        QVERIFY(writeOwner(unregistered,"run-unregistered",999999,"tok-unregistered"));
        QCOMPARE(runCacheCleanupHelper({"--cache-run","run-unregistered","--cache-user","user",
                                        "--cache-token","tok-unregistered"}),2);
        QVERIFY(QFileInfo::exists(unregistered));
        const auto tokenMismatch = root("run-token");
        QVERIFY(writeOwner(tokenMismatch,"run-token",999999,"correct"));
        QVERIFY(writeIndexEntry("run-token",tokenMismatch,"different"));
        QCOMPARE(runCacheCleanupHelper({"--cache-run","run-token","--cache-user","user",
                                        "--cache-token","different"}),1);
        QVERIFY(QFileInfo::exists(tokenMismatch));
        // An unregistered index forces prepare to fail rather than extract.
        QFile::remove(indexFile);
        QVERIFY(QDir().mkpath(indexFile));
        CacheContext unprepared;
        unprepared.root = root("run-index-fail"); unprepared.userKey = "user";
        unprepared.runId = "run-index-fail"; unprepared.installKey = "install";
        QVERIFY(!prepareCacheSession(unprepared).isEmpty());
        QVERIFY(indexedRoot("run-index-fail").isEmpty());
        QVERIFY(QDir(indexFile).removeRecursively());
        CacheContext outside;
        outside.root = QDir(directory.path()).filePath("outside/run");
        outside.userKey = "user"; outside.runId = "run";
        QVERIFY(!prepareCacheSession(outside).isEmpty());
        QFile::remove(QDir(live).filePath("owner.json"));
        QFile::remove(QDir(tokenMismatch).filePath("owner.json"));
        QFile::remove(indexFile);
    }

    void contractRejectsKitAndHostOverrides() {
        auto p=inspect(FIXTURE_PACKAGE); QVERIFY(p.ok()); QVERIFY(!hostRuntimeContract().isEmpty());
        auto files=p.info["files"].toArray(); auto item=files[0].toObject();
        item["role"]="runtime-library"; item["path"]="runtime/bin/Qt6Multimedia.dll"; files.append(item);
        p.info["files"]=files; p.info["runtime"]=QJsonObject{{"qtBuildId","wrong"}};
        QVERIFY(validateRuntime(p).contains("worker-process"));
        item["path"]="runtime/bin/Qt6Core.dll"; files[1]=item; p.info["files"]=files;
        QVERIFY(validateRuntime(p).contains("override"));
        p.info["execution"]=QJsonObject{{"kind","worker-process"}};
        p.info["requiresHost"]=QJsonArray{};
        QVERIFY(validateRuntime(p).contains("runtime/bin"));
        p.info["entry"]="runtime/bin/worker.exe";
        // A worker owns its Qt runtime and does not share the host's Kit ABI.
        QVERIFY(validateRuntime(p).isEmpty());
    }
};
QTEST_GUILESS_MAIN(PluginPackageTests)
#include "PluginPackageTests.moc"
