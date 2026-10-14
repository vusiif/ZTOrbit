#include "PluginRuntime.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <QSet>
#include <QMap>
#include <QtEndian>
#include <vector>
#include <functional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace vusiif::ztgui::package {
QJsonObject hostRuntimeContract() {
#ifdef QT_DEBUG
    QFile file(":/ztgui/contracts/Debug.json");
#else
    QFile file(":/ztgui/contracts/Release.json");
#endif
    if(!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool businessLibraryMapped(const QString &path) {
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    return GetModuleHandleW(reinterpret_cast<LPCWSTR>(native.utf16()))!=nullptr;
#else
    Q_UNUSED(path); return false;
#endif
}
static QString digest(const QString &path) {
    QFile file(path); if(!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256); if(!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}
QStringList windowsImports(const QString &path,QString *error) {
    QStringList result;
#ifdef Q_OS_WIN
    QFile file(path); if(!file.open(QIODevice::ReadOnly)) { if(error) *error=file.errorString(); return {}; }
    const qint64 size=file.size(); const auto *data=file.map(0,size);
    auto fail=[&] { if(error) *error="Invalid PE import table: "+path; };
    if(!data || size<64 || qFromLittleEndian<quint16>(data)!=0x5a4d) { fail(); return {}; }
    auto fits=[&](quint64 at,quint64 count) { return at<=quint64(size) && count<=quint64(size)-at; };
    const quint32 pe=qFromLittleEndian<quint32>(data+60);
    if(!fits(pe,24) || qFromLittleEndian<quint32>(data+pe)!=0x4550) { fail(); return {}; }
    const quint16 sections=qFromLittleEndian<quint16>(data+pe+6),optional=qFromLittleEndian<quint16>(data+pe+20);
    const quint64 opt=quint64(pe)+24, table=opt+optional;
    if(!fits(opt,optional) || !fits(table,quint64(sections)*40) || optional<112) { fail(); return {}; }
    const quint16 magic=qFromLittleEndian<quint16>(data+opt);
    if(magic!=0x20b && magic!=0x10b) { fail(); return {}; }
    const quint64 dirs=opt+(magic==0x20b?112:96);
    auto offset=[&](quint32 rva)->qint64 {
        for(int i=0;i<sections;++i) {
            const auto *s=data+table+i*40; const quint32 va=qFromLittleEndian<quint32>(s+12),raw=qFromLittleEndian<quint32>(s+20),len=qFromLittleEndian<quint32>(s+16);
            if(rva>=va && quint64(rva)-va<len && fits(quint64(raw)+rva-va,1)) return quint64(raw)+rva-va;
        } return -1;
    };
    for(const int directory:{1,13}) {
        if(dirs+directory*8+8>opt+optional) continue;
        const quint32 rva=qFromLittleEndian<quint32>(data+dirs+directory*8),length=qFromLittleEndian<quint32>(data+dirs+directory*8+4);
        if(!rva) continue;
        qint64 at=offset(rva); const int stride=directory==1?20:32;
        if(at<0 || !fits(at,length)) { fail(); return {}; }
        bool terminated=false;
        for(quint32 consumed=0;consumed+stride<=length;consumed+=stride,at+=stride) {
            if(!fits(at,stride)) { fail(); return {}; }
            const quint32 name=qFromLittleEndian<quint32>(data+at+(directory==1?12:4));
            if(!name) { terminated=true; break; }
            if(directory==13 && !(qFromLittleEndian<quint32>(data+at)&1)) { fail(); return {}; }
            const qint64 start=offset(name); if(start<0) { fail(); return {}; }
            qint64 end=start; while(end<size && end-start<260 && data[end]) ++end;
            if(end==size || end-start>=260) { fail(); return {}; }
            const auto imported=QString::fromLatin1(reinterpret_cast<const char *>(data+start),end-start).toLower();
            if(imported.contains('/') || imported.contains('\\') || imported.contains(':')) { fail(); return {}; }
            if(!result.contains(imported)) result.append(imported);
        }
        if(!terminated) { fail(); return {}; }
    }
#else
    Q_UNUSED(path); Q_UNUSED(error);
#endif
    return result;
}
static bool systemLibrary(const QString &name) {
#ifdef Q_OS_WIN
    if(name.startsWith("api-ms-win-") || name.startsWith("ext-ms-win-")) return true;
    wchar_t system[MAX_PATH]; const auto length=GetSystemDirectoryW(system,MAX_PATH);
    return length>0 && length<MAX_PATH && QFileInfo(QDir(QString::fromWCharArray(system,length)).filePath(name)).isFile();
#else
    Q_UNUSED(name); return false;
#endif
}
static QSet<QString> baseNames(const QJsonObject &contract) {
    QSet<QString> result; for(const auto &v:contract["baseLibraries"].toArray()) result.insert(v.toString().toLower()); return result;
}
QString validateRuntime(const PackageResult &p) {
    if(!p.ok()) return p.error;
    if(p.info["formatVersion"].toInt()==1) return {};
    const auto host=hostRuntimeContract(),runtime=p.info["runtime"].toObject(); const auto bases=baseNames(host);
    bool privateQt=false; QSet<QString> names;
    for(const auto &value:p.info["files"].toArray()) {
        const auto item=value.toObject(); const auto role=item["role"].toString();
        if(role!="runtime-library" && role!="qt-plugin") continue;
        const auto name=QFileInfo(item["path"].toString()).fileName().toLower();
        if(names.contains(name)) return "Duplicate runtime DLL basename: "+name;
        names.insert(name);
        if(bases.contains(name) || systemLibrary(name) || name.startsWith("vcruntime") || name.startsWith("msvcp") || name=="ucrtbase.dll"
           || name=="libsodium.dll") return "Bundle attempts to override a host/system library: "+name;
        if(role=="qt-plugin" || name.startsWith("qt6")) privateQt=true;
#ifndef Q_OS_WIN
        return "Private runtime deployment is not supported on this platform";
#endif
    }
    if((privateQt || !p.info["requiresHost"].toArray().isEmpty()) && host.isEmpty()) return "Host runtime contract is missing";
    if(privateQt && (runtime["qtBuildId"]!=host["qtBuildId"] || runtime["qtBuildId"].toString().isEmpty()
       || p.info["qtVersion"]!=host["qtVersion"])) return "Qt Kit build mismatch; use the matching SDK/Kit or restart with a matching host";
    if(!host.isEmpty()) for(const auto &field:{"platform","architecture","compilerAbi","debug"})
        if(p.info[field]!=host[field]) return "Host runtime contract mismatch: "+QString::fromLatin1(field);
    QSet<QString> required;
    for(const auto &value:p.info["requiresHost"].toArray()) {
        const auto name=value.toString().toLower();
        if(name.isEmpty() || !bases.contains(name) || required.contains(name)) return "Invalid required host library: "+name;
        required.insert(name);
#ifdef Q_OS_WIN
        const auto app=QDir(QCoreApplication::applicationDirPath()).filePath(name);
        // Already loaded base modules are allowed, including the Qt Test Kit in developer tests.
        if(!QFileInfo(app).isFile() && !GetModuleHandleW(reinterpret_cast<LPCWSTR>(name.utf16()))) return "Missing host runtime library: "+name;
#endif
    }
    return {};
}
struct RuntimeDirectory {
    std::unique_ptr<QTemporaryDir> directory;
    std::unique_ptr<QLockFile> lock;
    QString fingerprint;
#ifdef Q_OS_WIN
    DLL_DIRECTORY_COOKIE cookie=nullptr;
#endif
};
struct Module { QString hash,path; void *handle=nullptr; bool active=false; };
struct State { std::vector<RuntimeDirectory> directories; QMap<QString,Module> modules; bool policy=false; };
// Intentionally process-lifetime: Qt application/global statics may still call these DLLs.
static State &state() { static auto *value=new State; return *value; }
QJsonObject runtimeStatistics() {
    int handles=0; for(const auto &m:state().modules) if(m.handle) ++handles;
    return {{"directories",int(state().directories.size())},{"modules",int(state().modules.size())},{"handles",handles}};
}
QString activateRuntime(const PackageResult &p,const QString &businessDirectory) {
    auto error=validateRuntime(p); if(!error.isEmpty()) return error;
#ifdef Q_OS_WIN
    auto &s=state();
    if(!s.policy) { if(!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return "Cannot establish safe DLL search policy"; s.policy=true; }
    const auto bases=baseNames(hostRuntimeContract());
    QMap<QString,QJsonObject> files; QMap<QString,QStringList> imports;
    const bool legacy=p.info["formatVersion"].toInt()==1;
    // v1 has no file manifest. Ignore unknown v2-looking fields in legacy JSON.
    const auto runtimeFiles=legacy?QJsonArray{}:p.info["files"].toArray();
    for(const auto &value:runtimeFiles) {
        const auto item=value.toObject(); const auto role=item["role"].toString();
        if(role=="runtime-library" || role=="qt-plugin") files.insert(QFileInfo(item["path"].toString()).fileName().toLower(),item);
    }
    QStringList binaries;
    if(p.info["formatVersion"].toInt()==1) {
        binaries<<QDir(businessDirectory).filePath("plugin.dll");
    } else {
        binaries<<QDir(businessDirectory).filePath(p.info["entry"].toString());
        for(const auto &item:files) binaries<<QDir(businessDirectory).filePath(item["path"].toString());
    }
    QMap<QString,QString> legacyFiles; QSet<QString> checked;
    for(int binary=0;binary<binaries.size();++binary) {
        const auto path=binaries[binary]; if(checked.contains(path.toLower())) continue; checked.insert(path.toLower());
        auto names=windowsImports(path,&error); if(!error.isEmpty()) return error;
        imports.insert(QFileInfo(path).fileName().toLower(),names);
        for(const auto &name:names) {
            if(files.contains(name) || systemLibrary(name)) continue;
            if(bases.contains(name)) {
                if(!QFileInfo(QDir(QCoreApplication::applicationDirPath()).filePath(name)).isFile()
                   && !GetModuleHandleW(reinterpret_cast<LPCWSTR>(name.utf16()))) return "Missing host runtime library: "+name;
                continue;
            }
            if((name.startsWith("vcruntime") || name.startsWith("msvcp") || name=="libsodium.dll")
               && QFileInfo(QDir(QCoreApplication::applicationDirPath()).filePath(name)).isFile()) continue;
            const auto provided=QDir(QCoreApplication::applicationDirPath()).filePath(name);
            if(legacy && QFileInfo(provided).isFile() && !QFileInfo(provided).isSymLink() && !QFileInfo(provided).isJunction()) {
                legacyFiles.insert(name,provided); binaries.append(provided); continue;
            }
            return "Missing runtime dependency "+name+"; rebuild the plugin with the new SDK";
        }
    }
    if(legacy) {
        // No collection or guesses for v1: only already deployed host files qualify.
        // Pin these extra host libraries until exit, including application-lifetime Qt backends.
        QMap<QString,QString> hashes;
        for(auto it=legacyFiles.begin();it!=legacyFiles.end();++it) {
            const auto hash=digest(it.value()); if(hash.isEmpty()) return "Cannot verify legacy host dependency: "+it.key();
            hashes.insert(it.key(),hash);
            if(s.modules.contains(it.key()) && s.modules[it.key()].hash!=hash) return "Legacy runtime DLL conflict: "+it.key()+"; restart required";
            if(auto module=GetModuleHandleW(reinterpret_cast<LPCWSTR>(it.key().utf16()))) {
                wchar_t path[32768]; const auto length=GetModuleFileNameW(module,path,32768);
                if(!length || length>=32768 || digest(QString::fromWCharArray(path,length))!=hash) return "External legacy runtime conflict: "+it.key()+"; restart required";
            }
        }
        for(auto it=legacyFiles.begin();it!=legacyFiles.end();++it) {
            auto &module=s.modules[it.key()];
            if(module.active) continue;
            module.hash=hashes[it.key()]; module.path=it.value();
            const auto native=QDir::toNativeSeparators(module.path);
            module.handle=LoadLibraryExW(reinterpret_cast<LPCWSTR>(native.utf16()),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if(!module.handle) return "Legacy dependency preload failed: "+it.key()+"; rebuild with the new SDK or restart";
            module.active=true;
        }
        return {};
    }
    if(files.isEmpty()) return {};
    // Check all conflicts before adding paths or executing a single DLL.
    for(auto it=files.begin();it!=files.end();++it) {
        const auto name=it.key(),hash=it.value()["sha256"].toString();
        if(s.modules.contains(name) && s.modules[name].hash!=hash) return "Runtime DLL conflict: "+name+"; restart with a matching plugin build";
        if(auto loaded=GetModuleHandleW(reinterpret_cast<LPCWSTR>(name.utf16()))) {
            wchar_t path[32768]; const auto length=GetModuleFileNameW(loaded,path,32768);
            if(!length || length>=32768 || digest(QString::fromWCharArray(path,length))!=hash)
                return "External runtime DLL conflict: "+name+"; restart with a matching plugin build";
        }
    }
    // Topologically sort ordinary runtime libraries; Qt factory plugins remain uninstantiated.
    QStringList order; QSet<QString> done,visiting;
    std::function<bool(const QString &)> visit=[&](const QString &name) {
        if(done.contains(name)) return true; if(visiting.contains(name)) return false; visiting.insert(name);
        for(const auto &dep:imports[name]) if(files.contains(dep)) {
            if(files[dep]["role"]=="qt-plugin" || !visit(dep)) return false;
        }
        visiting.remove(name); done.insert(name); order.append(name); return true;
    };
    for(auto it=files.begin();it!=files.end();++it) if(it.value()["role"]=="runtime-library" && !visit(it.key())) return "Unsupported cyclic runtime dependency";
    QJsonArray signature;
    for(auto it=files.begin();it!=files.end();++it) {
        const auto item=it.value();
        signature.append(QJsonObject{{"path",item["path"].toString().normalized(QString::NormalizationForm_C).toCaseFolded()},
            {"role",item["role"]},{"size",item["size"]},{"sha256",item["sha256"]}});
    }
    const auto fingerprint=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonObject{
        {"files",signature},{"runtime",p.info["runtime"]}}).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    for(const auto &directory:s.directories) if(directory.fingerprint==fingerprint) {
        for(const auto &name:order) if(!s.modules[name].active) return "Previous runtime activation failed; restart required";
        return {};
    }
    RuntimeDirectory directory; error=createCache("runtime",directory.directory,directory.lock); if(!error.isEmpty()) return error;
    directory.fingerprint=fingerprint;
    for(const auto &item:files) {
        const auto relative=item["path"].toString(),source=QDir(businessDirectory).filePath(relative),destination=directory.directory->filePath(relative);
        if(!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::copy(source,destination) || digest(destination)!=item["sha256"].toString())
            return "Cannot prepare verified runtime cache";
        QFile::setPermissions(destination,QFile::ReadOwner|QFile::WriteOwner);
    }
    const auto bin=directory.directory->filePath("runtime/bin");
    if(QFileInfo(bin).isDir()) {
        const auto native=QDir::toNativeSeparators(bin);
        directory.cookie=AddDllDirectory(reinterpret_cast<LPCWSTR>(native.utf16()));
        if(!directory.cookie) return "Cannot register private DLL directory";
    }
    directory.directory->setAutoRemove(false);
    s.directories.push_back(std::move(directory)); auto &active=s.directories.back();
    for(auto it=files.begin();it!=files.end();++it) if(!s.modules.contains(it.key()))
        s.modules.insert(it.key(),Module{it.value()["sha256"].toString(),active.directory->filePath(it.value()["path"].toString()),nullptr,false});
    for(const auto &name:order) {
        auto &module=s.modules[name]; if(module.active) continue;
        const auto native=QDir::toNativeSeparators(module.path);
        module.handle=LoadLibraryExW(reinterpret_cast<LPCWSTR>(native.utf16()),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!module.handle) return QString("Runtime preload failed: %1 (Windows %2); restart required").arg(name).arg(GetLastError());
        module.active=true;
    }
    const auto qt=active.directory->filePath("runtime/qt-plugins");
    if(QFileInfo(qt).isDir()) QCoreApplication::addLibraryPath(qt);
    // Remove redundant copies from the unloadable business directory.
    for(const auto &item:files) QFile::remove(QDir(businessDirectory).filePath(item["path"].toString()));
#else
    Q_UNUSED(businessDirectory);
#endif
    return {};
}
}
