#include "PluginPackage.h"
#include "PluginRuntime.h"
#include <ztgui/PluginInterface.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QtEndian>
#include <QtTest>
#include <QQuickItem>
#include <QProcess>
namespace vusiif::ztgui::package { QString writeLegacy(const QString &,const QString &,QJsonObject); }
using namespace vusiif::ztgui::package;
class PluginPackageTests : public QObject {
    Q_OBJECT
    QJsonObject info() const {
        return {{"host","client"},{"id","test-dynamic"},{"name","Dynamic"},{"version","1"},{"apiVersion",QString::fromLatin1(vusiif::ztgui::api::ApiVersion)},{"iid","vusiif.ztgui.api.PluginInterface/3.0"}};
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
    void linkedInputsAndLicenseDestinationsAreRejected() {
        QTemporaryDir temp; const auto real=temp.filePath("real"),link=temp.filePath("linked");
        QVERIFY(QDir().mkpath(real)); QVERIFY(QFile::copy(FIXTURE_LIBRARY,QDir(real).filePath("entry.dll")));
#ifdef Q_OS_WIN
        const int linked=QProcess::execute("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(real)});
        if(linked!=0) QSKIP("Cannot create an NTFS junction in this environment");
#else
        if(!QFile::link(real,link)) QSKIP("Cannot create a symlink");
#endif
        const auto data=temp.filePath("data"),path=temp.filePath("linked-test.ztplugin"); save(data,"license");
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
        QTemporaryDir temp; const auto resource=temp.filePath("data"),path=temp.filePath("runtime.ztplugin"); save(resource,"x");
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
            QTemporaryDir temp; const auto resource=temp.filePath("data"),package=temp.filePath("test.ztplugin");
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
        QTemporaryDir temp; const auto resource=temp.filePath("data"),path=temp.filePath("test.ztplugin");
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
        QTemporaryDir temp; const auto resource=temp.filePath("data"),path=temp.filePath("test.ztplugin");
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
        QTemporaryDir temp; const auto path=temp.filePath("legacy.ztplugin"); const auto error=writeLegacy(FIXTURE_LIBRARY,path,info()); QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto p=inspect(path); QVERIFY2(p.ok(),qPrintable(p.error)); QCOMPARE(p.info["formatVersion"].toInt(),1);
        std::unique_ptr<QTemporaryDir> directory; std::unique_ptr<QLockFile> lock; QString entry;
        QVERIFY(extract(p,directory,entry,lock).isEmpty()); QCOMPARE(read(entry),read(FIXTURE_LIBRARY));
    }
    void legacyIgnoresBundleFieldsAndUsesProvidedHostFiles() {
#ifdef Q_OS_WIN
        QTemporaryDir temp; const auto data=temp.filePath("data"),path=temp.filePath("legacy-deps.ztplugin"); save(data,"license");
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
