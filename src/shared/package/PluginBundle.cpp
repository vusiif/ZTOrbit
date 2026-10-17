#include "PluginPackage.h"
#include "PluginDependencies.h"
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPluginLoader>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <sodium.h>
#include <cmath>
namespace vusiif::ztgui::package {
static constexpr qint64 Chunk=1024*1024, MaxFile=256LL*1024*1024, MaxTotal=512LL*1024*1024;
static const unsigned char Key[32]={0x7d,0x41,0xc2,0xe8,0xf1,0x07,0x38,0x25,0x96,0x9e,0x70,0x93,0x11,0x6a,0xbb,0x82,
                                   0x10,0xd9,0x22,0x31,0xac,0x48,0xf0,0xb7,0x58,0x19,0x6e,0xd4,0x02,0xa8,0x65,0xfa};
QString safeRelativePath(const QString &path) {
    if(path.isEmpty() || path.contains('\\') || path.contains(QChar(0)) || path.contains(':') || QDir::isAbsolutePath(path)) return "Invalid relative bundle path";
    static const QRegularExpression device("\\A(?:CON|PRN|AUX|NUL|COM[0-9¹²³]|LPT[0-9¹²³])(?:\\.|$)",QRegularExpression::CaseInsensitiveOption);
    for(const auto &part:path.split('/'))
        if(part.isEmpty() || part=="." || part==".." || part.endsWith('.') || part.endsWith(' ')
           || part.contains(QRegularExpression("[\\x00-\\x1f<>\\\"|?*]")) || device.match(part).hasMatch()) return "Unsafe bundle path: "+path;
    return {};
}
static QString regularSource(const QString &path,bool file=true) {
    QFileInfo node(path); if(file && !node.isFile()) return "Not a regular input file: "+path;
    for(QFileInfo part(node.absoluteFilePath());;) {
        if(part.isSymLink() || part.isJunction()) return "Linked path: "+path;
        if(part.absolutePath()==part.absoluteFilePath()) break; part=QFileInfo(part.absolutePath());
    }
    return {};
}
QString validBundle(const QJsonObject &info) {
    const auto semanticError=validateServiceManifest(info); if(!semanticError.isEmpty()) return semanticError;
    if(!info["files"].isArray() || !info["runtime"].isObject() || !info["requiresHost"].isArray()) return "Missing v2 manifest fields";
    const auto files=info["files"].toArray(); if(files.isEmpty() || files.size()>512) return "Invalid bundle file count";
    QSet<QString> paths; int entries=0; qint64 total=0;
    static const QSet<QString> roles={"entry","runtime-library","qt-plugin","license","resource"};
    static const QRegularExpression hash("\\A[0-9a-f]{64}\\z");
    for(const auto &value:files) {
        if(!value.isObject()) return "Invalid file entry";
        const auto item=value.toObject(); const auto path=item["path"].toString(),role=item["role"].toString();
        auto error=safeRelativePath(path); if(!error.isEmpty()) return error;
        const auto normalized=path.normalized(QString::NormalizationForm_C).toCaseFolded();
        if(paths.contains(normalized)) return "Duplicate bundle path: "+path;
        for(const auto &previous:paths) if(normalized.startsWith(previous+'/') || previous.startsWith(normalized+'/')) return "Bundle file/directory collision";
        paths.insert(normalized); const double size=item["size"].toDouble(-1);
        if(!item["size"].isDouble() || size<0 || size>MaxFile || std::floor(size)!=size) return "Invalid bundle file size";
        total+=qint64(size); if(total>MaxTotal) return "Bundle exceeds total size limit";
        if(!hash.match(item["sha256"].toString()).hasMatch() || !roles.contains(role)) return "Invalid file hash or role";
        if(role=="entry") { ++entries; if(path!=info["entry"].toString() || size==0) return "Entry path mismatch"; }
        if(role=="runtime-library" && (!path.startsWith("runtime/bin/") || path.mid(12).contains('/'))) return "Runtime libraries must be in runtime/bin";
        if(role=="qt-plugin" && !path.startsWith("runtime/qt-plugins/")) return "Qt plugin must be below runtime/qt-plugins";
        if(role=="license" && !path.startsWith("licenses/")) return "License must be below licenses";
        const bool worker=info["execution"].toObject()["kind"]=="worker-process";
        const bool workerEntry=worker && role=="entry" && path.startsWith("runtime/bin/") && !path.mid(12).contains('/') && path.endsWith(".exe",Qt::CaseInsensitive);
        const bool workerConfig=worker && role=="resource" && path=="runtime/bin/qt.conf";
        if((role=="resource" || role=="entry") && (path.startsWith("runtime/") || path.startsWith("licenses/")) && !workerEntry && !workerConfig) return "Business file in reserved directory";
    }
    return entries==1 ? QString() : QString("Bundle needs exactly one entry");
}
static qint64 containerSize(const PackageResult &p) {
    qint64 size=10+p.header.size();
    for(const auto &value:p.info["files"].toArray()) {
        const qint64 bytes=qint64(value.toObject()["size"].toDouble()); size+=bytes+40*qMax(qint64(1),(bytes+Chunk-1)/Chunk);
    }
    return size;
}
static QByteArray aad(const QByteArray &header,quint32 file,quint32 block) {
    auto result=header; const auto offset=result.size(); result.resize(offset+8);
    qToBigEndian(file,reinterpret_cast<uchar *>(result.data()+offset)); qToBigEndian(block,reinterpret_cast<uchar *>(result.data()+offset+4)); return result;
}
// Every block is authenticated, including files not selected for license export.
QString readBundle(const PackageResult &p,const QString &destination,bool licensesOnly) {
    if(sodium_init()<0) return "Crypto initialization failed";
    auto error=validBundle(p.info); if(!error.isEmpty()) return error;
    QFile input(p.path); if(!input.open(QIODevice::ReadOnly)) return input.errorString();
    if(input.size()!=containerSize(p) || input.read(6)!=(p.info["formatVersion"].toInt()==3?"ZTPK3\n":"ZTPK2\n")) return "Package changed or invalid size";
    const auto length=input.read(4);
    if(length.size()!=4 || qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(length.constData()))!=p.header.size()
       || input.read(p.header.size())!=p.header) return "Package metadata changed";
    int index=0;
    for(const auto &value:p.info["files"].toArray()) {
        const auto item=value.toObject(); qint64 remaining=qint64(item["size"].toDouble()); quint32 block=0;
        QCryptographicHash hash(QCryptographicHash::Sha256); std::unique_ptr<QSaveFile> output;
        if(!destination.isEmpty() && (!licensesOnly || item["role"]=="license")) {
            const auto path=QDir(destination).filePath(item["path"].toString()); error=regularSource(path,false); if(!error.isEmpty()) return error;
            if(!QDir().mkpath(QFileInfo(path).absolutePath())) return "Cannot create bundle directories";
            output=std::make_unique<QSaveFile>(path); if(!output->open(QIODevice::WriteOnly)) return output->errorString();
            output->setPermissions(QFile::ReadOwner|QFile::WriteOwner);
        }
        do {
            const qint64 count=qMin(remaining,Chunk); const auto nonce=input.read(24),encrypted=input.read(count+16);
            if(nonce.size()!=24 || encrypted.size()!=count+16) return "Truncated package block";
            QByteArray bytes(count,Qt::Uninitialized); unsigned long long length=0; const auto auth=aad(p.header,index,block++);
            if(crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char *>(bytes.data()),&length,nullptr,
                reinterpret_cast<const unsigned char *>(encrypted.constData()),encrypted.size(),
                reinterpret_cast<const unsigned char *>(auth.constData()),auth.size(),
                reinterpret_cast<const unsigned char *>(nonce.constData()),Key)!=0 || qint64(length)!=count) return "Package authentication failed";
            hash.addData(bytes); if(output && output->write(bytes)!=count) return output->errorString();
            sodium_memzero(bytes.data(),bytes.size()); remaining-=count;
        } while(remaining>0);
        if(QString::fromLatin1(hash.result().toHex())!=item["sha256"].toString()) return "Bundle SHA-256 mismatch";
        if(output && !output->commit()) return output->errorString();
        ++index;
    }
    return input.atEnd() ? QString() : QString("Trailing package data");
}
QString writeBundle(const QJsonObject &manifest,const QString &output,QJsonObject info) {
    if(sodium_init()<0) return "Crypto initialization failed";
    const auto build=buildInfo(); for(auto i=build.begin();i!=build.end();++i) info[i.key()]=i.value();
    const auto sources=manifest["files"].toArray(); QJsonArray files; QString entrySource;
    if(sources.isEmpty() || sources.size()>512) return "Invalid bundle file count";
    for(const auto &value:sources) {
        auto item=value.toObject(); const auto source=item.take("source").toString(); auto error=regularSource(source); if(!error.isEmpty()) return error;
        QFile input(source); if(!input.open(QIODevice::ReadOnly)) return input.errorString(); if(input.size()>MaxFile) return "Input exceeds file size limit";
        QCryptographicHash hash(QCryptographicHash::Sha256); if(!hash.addData(&input)) return "Cannot hash input";
        item["size"]=input.size(); item["sha256"]=QString::fromLatin1(hash.result().toHex());
        if(item["role"]=="entry") { entrySource=source; info["entry"]=item["path"]; }
        files.append(item);
    }
    const bool v3=manifest.contains("execution");
    info["formatVersion"]=v3?3:2; info["files"]=files; info["runtime"]=manifest["runtime"].toObject(); info["requiresHost"]=manifest["requiresHost"].toArray();
    if(v3) {
        auto execution=manifest["execution"].toObject(); execution["entry"]=info["entry"];
        info["execution"]=execution; info["dependencies"]=manifest["dependencies"]; info["provides"]=manifest["provides"];
    }
    auto error=validBundle(info); if(!error.isEmpty()) return error;
    if(info.value("execution").toObject()["kind"]!="worker-process") {
    const auto compiled=QPluginLoader(entrySource).metaData(); if(compiled.isEmpty()) return "Input is not a Qt plugin library";
    if(v3) for(const auto &key:{"dependencies","provides"}) if(compiled["MetaData"].toObject()[key]!=info[key]) return "Inner plugin service metadata mismatch";
    info["debug"]=compiled["debug"].toBool(); const int qt=compiled["version"].toInt();
    if((qt>>16)!=QString::fromLatin1(qVersion()).section('.',0,0).toInt()
       || ((qt>>8)&255)!=QString::fromLatin1(qVersion()).section('.',1,1).toInt()) return "Plugin and packaging Kit Qt versions differ";
    }
    const auto header=QJsonDocument(info).toJson(QJsonDocument::Compact); if(header.size()>Chunk) return "Metadata too large";
    QSaveFile out(output); if(!out.open(QIODevice::WriteOnly)) return out.errorString();
    QByteArray prefix(v3?"ZTPK3\n":"ZTPK2\n",6); prefix.resize(10); qToBigEndian(quint32(header.size()),reinterpret_cast<uchar *>(prefix.data()+6));
    if(out.write(prefix)!=10 || out.write(header)!=header.size()) return out.errorString();
    for(int index=0;index<sources.size();++index) {
        const auto source=sources[index].toObject()["source"].toString(); error=regularSource(source); if(!error.isEmpty()) return error;
        QFile input(source); if(!input.open(QIODevice::ReadOnly)) return input.errorString();
        qint64 remaining=qint64(files[index].toObject()["size"].toDouble()); quint32 block=0;
        if(input.size()!=remaining) return "Input changed while packaging";
        QCryptographicHash hash(QCryptographicHash::Sha256);
        do {
            const auto count=qMin(remaining,Chunk); auto bytes=input.read(count); if(bytes.size()!=count) return "Input changed while packaging";
            hash.addData(bytes); const auto auth=aad(header,index,block++); unsigned char nonce[24]; randombytes_buf(nonce,24);
            QByteArray encrypted(count+16,Qt::Uninitialized); unsigned long long length=0;
            crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char *>(encrypted.data()),&length,
                reinterpret_cast<const unsigned char *>(bytes.constData()),bytes.size(),
                reinterpret_cast<const unsigned char *>(auth.constData()),auth.size(),nullptr,nonce,Key);
            sodium_memzero(bytes.data(),bytes.size());
            if(out.write(reinterpret_cast<const char *>(nonce),24)!=24 || out.write(encrypted)!=encrypted.size()) return out.errorString();
            remaining-=count;
        } while(remaining>0);
        if(!input.atEnd() || QString::fromLatin1(hash.result().toHex())!=files[index].toObject()["sha256"].toString()) return "Input changed while packaging";
    }
    return out.commit() ? QString() : out.errorString();
}
QString extractLicenses(const PackageResult &p,const QString &destination) {
    if(!p.ok()) return p.error; if(p.info["formatVersion"].toInt()<2) return "Legacy packages have no embedded licenses";
    QTemporaryDir staging; if(!staging.isValid()) return "Cannot create license staging";
    auto error=readBundle(p,staging.path(),true); if(!error.isEmpty()) return error;
    for(const auto &value:p.info["files"].toArray()) {
        const auto item=value.toObject(); if(item["role"]!="license") continue;
        const auto relative=item["path"].toString(),path=QDir(destination).filePath(relative);
        error=regularSource(path,false); if(!error.isEmpty()) return error;
        if(!QDir().mkpath(QFileInfo(path).absolutePath())) return "Cannot create license directory";
        QFile input(staging.filePath(relative)); QSaveFile output(path);
        if(!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return "Cannot export license";
        while(!input.atEnd()) { const auto bytes=input.read(Chunk); if(bytes.isEmpty() || output.write(bytes)!=bytes.size()) return "Cannot export license"; }
        if(!output.commit()) return output.errorString();
    }
    return {};
}
}
