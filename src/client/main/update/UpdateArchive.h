#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace vusiif::zto::update {

struct ArchiveLimits {
    int maxEntries = 4096;
    qint64 maxEntryUncompressed = 512LL * 1024 * 1024;
    qint64 maxTotalUncompressed = 1024LL * 1024 * 1024;
    qint64 maxPackageBytes = 512LL * 1024 * 1024;
};

struct ArchiveEntry {
    QString path; // validated, '/' separated, no leading slash
    qint64 size = 0;
    quint32 crc = 0;
    bool directory = false;
    // Internal but kept for extraction: byte offsets in the package.
    quint32 method = 0;
    quint32 flags = 0;
    qint64 compressedSize = 0;
    qint64 localHeaderOffset = 0;
};

struct ExtractedFile {
    QString path;
    qint64 size = 0;
    QString sha256;
};

class UpdateArchive {
public:
    // Validates the central directory, every entry name, limits and the
    // declared local-header geometry. Does not extract anything.
    static bool validate(const QString &packagePath, const ArchiveLimits &limits,
                         QList<ArchiveEntry> &entries, QString &error);

    // Validates and extracts regular files into a fresh staging directory,
    // verifying every entry's CRC and computing its SHA-256 while streaming.
    // Directory entries are created but not listed in `files`.
    static bool extract(const QString &packagePath, const QString &stagingDirectory,
                        const ArchiveLimits &limits, QList<ExtractedFile> &files, QString &error);
};

} // namespace vusiif::zto::update
