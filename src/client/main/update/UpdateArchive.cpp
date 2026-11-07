#include "UpdateArchive.h"

#include "UpdateDeflate.h"
#include "UpdaterPlan.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>
#include <QByteArrayView>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace vusiif::zto::update {
namespace {

constexpr quint32 EndOfCentralDirectorySignature = 0x06054b50;
constexpr quint32 CentralDirectorySignature = 0x02014b50;
constexpr quint32 LocalFileHeaderSignature = 0x04034b50;
constexpr qint64 MaxCentralDirectoryBytes = 64LL * 1024 * 1024;
constexpr int MaxNameLength = 240;

quint16 read16(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(bytes.constData()) + offset);
}

quint32 read32(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()) + offset);
}

QString decodeName(const QByteArray &bytes, bool utf8Flag, QString &error)
{
    if (bytes.isEmpty()) {
        error = QStringLiteral("ZIP 条目名称为空");
        return {};
    }
    // Archive names are required to be UTF-8. Guessing a legacy code page for a
    // security-sensitive path would make validation ambiguous.
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString name = decoder.decode(bytes);
    if (decoder.hasError()) {
        error = QStringLiteral("ZIP 条目名称不是有效的 UTF-8");
        return {};
    }
    Q_UNUSED(utf8Flag)
    if (name.contains(QChar(0))) {
        error = QStringLiteral("ZIP 条目名称包含 NUL");
        return {};
    }
    return name;
}

quint32 crcTableEntry(quint32 index)
{
    quint32 value = index;
    for (int bit = 0; bit < 8; ++bit)
        value = (value & 1) ? (0xedb88320u ^ (value >> 1)) : (value >> 1);
    return value;
}

const std::array<quint32, 256> &crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> result{};
        for (quint32 index = 0; index < 256; ++index) result[index] = crcTableEntry(index);
        return result;
    }();
    return table;
}

quint32 crcUpdate(quint32 crc, const unsigned char *data, qint64 size)
{
    const auto &table = crcTable();
    for (qint64 index = 0; index < size; ++index)
        crc = table[(crc ^ data[index]) & 0xff] ^ (crc >> 8);
    return crc;
}

class FileSliceSource final : public InflateSource {
public:
    FileSliceSource(QFile &file, qint64 length) : m_file(file), m_remaining(length) {}
    qint64 read(unsigned char *destination, qint64 maximum) override
    {
        if (m_remaining <= 0) return 0;
        const qint64 count = std::min(maximum, m_remaining);
        const qint64 read = m_file.read(reinterpret_cast<char *>(destination), count);
        if (read <= 0) return -1;
        m_remaining -= read;
        return read;
    }
private:
    QFile &m_file;
    qint64 m_remaining;
};

class StagingSink final : public InflateSink {
public:
    StagingSink(QSaveFile &file) : m_file(file) { m_hash.reset(new QCryptographicHash(QCryptographicHash::Sha256)); }
    bool write(const unsigned char *data, qint64 size) override
    {
        if (size <= 0) return true;
        if (m_file.write(reinterpret_cast<const char *>(data), size) != size) return false;
        m_hash->addData(QByteArrayView(reinterpret_cast<const char *>(data), size));
        m_crc = crcUpdate(m_crc, data, size);
        m_written += size;
        return true;
    }
    quint32 crc() const { return m_crc ^ 0xffffffffu; }
    qint64 written() const { return m_written; }
    QByteArray digest() const { return m_hash->result(); }
private:
    QSaveFile &m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    quint32 m_crc = 0xffffffffu;
    qint64 m_written = 0;
};

bool withinDirectory(const QString &directory, const QString &path)
{
    // QDir/QFileInfo use '/' separators on Windows as well; normalizing avoids
    // a false "escaped staging" result when native paths use backslashes.
    const auto canonicalDirectory =
        QDir::fromNativeSeparators(QDir::cleanPath(QDir(directory).absolutePath()));
    const auto canonicalPath =
        QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
#ifdef Q_OS_WIN
    return canonicalPath.startsWith(canonicalDirectory + QLatin1Char('/'), Qt::CaseInsensitive)
        || canonicalPath.compare(canonicalDirectory, Qt::CaseInsensitive) == 0;
#else
    return canonicalPath.startsWith(canonicalDirectory + QLatin1Char('/'))
        || canonicalPath == canonicalDirectory;
#endif
}

struct ParsedEntry {
    ArchiveEntry entry;
    QByteArray rawName;
};

bool parseCentralDirectory(QFile &file, qint64 fileSize, const ArchiveLimits &limits,
                           QList<ParsedEntry> &entries, qint64 centralDirectoryOffset,
                           qint64 centralDirectorySize, QString &error);

} // namespace

bool UpdateArchive::validate(const QString &packagePath, const ArchiveLimits &limits,
                             QList<ArchiveEntry> &entries, QString &error)
{
    QList<ParsedEntry> parsed;
    QFile file(packagePath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("无法打开更新包：%1").arg(file.errorString());
        return false;
    }
    const qint64 fileSize = file.size();
    if (fileSize <= 0) {
        error = QStringLiteral("更新包为空");
        return false;
    }
    if (fileSize > limits.maxPackageBytes) {
        error = QStringLiteral("更新包超过允许的大小");
        return false;
    }
    const qint64 tailLength = std::min<qint64>(fileSize, 65557);
    if (!file.seek(fileSize - tailLength)) {
        error = QStringLiteral("无法读取更新包尾部");
        return false;
    }
    const QByteArray tail = file.read(tailLength);
    int eocdIndex = -1;
    for (int index = static_cast<int>(tail.size()) - 22; index >= 0; --index) {
        if (read32(tail, index) != EndOfCentralDirectorySignature) continue;
        const qint64 absolute = fileSize - tailLength + index;
        const quint16 comment = read16(tail, index + 20);
        if (static_cast<qint64>(comment) == fileSize - (absolute + 22)) {
            eocdIndex = index;
            break;
        }
    }
    if (eocdIndex < 0) {
        error = QStringLiteral("更新包不是有效的 ZIP 文件（缺少目录尾）");
        return false;
    }
    const quint16 diskNumber = read16(tail, eocdIndex + 4);
    const quint16 centralDisk = read16(tail, eocdIndex + 6);
    const quint16 entriesOnDisk = read16(tail, eocdIndex + 8);
    const quint16 entryCount = read16(tail, eocdIndex + 10);
    const quint32 centralSize = read32(tail, eocdIndex + 12);
    const quint32 centralOffset = read32(tail, eocdIndex + 16);
    const quint16 commentLength = read16(tail, eocdIndex + 20);
    if (diskNumber != 0 || centralDisk != 0 || entriesOnDisk != entryCount) {
        error = QStringLiteral("不支持分卷 ZIP 更新包");
        return false;
    }
    if (entryCount == 0xffff || centralSize == 0xffffffffu || centralOffset == 0xffffffffu) {
        error = QStringLiteral("不支持 ZIP64 更新包");
        return false;
    }
    const qint64 eocdAbsoluteOffset = fileSize - tailLength + eocdIndex;
    if (static_cast<qint64>(commentLength) != fileSize - (eocdAbsoluteOffset + 22)) {
        error = QStringLiteral("ZIP 目录尾注释长度无效");
        return false;
    }
    if (entryCount > static_cast<quint16>(limits.maxEntries)) {
        error = QStringLiteral("更新包条目数量超过限制");
        return false;
    }
    if (static_cast<qint64>(centralSize) > MaxCentralDirectoryBytes) {
        error = QStringLiteral("更新包中央目录过大");
        return false;
    }
    if (static_cast<qint64>(centralOffset) + static_cast<qint64>(centralSize) > eocdAbsoluteOffset) {
        error = QStringLiteral("ZIP 中央目录越界");
        return false;
    }
    if (!parseCentralDirectory(file, fileSize, limits, parsed, centralOffset, centralSize, error))
        return false;
    entries.clear();
    for (const auto &item : parsed) entries.append(item.entry);
    return true;
}

namespace {

bool parseCentralDirectory(QFile &file, qint64 fileSize, const ArchiveLimits &limits,
                           QList<ParsedEntry> &entries, qint64 centralDirectoryOffset,
                           qint64 centralDirectorySize, QString &error)
{
    if (!file.seek(centralDirectoryOffset)) {
        error = QStringLiteral("无法读取 ZIP 中央目录");
        return false;
    }
    const QByteArray directory = file.read(centralDirectorySize);
    if (directory.size() != centralDirectorySize) {
        error = QStringLiteral("ZIP 中央目录读取不完整");
        return false;
    }
    qint64 offset = 0;
    qint64 totalUncompressed = 0;
    QStringList seenNames;
    QSet<QString> filePaths;
    while (offset < directory.size()) {
        if (offset + 46 > directory.size()) {
            error = QStringLiteral("ZIP 中央目录条目被截断");
            return false;
        }
        if (read32(directory, static_cast<int>(offset)) != CentralDirectorySignature) {
            error = QStringLiteral("ZIP 中央目录签名无效");
            return false;
        }
        const int base = static_cast<int>(offset);
        const quint16 versionMadeBy = read16(directory, base + 4);
        const quint16 flags = read16(directory, base + 8);
        const quint16 method = read16(directory, base + 10);
        const quint32 crc = read32(directory, base + 16);
        const quint32 compressedSize = read32(directory, base + 20);
        const quint32 uncompressedSize = read32(directory, base + 24);
        const quint16 nameLength = read16(directory, base + 28);
        const quint16 extraLength = read16(directory, base + 30);
        const quint16 commentLength = read16(directory, base + 32);
        const quint16 diskStart = read16(directory, base + 34);
        const quint32 externalAttributes = read32(directory, base + 38);
        const quint32 localOffset = read32(directory, base + 42);
        const qint64 entryLength = 46LL + nameLength + extraLength + commentLength;
        if (offset + entryLength > directory.size() || nameLength == 0) {
            error = QStringLiteral("ZIP 中央目录条目长度无效");
            return false;
        }
        if (compressedSize == 0xffffffffu || uncompressedSize == 0xffffffffu || localOffset == 0xffffffffu) {
            error = QStringLiteral("不支持 ZIP64 条目");
            return false;
        }
        if (diskStart != 0) {
            error = QStringLiteral("不支持分卷 ZIP 条目");
            return false;
        }
        if (flags & 0x0001) {
            error = QStringLiteral("更新包包含加密条目");
            return false;
        }
        if (flags & (0x0020 | 0x0040 | 0x2000)) {
            error = QStringLiteral("更新包包含不支持的 ZIP 条目标志");
            return false;
        }
        if (method != 0 && method != 8) {
            error = QStringLiteral("更新包使用不支持的压缩算法");
            return false;
        }
        const QByteArray rawName = directory.mid(base + 46, nameLength);
        const QString name = decodeName(rawName, (flags & 0x0800) != 0, error);
        if (name.isEmpty() && !error.isEmpty()) return false;

        const auto parts = name.split(QLatin1Char('/'), Qt::KeepEmptyParts);
        const bool directoryEntry = name.endsWith(QLatin1Char('/'));
        QString relative = name;
        if (directoryEntry) relative.chop(1);
        if (relative.isEmpty()) {
            error = QStringLiteral("ZIP 条目名称为空");
            return false;
        }
        std::string pathError;
        if (updater::normalizeRelativePath(relative.toStdString(), pathError).empty()
            || !updater::isValidRelativePath(relative.toStdString(), pathError)) {
            error = QStringLiteral("更新包包含不安全的路径：%1").arg(QString::fromStdString(pathError));
            return false;
        }
        if (relative.size() > MaxNameLength) {
            error = QStringLiteral("更新包条目路径过长");
            return false;
        }
        // Unix symlink permission bits and the Windows reparse attribute are
        // both rejected: the package may only contain regular files/directories.
        if ((versionMadeBy >> 8) == 3 && ((externalAttributes >> 16) & 0xf000) == 0xa000) {
            error = QStringLiteral("更新包包含符号链接条目");
            return false;
        }
        if (externalAttributes & 0x00000400u) {
            error = QStringLiteral("更新包包含重解析点条目");
            return false;
        }
        const bool attributeDirectory = (externalAttributes & 0x00000010u) != 0;
        if (attributeDirectory && !directoryEntry) {
            // A directory attribute on a file-shaped name would be extracted as
            // a file by this reader, so reject the ambiguity.
            error = QStringLiteral("ZIP 条目类型与名称不一致");
            return false;
        }
        if (directoryEntry) {
            if (compressedSize != 0 || uncompressedSize != 0 || method != 0) {
                error = QStringLiteral("ZIP 目录条目带有数据");
                return false;
            }
        } else {
            if (uncompressedSize > static_cast<quint64>(limits.maxEntryUncompressed)) {
                error = QStringLiteral("更新包单个文件超过限制");
                return false;
            }
            if (static_cast<qint64>(uncompressedSize) > 0
                && static_cast<qint64>(uncompressedSize)
                       > static_cast<qint64>(compressedSize) * 1000 + 1024 * 1024) {
                error = QStringLiteral("更新包条目压缩比异常");
                return false;
            }
            if (method == 0 && compressedSize != uncompressedSize) {
                error = QStringLiteral("ZIP 存储条目大小不一致");
                return false;
            }
        }
        if (static_cast<qint64>(localOffset) + 30 > centralDirectoryOffset) {
            error = QStringLiteral("ZIP 本地文件头越界");
            return false;
        }
        const QString lower = relative.toLower();
        if (seenNames.contains(lower)) {
            error = QStringLiteral("更新包包含重复条目：%1").arg(relative);
            return false;
        }
        seenNames.append(lower);
        totalUncompressed += uncompressedSize;
        if (totalUncompressed > limits.maxTotalUncompressed) {
            error = QStringLiteral("更新包解压总大小超过限制");
            return false;
        }
        if (!directoryEntry) {
            for (const auto &prefix : filePaths) {
                if (lower.startsWith(prefix + QLatin1Char('/'))) {
                    error = QStringLiteral("更新包条目与文件路径冲突：%1").arg(relative);
                    return false;
                }
            }
            for (const auto &existing : seenNames) {
                if (existing.startsWith(lower + QLatin1Char('/'))) {
                    error = QStringLiteral("更新包条目与目录路径冲突：%1").arg(relative);
                    return false;
                }
            }
            filePaths.insert(lower);
        }
        ParsedEntry parsed;
        parsed.entry.path = relative;
        parsed.entry.size = static_cast<qint64>(uncompressedSize);
        parsed.entry.crc = crc;
        parsed.entry.directory = directoryEntry;
        parsed.entry.method = method;
        parsed.entry.flags = flags;
        parsed.entry.compressedSize = static_cast<qint64>(compressedSize);
        parsed.entry.localHeaderOffset = static_cast<qint64>(localOffset);
        parsed.rawName = rawName;
        entries.append(parsed);
        offset += entryLength;
    }
    if (offset != directory.size()) {
        error = QStringLiteral("ZIP 中央目录长度不一致");
        return false;
    }
    Q_UNUSED(fileSize)
    return true;
}

} // namespace

bool UpdateArchive::extract(const QString &packagePath, const QString &stagingDirectory,
                            const ArchiveLimits &limits, QList<ExtractedFile> &files, QString &error)
{
    QList<ArchiveEntry> entries;
    if (!validate(packagePath, limits, entries, error)) return false;

    const QDir staging(stagingDirectory);
    if (!staging.exists()) {
        if (!QDir().mkpath(stagingDirectory)) {
            error = QStringLiteral("无法创建暂存目录");
            return false;
        }
    }
    if (!QDir(stagingDirectory).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty()) {
        error = QStringLiteral("暂存目录不是空的");
        return false;
    }
    QFile file(packagePath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("无法打开更新包：%1").arg(file.errorString());
        return false;
    }
    const qint64 fileSize = file.size();
    files.clear();

    // Local headers are re-validated against the central directory below.
    for (const auto &entry : entries) {
        if (entry.directory) continue;
        const QString target = staging.filePath(entry.path);
        if (!withinDirectory(stagingDirectory, target)) {
            error = QStringLiteral("更新包条目越出暂存目录：%1").arg(entry.path);
            return false;
        }
        if (!file.seek(entry.localHeaderOffset)) {
            error = QStringLiteral("无法读取 ZIP 本地文件头");
            return false;
        }
        const QByteArray header = file.read(30);
        if (header.size() != 30 || read32(header, 0) != LocalFileHeaderSignature) {
            error = QStringLiteral("ZIP 本地文件头无效：%1").arg(entry.path);
            return false;
        }
        const quint16 localFlags = read16(header, 6);
        const quint16 localMethod = read16(header, 8);
        const quint16 nameLength = read16(header, 26);
        const quint16 extraLength = read16(header, 28);
        if (localMethod != entry.method || (localFlags & 0x0001)) {
            error = QStringLiteral("ZIP 本地文件头与目录不一致：%1").arg(entry.path);
            return false;
        }
        const QByteArray localName = file.read(nameLength);
        if (localName.size() != nameLength) {
            error = QStringLiteral("ZIP 本地文件头名称被截断");
            return false;
        }
        QString localError;
        const QString decodedLocal = decodeName(localName, (localFlags & 0x0800) != 0, localError);
        if (decodedLocal != entry.path + (entry.directory ? QStringLiteral("/") : QString())) {
            // Directory entries are skipped, so only file entries reach here.
            error = QStringLiteral("ZIP 本地文件头名称与目录不一致：%1").arg(entry.path);
            return false;
        }
        if (entry.localHeaderOffset + 30LL + nameLength + extraLength + entry.compressedSize
            > fileSize) {
            error = QStringLiteral("ZIP 条目数据越界：%1").arg(entry.path);
            return false;
        }
        if (!file.seek(entry.localHeaderOffset + 30LL + nameLength + extraLength)) {
            error = QStringLiteral("无法定位 ZIP 条目数据");
            return false;
        }

        const QFileInfo targetInfo(target);
        if (!QDir().mkpath(targetInfo.absolutePath())) {
            error = QStringLiteral("无法创建条目目录：%1").arg(entry.path);
            return false;
        }
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly)) {
            error = QStringLiteral("无法写入暂存文件：%1").arg(entry.path);
            return false;
        }
        StagingSink sink(output);
        if (entry.method == 8) {
            FileSliceSource source(file, entry.compressedSize);
            const auto result = inflateRaw(source, sink, limits.maxEntryUncompressed, entry.size);
            if (!result.ok) {
                error = QStringLiteral("解压失败（%1）：%2").arg(entry.path, result.error);
                output.cancelWriting();
                return false;
            }
        } else {
            qint64 remaining = entry.compressedSize;
            QByteArray buffer;
            buffer.resize(64 * 1024);
            while (remaining > 0) {
                const qint64 read = file.read(buffer.data(), std::min<qint64>(remaining, static_cast<qint64>(buffer.size())));
                if (read <= 0) {
                    error = QStringLiteral("读取更新包条目失败：%1").arg(entry.path);
                    output.cancelWriting();
                    return false;
                }
                if (!sink.write(reinterpret_cast<const unsigned char *>(buffer.constData()), read)) {
                    error = QStringLiteral("写入暂存文件失败：%1").arg(entry.path);
                    output.cancelWriting();
                    return false;
                }
                remaining -= read;
            }
        }
        if (sink.written() != entry.size) {
            error = QStringLiteral("条目解压大小与声明不一致：%1").arg(entry.path);
            output.cancelWriting();
            return false;
        }
        if (sink.crc() != entry.crc) {
            error = QStringLiteral("条目 CRC 校验失败：%1").arg(entry.path);
            output.cancelWriting();
            return false;
        }
        if (!output.commit()) {
            error = QStringLiteral("无法提交暂存文件：%1").arg(entry.path);
            output.cancelWriting();
            return false;
        }
        ExtractedFile extracted;
        extracted.path = entry.path;
        extracted.size = entry.size;
        extracted.sha256 = QString::fromLatin1(sink.digest().toHex());
        files.append(extracted);
    }
    if (files.isEmpty()) {
        error = QStringLiteral("更新包不包含任何文件");
        return false;
    }
    return true;
}

} // namespace vusiif::zto::update
