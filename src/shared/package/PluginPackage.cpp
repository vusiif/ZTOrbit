#include "PluginPackage.h"
#include "PluginDependencies.h"
#include <ztprotocol/PluginIdentity.h>
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSysInfo>
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>
#include <QElapsedTimer>
#include <QDateTime>
#include <QUuid>
#include <QtEndian>
#include <sodium.h>
#include <QCoreApplication>
#include <QPluginLoader>
#include <QTimer>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <signal.h>
#include <errno.h>
#ifdef Q_OS_MACOS
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#endif
#include <QJsonArray>
namespace vusiif::zto::package {
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
    if(!QFileInfo(path).isFile() || QFileInfo(path).isSymLink() || QFileInfo(path).suffix().compare("z2plugin",Qt::CaseInsensitive)) {
        result.error="Expected a regular .z2plugin file"; return result;
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
static QString legacyCacheRoot() {
    const auto identity=QString::number(qHash(QCoreApplication::applicationDirPath()),16);
    return QDir::temp().filePath("zto-plugin-cache-"+identity);
}
CacheContext CacheContext::temporary() {
    CacheContext context; context.root=legacyCacheRoot(); context.legacy=true; return context;
}
namespace {
QMutex sessionMutex;
QHash<QString,std::shared_ptr<QLockFile>> sessionLocks;

bool linkedPath(const QString &path) {
    const QFileInfo info(path);
    return info.isSymLink() || info.isJunction();
}

bool processIdentityReadable(qint64 pid, QString &identity) {
    if(pid<=0) return false;
#ifdef Q_OS_WIN
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(pid));
    if(!process) return false;
    FILETIME creation{},exit{},kernel{},user{};
    ULARGE_INTEGER value{};
    const bool ok=GetProcessTimes(process,&creation,&exit,&kernel,&user)!=0;
    if(ok) { value.LowPart=creation.dwLowDateTime; value.HighPart=creation.dwHighDateTime; }
    CloseHandle(process);
    if(!ok) return false;
    identity=QStringLiteral("win:")+QString::number(value.QuadPart);
    return true;
#elif defined(Q_OS_MACOS)
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PID,int(pid)};
    struct kinfo_proc info{};
    size_t size=sizeof info;
    if(sysctl(mib,4,&info,&size,nullptr,0)!=0 || size!=sizeof info) return false;
    identity=QStringLiteral("mac:%1:%2").arg(qint64(info.kp_proc.p_starttime.tv_sec)).arg(qint64(info.kp_proc.p_starttime.tv_usec));
    return true;
#elif defined(Q_OS_LINUX)
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if(!file.open(QIODevice::ReadOnly)) return false;
    const auto data=file.readAll();
    // The command name may contain spaces and parentheses; fields resume after
    // the last ')'. Field 22 (starttime) is index 19 of the remaining fields.
    const auto close=data.lastIndexOf(')');
    if(close<0) return false;
    const auto fields=data.mid(close+1).trimmed().split(' ');
    if(fields.size()<=19) return false;
    identity=QStringLiteral("linux:")+QString::fromLatin1(fields[19]);
    return true;
#else
    Q_UNUSED(identity);
    return false;
#endif
}

bool processAlive(qint64 pid, const QString &startIdentity = {}) {
    if(pid<=0) return false;
    if(!startIdentity.isEmpty()) {
        QString current;
        if(processIdentityReadable(pid,current)) return current==startIdentity;
#ifdef Q_OS_WIN
        // An inaccessible but existing process is unknown, not dead. Only a
        // confirmed missing process may lose its cache session.
        HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid));
        if(!process) return GetLastError()==ERROR_ACCESS_DENIED;
        const bool alive=WaitForSingleObject(process,0)==WAIT_TIMEOUT;
        CloseHandle(process);
        return alive;
#else
        return false;
#endif
    }
#ifdef Q_OS_WIN
    HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(pid));
    if(!process) return false;
    DWORD code=0;
    bool alive=GetExitCodeProcess(process,&code) && code==STILL_ACTIVE;
    if(alive) alive=WaitForSingleObject(process,0)==WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    return kill(pid_t(pid),0)==0 || errno==EPERM;
#endif
}

QString cacheIndexFile() {
    const auto base=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base.isEmpty()?QString():QDir(base).filePath("cache-index.json");
}

bool updateCacheIndex(const CacheContext &context, const QJsonObject *entry, QString &error) {
    const auto path=cacheIndexFile();
    if(path.isEmpty()) { error="Cannot locate the current user's cache index"; return false; }
    if(!QDir().mkpath(QFileInfo(path).absolutePath())) { error="Cannot create the cache index directory"; return false; }
    QLockFile lock(path+".lock"); lock.setStaleLockTime(30000);
    if(!lock.tryLock(3000)) { error="Cannot lock the cache index"; return false; }
    QJsonArray entries;
    {
        // Close the reader before QSaveFile replaces the file on Windows.
        QFile input(path);
        if(input.open(QIODevice::ReadOnly)) {
            const auto document=QJsonDocument::fromJson(input.readAll());
            if(document.isArray()) entries=document.array();
        }
    }
    QJsonArray kept;
    for(const auto &value:entries) {
        const auto object=value.toObject();
        if(object.value("runId").toString()==context.runId) continue;
        kept.append(object);
    }
    if(entry) kept.append(*entry);
    while(kept.size()>64) kept.removeFirst();
    QSaveFile output(path);
    const auto bytes=QJsonDocument(kept).toJson(QJsonDocument::Compact);
    if(!output.open(QIODevice::WriteOnly) || output.write(bytes)!=bytes.size() || !output.commit()) {
        error=output.errorString(); return false;
    }
    return true;
}

bool findCacheIndexEntry(const QString &userKey, const QString &runId, QJsonObject &entry) {
    if(userKey.isEmpty() || runId.isEmpty()) return false;
    QFile input(cacheIndexFile());
    if(!input.open(QIODevice::ReadOnly)) return false;
    const auto document=QJsonDocument::fromJson(input.readAll());
    if(!document.isArray()) return false;
    for(const auto &value:document.array()) {
        const auto object=value.toObject();
        if(object.value("userKey").toString()==userKey && object.value("runId").toString()==runId) {
            entry=object; return true;
        }
    }
    return false;
}

bool sessionStructure(const QString &root, const QString &userKey, const QString &runId) {
    if(root.isEmpty() || userKey.isEmpty() || runId.isEmpty()) return false;
    const QFileInfo info(root);
    if(!info.isAbsolute() || info.fileName()!=runId) return false;
    const QFileInfo userDirectory(info.absolutePath());
    if(userDirectory.fileName()!=userKey) return false;
    return QFileInfo(userDirectory.absolutePath()).fileName()=="plugin-packages";
}

QMutex workerMutex;

QString sessionRootForCache(const QString &cacheDirectory) {
    if(cacheDirectory.isEmpty()) return {};
    const QFileInfo directory(cacheDirectory);
    if(!directory.isDir()) return {};
    const QFileInfo parent(directory.absolutePath());
    if(!parent.isDir()) return {};
    if(!QFileInfo::exists(QDir(parent.absoluteFilePath()).filePath("owner.json"))) return {};
    return parent.absoluteFilePath();
}

QString workersFile(const QString &sessionRoot) {
    return QDir(sessionRoot).filePath("workers.json");
}

bool readWorkers(const QString &sessionRoot, QJsonArray &workers, QString &error) {
    workers={};
    QFile file(workersFile(sessionRoot));
    if(!file.exists()) {
        // The marker is created before the session becomes usable. A session
        // without it may predate worker tracking; its worker state is unknown,
        // so cleanup must not assume there is none.
        error=QStringLiteral("Cache worker record is missing"); return false;
    }
    if(!file.open(QIODevice::ReadOnly)) { error=QStringLiteral("Cannot read the cache worker record"); return false; }
    QJsonParseError parse;
    const auto document=QJsonDocument::fromJson(file.readAll(),&parse);
    if(parse.error!=QJsonParseError::NoError || !document.isArray()) {
        error=QStringLiteral("Cache worker record is invalid"); return false;
    }
    workers=document.array();
    return true;
}

bool writeWorkers(const QString &sessionRoot, const QJsonArray &workers, QString &error) {
    QSaveFile file(workersFile(sessionRoot));
    const auto bytes=QJsonDocument(workers).toJson(QJsonDocument::Compact);
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) {
        error=QStringLiteral("Cannot write the cache worker record"); return false;
    }
    return true;
}

bool ensureWorkersRecord(const QString &sessionRoot, QString &error) {
    QFile file(workersFile(sessionRoot));
    if(file.exists()) {
        QJsonArray workers;
        return readWorkers(sessionRoot,workers,error);
    }
    return writeWorkers(sessionRoot,{},error);
}

// A worker may outlive a crashed host (the Windows job object normally stops
// it, but other platforms and injection/handles can delay that). Any live or
// unresolvable worker blocks destructive cleanup of its session.
bool cacheWorkersStopped(const QString &sessionRoot, QString &error) {
    QJsonArray workers;
    if(!readWorkers(sessionRoot,workers,error)) return false;
    for(const auto &value:workers) {
        const auto object=value.toObject();
        const auto pid=qint64(object.value("pid").toDouble());
        const auto identity=object.value("processStart").toString();
        if(pid<=0 || identity.isEmpty()) { error=QStringLiteral("Cache worker record is incomplete"); return false; }
        if(processAlive(pid,identity)) { error=QStringLiteral("Cache session still has a running worker process"); return false; }
    }
    return true;
}

bool treeContainsLinks(const QString &root) {
    QDirIterator iterator(root,QDir::AllEntries|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
    while(iterator.hasNext()) {
        iterator.next();
        if(linkedPath(iterator.filePath())) return true;
    }
    return false;
}

QJsonObject readOwner(const QString &root, QString &error) {
    QFile file(QDir(root).filePath("owner.json"));
    if(!file.open(QIODevice::ReadOnly)) { error=file.errorString(); return {}; }
    QJsonParseError parse;
    const auto document=QJsonDocument::fromJson(file.readAll(),&parse);
    if(parse.error!=QJsonParseError::NoError || !document.isObject()) { error="Cache owner record is invalid"; return {}; }
    return document.object();
}

bool ownerMatches(const QJsonObject &owner, const QString &userKey, const QString &runId, const QString &token) {
    if(owner.value("format").toInt()!=1 || owner.value("userKey").toString()!=userKey
        || owner.value("runId").toString()!=runId || owner.value("token").toString().isEmpty()) return false;
    return token.isEmpty() || owner.value("token").toString()==token;
}

CacheContext contextFor(const QString &userKey, const QString &runId) {
    CacheContext context; context.userKey=userKey; context.runId=runId; return context;
}

bool removeSession(const QString &root, const QString &userKey, const QString &runId, const QString &token, QString &error) {
    if(!sessionStructure(root,userKey,runId)) { error="Cache session path is outside the managed layout"; return false; }
    if(linkedPath(root)) { error="Cache session root is a link"; return false; }
    if(!QFileInfo::exists(root)) return true;
    QString ownerError;
    const auto owner=readOwner(root,ownerError);
    if(!ownerError.isEmpty()) { error=ownerError; return false; }
    if(!ownerMatches(owner,userKey,runId,token)) { error="Cache owner record does not match the requested session"; return false; }
    if(processAlive(qint64(owner.value("pid").toDouble()),owner.value("processStart").toString())) {
        error="Cache session owner is still running"; return false;
    }
    if(!cacheWorkersStopped(root,error)) return false;
    // Unverifiable contents (links/junctions) could redirect a recursive delete
    // outside the managed session. Keep the index entry and skip instead.
    if(treeContainsLinks(root)) { error="Cache session contains a link and cannot be removed safely"; return false; }
    QLockFile lock(QDir(root).filePath("active.lock"));
    lock.setStaleLockTime(0);
    if(!lock.tryLock(0)) {
        qint64 pid=0; QString host,application;
        lock.getLockInfo(&pid,&host,&application);
        if(processAlive(pid)) { error="Cache session lock is held by a running process"; return false; }
        lock.removeStaleLockFile();
        if(!lock.tryLock(0)) { error="Cannot acquire the cache session lock"; return false; }
    }
    QDir directory(root);
    for(const auto &file:directory.entryInfoList(QDir::Files|QDir::NoSymLinks))
        QFile::setPermissions(file.absoluteFilePath(),QFile::ReadOwner|QFile::WriteOwner|QFile::ReadUser|QFile::WriteUser);
    QFile::setPermissions(root,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadUser|QFile::WriteUser|QFile::ExeUser);
    // Windows cannot remove a directory containing the open lock file.
    lock.unlock();
    if(!directory.removeRecursively()) { error="Cannot remove the cache session directory"; return false; }
    QString indexError;
    if(!updateCacheIndex(contextFor(userKey,runId),nullptr,indexError))
        qWarning().noquote()<<"[package] Cache index update failed:"<<indexError;
    return true;
}
} // namespace

QString registerCacheWorker(const QString &cacheDirectory, qint64 pid) {
    if(pid<=0) return "Invalid worker process identifier";
    const auto root=sessionRootForCache(cacheDirectory);
    // Legacy process-private caches are not indexed sessions and have no host.
    if(root.isEmpty()) return {};
    const auto identity=processStartIdentity(pid);
    if(identity.isEmpty()) return "Cannot determine the worker process identity";
    QMutexLocker locker(&workerMutex);
    QJsonArray workers;
    QString error;
    if(!readWorkers(root,workers,error)) return error;
    for(int index=workers.size()-1;index>=0;--index)
        if(qint64(workers[index].toObject().value("pid").toDouble())==pid) workers.removeAt(index);
    workers.append(QJsonObject{{"pid",double(pid)},{"processStart",identity},
        {"startedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
    return writeWorkers(root,workers,error)?QString():error;
}

void unregisterCacheWorker(const QString &cacheDirectory, qint64 pid) {
    if(pid<=0) return;
    const auto root=sessionRootForCache(cacheDirectory);
    if(root.isEmpty()) return;
    QMutexLocker locker(&workerMutex);
    QJsonArray workers;
    QString error;
    if(!readWorkers(root,workers,error)) { qWarning().noquote()<<"[package]"<<error; return; }
    bool changed=false;
    for(int index=workers.size()-1;index>=0;--index) {
        if(qint64(workers[index].toObject().value("pid").toDouble())==pid) { workers.removeAt(index); changed=true; }
    }
    if(changed && !writeWorkers(root,workers,error)) qWarning().noquote()<<"[package]"<<error;
}

QString processStartIdentity(qint64 pid) {
    QString identity;
    if(processIdentityReadable(pid,identity)) return identity;
    return {};
}

QString prepareCacheSession(CacheContext &context) {
    if(context.legacy || context.root.isEmpty()) return {};
    if(context.userKey.isEmpty() || context.runId.isEmpty()) return "Cache session identity is incomplete";
    if(!sessionStructure(context.root,context.userKey,context.runId)) return "Cache session path is outside the managed layout";
    if(linkedPath(context.root)) return "Cache session root is a link";
    if(!QDir().mkpath(context.root)) return "Cannot create the cache session directory";
    QFile::setPermissions(context.root,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadUser|QFile::WriteUser|QFile::ExeUser);
    QString error;
    QJsonObject owner;
    const auto ownerPath=QDir(context.root).filePath("owner.json");
    if(QFileInfo::exists(ownerPath)) {
        owner=readOwner(context.root,error);
        if(!error.isEmpty()) return "Existing cache owner record is invalid: "+error;
        if(!ownerMatches(owner,context.userKey,context.runId,{}))
            return "Existing cache owner record belongs to another session";
    } else {
        owner=QJsonObject{{"format",1},{"runId",context.runId},{"userKey",context.userKey},
            {"installKey",context.installKey},{"root",QDir::cleanPath(context.root)},
            {"pid",double(QCoreApplication::applicationPid())},
            {"processStart",processStartIdentity(QCoreApplication::applicationPid())},
            {"token",QUuid::createUuid().toString(QUuid::Id128)},
            {"startedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
        QSaveFile output(ownerPath);
        const auto bytes=QJsonDocument(owner).toJson(QJsonDocument::Indented);
        if(!output.open(QIODevice::WriteOnly) || output.write(bytes)!=bytes.size() || !output.commit())
            return "Cannot write the cache owner record";
    }
    context.token=owner.value("token").toString();
    if(context.token.isEmpty()) return "Cache owner record has no ownership token";
    QString workerRecordError;
    if(!ensureWorkersRecord(context.root,workerRecordError))
        return "Cannot initialize the cache worker record: "+workerRecordError;
    QMutexLocker locker(&sessionMutex);
    if(!sessionLocks.contains(context.root)) {
        auto lock=std::make_shared<QLockFile>(QDir(context.root).filePath("active.lock"));
        lock->setStaleLockTime(0);
        if(!lock->tryLock(1000)) return "Cannot lock the cache session";
        sessionLocks.insert(context.root,std::move(lock));
    }
    QJsonObject entry{{"runId",context.runId},{"root",QDir::cleanPath(context.root)},
        {"userKey",context.userKey},{"installKey",context.installKey},
        {"pid",double(QCoreApplication::applicationPid())},
        {"processStart",owner.value("processStart").toString()},
        {"token",context.token},
        {"startedAt",owner.value("startedAt").toString()}};
    QString indexError;
    // A session that is not discoverable through the user index must never be
    // used for extraction: cleanup could no longer validate its ownership.
    if(!updateCacheIndex(context,&entry,indexError))
        return "Cannot register the cache session in the user index: "+indexError;
    return {};
}

void cleanStaleCache(const CacheContext &context) {
    if(context.legacy || context.root.isEmpty() || context.userKey.isEmpty()) {
        cleanStaleCache();
        return;
    }
    QStringList roots;
    const auto indexPath=cacheIndexFile();
    {
        QFile index(indexPath);
        if(index.open(QIODevice::ReadOnly)) {
            const auto document=QJsonDocument::fromJson(index.readAll());
            for(const auto &value:document.array()) {
                const auto object=value.toObject();
                if(object.value("userKey").toString()==context.userKey)
                    roots.append(object.value("root").toString());
            }
        }
    }
    // The index may be missing or stale; scan this user's managed parent too.
    const QDir parent(QFileInfo(context.root).absolutePath());
    if(parent.exists() && !linkedPath(parent.absolutePath()))
        for(const auto &entry:parent.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDir::Name))
            roots.append(entry.absoluteFilePath());
    roots.removeDuplicates();
    for(const auto &root:roots) {
        const QFileInfo info(root);
        if(info.fileName()==context.runId) continue; // Never reclaim this process's session.
        if(!sessionStructure(root,context.userKey,info.fileName())) continue;
        QString error;
        if(removeSession(root,context.userKey,info.fileName(),{},error)) continue;
        // Unknown owner, live process or lock: conservatively keep it.
    }
}

void cleanStaleCache() {
    // Legacy process-private strategy for the archived server and packaging tools.
    QDir root(legacyCacheRoot());
    if(QFileInfo(root.absolutePath()).isSymLink() || QFileInfo(root.absolutePath()).isJunction()) return;
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

int runCacheCleanupHelper(const QStringList &arguments) {
    const auto value=[&arguments](const QString &name) {
        const int index=arguments.indexOf(name);
        if(index<0 || index+1>=arguments.size()) return QString();
        const auto result=arguments[index+1];
        return result.startsWith("--")?QString():result;
    };
    const auto runId=value("--cache-run");
    const auto userKey=value("--cache-user");
    const auto token=value("--cache-token");
    const bool waitForParent=arguments.contains("--cache-wait");
    const auto parentPid=value("--cache-pid").toLongLong();
    const auto parentIdentity=value("--cache-parent-start");
    if(waitForParent && parentPid>0) {
        QElapsedTimer timer; timer.start();
        while(timer.elapsed()<30000 && processAlive(parentPid,parentIdentity)) QThread::msleep(100);
    }
    // The root is never supplied as a raw path: the token must match the entry
    // this user's cache index registered for the same run.
    QJsonObject entry;
    if(token.isEmpty() || !findCacheIndexEntry(userKey,runId,entry)) return 2;
    if(entry.value("token").toString()!=token) return 2;
    const auto root=entry.value("root").toString();
    if(!sessionStructure(root,userKey,runId)) return 2;
    if(!QFileInfo::exists(root)) {
        QString indexError;
        updateCacheIndex(contextFor(userKey,runId),nullptr,indexError);
        return 0;
    }
    QString error;
    if(removeSession(root,userKey,runId,token,error)) return 0;
    return 1;
}

static void removeCache(const QString &path, int attempts) {
    const QFileInfo file(path);
    if(file.isSymLink() || file.isJunction()
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
static QString cacheRootFor(const CacheContext &context) {
    return context.legacy || context.root.isEmpty()?legacyCacheRoot():QDir::cleanPath(context.root);
}
static QString createCacheAt(const QString &kind, const QString &root) {
    if(kind!="load" && kind!="runtime") return "Invalid cache kind";
    if(root.isEmpty()) return "Cache context has no root";
    if(linkedPath(root) || !QDir().mkpath(root)) return "Cannot create private plugin cache";
    QFile::setPermissions(root,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadUser|QFile::WriteUser|QFile::ExeUser);
    return {};
}
QString extract(const PackageResult &result, const CacheContext &context, std::unique_ptr<QTemporaryDir> &directory,
                QString &library, std::unique_ptr<QLockFile> &lock) {
    const auto root=cacheRootFor(context);
    if(result.info["formatVersion"].toInt()>=2) {
        auto error=createCache("load",context,directory,lock); if(!error.isEmpty()) return error;
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
    bytes.resize(qsizetype(size));
    const auto rootError=createCacheAt("load",root);
    if(!rootError.isEmpty()) return rootError;
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
QString extract(const PackageResult &result, std::unique_ptr<QTemporaryDir> &directory, QString &library,
                std::unique_ptr<QLockFile> &lock) {
    return extract(result,CacheContext::temporary(),directory,library,lock);
}
QString createCache(const QString &kind, const CacheContext &context, std::unique_ptr<QTemporaryDir> &directory,
                    std::unique_ptr<QLockFile> &lock) {
    if(kind!="load" && kind!="runtime") return "Invalid cache kind";
    const auto root=cacheRootFor(context);
    const auto error=createCacheAt(kind,root);
    if(!error.isEmpty()) return error;
    directory=std::make_unique<QTemporaryDir>(QDir(root).filePath(kind+"-XXXXXX"));
    if(!directory->isValid()) return "Cannot create private plugin directory";
    lock=std::make_unique<QLockFile>(directory->filePath("active.lock")); lock->setStaleLockTime(0);
    if(!lock->tryLock()) return "Cannot lock plugin cache";
    return {};
}
QString createCache(const QString &kind,std::unique_ptr<QTemporaryDir> &directory,std::unique_ptr<QLockFile> &lock) {
    return createCache(kind,CacheContext::temporary(),directory,lock);
}
QString authenticate(const PackageResult &p) {
    if(!p.ok()) return p.error;
    if(p.info["formatVersion"].toInt()!=1) return {};
    std::unique_ptr<QTemporaryDir> directory; std::unique_ptr<QLockFile> lock; QString entry;
    const auto error=extract(p,directory,entry,lock);
    lock.reset(); discardCache(directory); return error;
}
}
