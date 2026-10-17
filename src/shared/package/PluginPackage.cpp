#include "PluginPackage.h"
#include "PluginDependencies.h"
#include <ztprotocol/PluginIdentity.h>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QSysInfo>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QtEndian>
#include <sodium.h>
#include <QCoreApplication>
#include <QPluginLoader>
#include <QTimer>
#include <QJsonArray>
namespace vusiif::ztgui::package {
QString readBundle(const PackageResult &,const QString &,bool);
static constexpr qint64 MaxLibraryBytes=256*1024*1024;
// Public format key: packaging/obfuscation, deliberately not publisher authorization.
static const unsigned char Key[32]={0x7d,0x41,0xc2,0xe8,0xf1,0x07,0x38,0x25,0x96,0x9e,0x70,0x93,0x11,0x6a,0xbb,0x82,
                                   0x10,0xd9,0x22,0x31,0xac,0x48,0xf0,0xb7,0x58,0x19,0x6e,0xd4,0x02,0xa8,0x65,0xfa};
QJsonObject buildInfo() {
    QJsonObject result{{"platform",QSysInfo::productType()},{"architecture",QSysInfo::buildCpuArchitecture()},
                       {"qtVersion",QString::fromLatin1(qVersion())}};
#ifdef Q_OS_WIN
    result["platform"]="windows";
#elif defined(Q_OS_MACOS)
    result["platform"]="macos";
#else
    result["platform"]="linux";
#endif
#ifdef _MSC_VER
    result["compilerAbi"]=QString("msvc-%1").arg(_MSC_VER/100);
#elif defined(__clang__)
    result["compilerAbi"]=QString("clang-%1").arg(__clang_major__);
#else
    result["compilerAbi"]=QString("gcc-%1").arg(__GNUC__);
#endif
#ifdef QT_DEBUG
    result["debug"]=true;
#else
    result["debug"]=false;
#endif
    return result;
}
PackageResult inspect(const QString &path) {
    PackageResult result; result.path=QFileInfo(path).absoluteFilePath();
    QFile file(path);
    if(!QFileInfo(path).isFile() || QFileInfo(path).isSymLink() || QFileInfo(path).suffix().compare("ztplugin",Qt::CaseInsensitive)) {
        result.error="Expected a regular .ztplugin file"; return result;
    }
    if(!file.open(QIODevice::ReadOnly)) { result.error=file.errorString(); return result; }
    const auto magic=file.read(6);
    if(file.size()<50 || (magic!="ZTPK1\n" && magic!="ZTPK2\n" && magic!="ZTPK3\n")) { result.error="Invalid package header or size"; return result; }
    const bool v2=magic!="ZTPK1\n";
    const int format=magic=="ZTPK3\n"?3:v2?2:1;
    if(!v2 && file.size()>MaxLibraryBytes+65586) { result.error="Legacy package exceeds size limit"; return result; }
    const auto sizeBytes=file.read(4);
    if(sizeBytes.size()!=4) { result.error="Truncated package header"; return result; }
    const auto length=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(sizeBytes.constData()));
    if(sizeBytes.size()!=4 || length==0 || length>(v2?1024*1024:65536) || qint64(length)+50>file.size()) { result.error="Invalid metadata size"; return result; }
    result.header=file.read(length); QJsonParseError parse;
    const auto doc=QJsonDocument::fromJson(result.header,&parse);
    if(parse.error!=QJsonParseError::NoError || !doc.isObject()) { result.error="Invalid package JSON"; return result; }
    result.info=doc.object();
    if(result.info["formatVersion"].toInt()!=format) { result.error="Format version mismatch"; return result; }
    if(v2) { file.close(); result.error=readBundle(result,{},false); }
    // v1 is read only when extracting; inspect never buffers ciphertext.
    return result;
}
QString validate(const QJsonObject &info,const QString &host,const QString &api,const QString &iid) {
    if((info["formatVersion"].toInt()<1 || info["formatVersion"].toInt()>3) || info["host"].toString()!=host || !protocol::validPluginId(info["id"].toString())
        || info["name"].toString().trimmed().isEmpty() || info["version"].toString().trimmed().isEmpty()) return "Invalid plugin basic information";
    const auto semanticError=validateServiceManifest(info); if(!semanticError.isEmpty()) return semanticError;
    const bool worker=info["execution"].toObject()["kind"]=="worker-process";
    if(worker && host!="client") return "Worker services are supported by the client only";
    if(info["apiVersion"].toString()!=api || (!worker && info["iid"].toString()!=iid)) return "API version or interface IID mismatch";
    const auto own=buildInfo();
    for(const auto &field:{"platform","architecture"})
        if(info[field]!=own[field]) return QString("Incompatible plugin %1").arg(field);
    if(worker) return {};
    for(const auto &field:{"compilerAbi","debug"}) if(info[field]!=own[field]) return QString("Incompatible plugin %1").arg(field);
    const auto qt=info["qtVersion"].toString();
    if(!QRegularExpression("\\A[0-9]+\\.[0-9]+(?:\\.[0-9]+)?\\z").match(qt).hasMatch()
        || qt.section('.',0,1)!=own["qtVersion"].toString().section('.',0,1)) return "Incompatible plugin Qt version";
    return {};
}
QString write(const QString &library,const QString &output,QJsonObject info) {
    return writeBundle(QJsonObject{{"files",QJsonArray{QJsonObject{{"path","plugin/plugin."+QFileInfo(library).suffix()},{"role","entry"},{"source",library}}}}},output,info);
}
// Legacy writer retained only for compatibility fixtures, never used by SDK helpers.
QString writeLegacy(const QString &library,const QString &output,QJsonObject info) {
    if(sodium_init()<0) return "Crypto initialization failed";
    QFile input(library); if(!input.open(QIODevice::ReadOnly)) return input.errorString();
    if(input.size()<1 || input.size()>MaxLibraryBytes) return "Invalid plugin library size";
    auto bytes=input.readAll(); info["formatVersion"]=1;
    const auto build=buildInfo(); for(auto i=build.begin();i!=build.end();++i) info[i.key()]=i.value();
    // Read compile metadata without executing the plugin; SDK tools may be Release while a plugin is Debug.
    const auto compiled=QPluginLoader(library).metaData();
    if(compiled.isEmpty()) return "Input is not a Qt plugin library";
    info["debug"]=compiled["debug"].toBool();
    const int qtVersion=compiled["version"].toInt();
    // Qt plugin metadata encodes the major/minor ABI generation, not the patch.
    info["qtVersion"]=QString("%1.%2").arg(qtVersion>>16).arg((qtVersion>>8)&255);
    const auto header=QJsonDocument(info).toJson(QJsonDocument::Compact); if(header.size()>65536) return "Metadata too large";
    unsigned char nonce[24]; randombytes_buf(nonce,sizeof nonce);
    QByteArray encrypted(bytes.size()+16,Qt::Uninitialized); unsigned long long size=0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char *>(encrypted.data()),&size,
        reinterpret_cast<const unsigned char *>(bytes.constData()),bytes.size(),reinterpret_cast<const unsigned char *>(header.constData()),header.size(),nullptr,nonce,Key);
    sodium_memzero(bytes.data(),bytes.size()); encrypted.resize(qsizetype(size));
    QSaveFile file(output); if(!file.open(QIODevice::WriteOnly)) return file.errorString();
    QByteArray prefix("ZTPK1\n",6); prefix.resize(10); qToBigEndian(quint32(header.size()),reinterpret_cast<uchar *>(prefix.data()+6));
    if(file.write(prefix)!=prefix.size() || file.write(header)!=header.size() || file.write(reinterpret_cast<const char *>(nonce),24)!=24
        || file.write(encrypted)!=encrypted.size() || !file.commit()) return file.errorString();
    return {};
}
static QString cacheRoot() {
    const auto identity=QString::number(qHash(QCoreApplication::applicationDirPath()),16);
    return QDir::temp().filePath("ztgui-plugin-cache-"+identity);
}
static void removeCache(const QString &path, int attempts) {
    const QFileInfo file(path);
    if(file.isSymLink() || file.isJunction() || file.absolutePath()!=QDir(cacheRoot()).absolutePath()
        || (!file.fileName().startsWith("load-") && !file.fileName().startsWith("runtime-"))) return;
    if(!file.exists() || QDir(path).removeRecursively()) return;
    // Windows scanners can briefly retain a just-unloaded DLL. No plugin code
    // or handle survives in this cleanup closure; startup also recovers leftovers.
    if(attempts>0 && QCoreApplication::instance())
        QTimer::singleShot(100,QCoreApplication::instance(),[path,attempts] { removeCache(path,attempts-1); });
}
void discardCache(std::unique_ptr<QTemporaryDir> &directory) {
    if(!directory) return;
    const auto path=directory->path(); directory->setAutoRemove(false); directory.reset(); removeCache(path,50);
}
void cleanStaleCache() {
    // A process lock proves ownership/liveness; never remove another running host's files.
    QDir root(cacheRoot()); if(QFileInfo(root.absolutePath()).isSymLink() || QFileInfo(root.absolutePath()).isJunction()) return;
    for(const auto &entry:root.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks)) {
        if((!entry.fileName().startsWith("load-") && !entry.fileName().startsWith("runtime-")) || entry.isJunction()) continue;
        QLockFile lock(QDir(entry.absoluteFilePath()).filePath("active.lock"));
        lock.setStaleLockTime(0);
        if(lock.tryLock()) {
            lock.unlock();
            QDir owned(entry.absoluteFilePath());
            for(const auto &file:owned.entryInfoList(QDir::Files|QDir::NoSymLinks)) QFile::setPermissions(file.absoluteFilePath(),QFile::ReadOwner|QFile::WriteOwner|QFile::ReadUser|QFile::WriteUser);
            QFile::setPermissions(entry.absoluteFilePath(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadUser|QFile::WriteUser|QFile::ExeUser);
            owned.removeRecursively();
        }
    }
}
QString extract(const PackageResult &result,std::unique_ptr<QTemporaryDir> &directory,QString &library,std::unique_ptr<QLockFile> &lock) {
    if(result.info["formatVersion"].toInt()>=2) {
        auto error=createCache("load",directory,lock); if(!error.isEmpty()) return error;
        error=readBundle(result,directory->path(),false);
        if(!error.isEmpty()) { lock.reset(); discardCache(directory); return error; }
        library=directory->filePath(result.info["entry"].toString()); return {};
    }
    if(!result.ok() || sodium_init()<0) return "Invalid encrypted package";
    QFile input(result.path);
    if(!input.open(QIODevice::ReadOnly)) return input.errorString();
    if(input.size()<50 || input.size()>MaxLibraryBytes+65586 || input.read(6)!="ZTPK1\n") return "Legacy package changed";
    const auto prefix=input.read(4);
    if(prefix.size()!=4 || qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(prefix.constData()))!=result.header.size()
       || input.read(result.header.size())!=result.header) return "Legacy metadata changed";
    const auto encrypted=input.readAll();
    if(encrypted.size()<40) return "Invalid encrypted package";
    QByteArray bytes(encrypted.size()-40,Qt::Uninitialized); unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char *>(bytes.data()),&size,nullptr,
        reinterpret_cast<const unsigned char *>(encrypted.constData()+24),encrypted.size()-24,
        reinterpret_cast<const unsigned char *>(result.header.constData()),result.header.size(),reinterpret_cast<const unsigned char *>(encrypted.constData()),Key)!=0) return "Package authentication failed";
    bytes.resize(qsizetype(size)); const auto root=cacheRoot(); if(QFileInfo(root).isSymLink() || QFileInfo(root).isJunction() || !QDir().mkpath(root)) return "Cannot create private plugin cache";
    QFile::setPermissions(root,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadUser|QFile::WriteUser|QFile::ExeUser);
    directory=std::make_unique<QTemporaryDir>(QDir(root).filePath("load-XXXXXX")); if(!directory->isValid()) return "Cannot create temporary plugin directory";
    lock=std::make_unique<QLockFile>(directory->filePath("active.lock"));
    lock->setStaleLockTime(0);
    if(!lock->tryLock()) return "Cannot lock plugin cache";
    QFile::setPermissions(directory->path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadUser|QFile::WriteUser|QFile::ExeUser);
#ifdef Q_OS_WIN
    const QString extension="dll";
#elif defined(Q_OS_MACOS)
    const QString extension="dylib";
#else
    const QString extension="so";
#endif
    library=directory->filePath(QString("plugin.")+extension); QFile file(library);
    if(!file.open(QIODevice::WriteOnly)) return file.errorString(); const bool ok=file.write(bytes)==bytes.size();
    sodium_memzero(bytes.data(),bytes.size()); file.close(); QFile::setPermissions(library,QFile::ReadOwner|QFile::WriteOwner|QFile::ReadUser|QFile::WriteUser);
    if(!ok) return "Cannot extract plugin library";
    return {};
}
QString createCache(const QString &kind,std::unique_ptr<QTemporaryDir> &directory,std::unique_ptr<QLockFile> &lock) {
    if(kind!="load" && kind!="runtime") return "Invalid cache kind";
    const auto root=cacheRoot();
    if(QFileInfo(root).isSymLink() || QFileInfo(root).isJunction() || !QDir().mkpath(root)) return "Cannot create private plugin cache";
    QFile::setPermissions(root,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
    directory=std::make_unique<QTemporaryDir>(QDir(root).filePath(kind+"-XXXXXX"));
    if(!directory->isValid()) return "Cannot create private plugin directory";
    lock=std::make_unique<QLockFile>(directory->filePath("active.lock")); lock->setStaleLockTime(0);
    if(!lock->tryLock()) return "Cannot lock plugin cache";
    return {};
}
QString authenticate(const PackageResult &p) {
    if(!p.ok()) return p.error;
    if(p.info["formatVersion"].toInt()!=1) return {};
    std::unique_ptr<QTemporaryDir> directory; std::unique_ptr<QLockFile> lock; QString entry;
    const auto error=extract(p,directory,entry,lock);
    lock.reset(); discardCache(directory); return error;
}
}
