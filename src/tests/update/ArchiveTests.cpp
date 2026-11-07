#include "TestSupport.h"
#include "UpdateArchive.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using namespace vusiif::zto::update;

namespace {
const QString fixturesDirectory = QStringLiteral(UPDATE_FIXTURES_DIR);

QByteArray fixture(const QString &relative)
{
    return zto_update_test::readFile(QDir(fixturesDirectory).filePath(relative));
}

QByteArray singleFileZip(const QByteArray &name, const QByteArray &data)
{
    zto_update_test::ZipEntrySpec entry;
    entry.name = name;
    entry.data = data;
    return zto_update_test::buildZip({entry});
}

QByteArray patchLocalName(const QByteArray &archive, const QByteArray &from, const QByteArray &to)
{
    QByteArray result = archive;
    const auto index = result.indexOf(from);
    if (index >= 0 && from.size() == to.size()) result.replace(index, from.size(), to);
    return result;
}
} // namespace

class ArchiveTests : public QObject {
    Q_OBJECT
private slots:
    void extractsDeflatePackage();
    void extractsStoredPackage();
    void rejectsTraversal();
    void rejectsAbsoluteNames();
    void rejectsBackslashTraversal();
    void rejectsDuplicateNamesCaseInsensitively();
    void rejectsSymlinkEntry();
    void rejectsReparseAttribute();
    void rejectsDirectoryWithData();
    void rejectsEncryptedEntries();
    void rejectsUnsupportedCompression();
    void rejectsOversizedEntry();
    void rejectsEntryCountLimit();
    void rejectsTotalSizeLimit();
    void rejectsZip64Marker();
    void rejectsMissingEndOfCentralDirectory();
    void rejectsNonZip();
    void rejectsLocalNameMismatch();
    void rejectsCorruptCrc();
    void rejectsCorruptDeflate();
    void rejectsFileDirectoryConflict();
    void rejectsEmptyPackage();
    void rejectsNonEmptyStaging();
};

void ArchiveTests::extractsDeflatePackage()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto staging = temporary.filePath(QStringLiteral("stage"));
    QList<ExtractedFile> files;
    QString error;
    ArchiveLimits limits;
    const auto package = QDir(fixturesDirectory).filePath(QStringLiteral("deflate-package.zip"));
    QVERIFY2(UpdateArchive::extract(package, staging, limits, files, error), qPrintable(error));
    QCOMPARE(files.size(), 3);
    QHash<QString, ExtractedFile> byPath;
    for (const auto &file : files) byPath.insert(file.path, file);
    QVERIFY(byPath.contains(QStringLiteral("Z2.exe")));
    QVERIFY(byPath.contains(QStringLiteral("licenses/LICENSE.txt")));
    QVERIFY(byPath.contains(QStringLiteral("qml/ZTOrbit/Test.qml")));
    const auto exe = byPath.value(QStringLiteral("Z2.exe"));
    QCOMPARE(exe.sha256, zto_update_test::sha256FileHex(QDir(staging).filePath(exe.path)));
    const auto expected = QByteArrayLiteral("MZ fake executable for update tests\n").repeated(32);
    QCOMPARE(zto_update_test::readFile(QDir(staging).filePath(QStringLiteral("Z2.exe"))), expected);
    QVERIFY(QFileInfo::exists(QDir(staging).filePath(QStringLiteral("qml/ZTOrbit/Test.qml"))));
}

void ArchiveTests::extractsStoredPackage()
{
    QTemporaryDir temporary;
    const auto archive = singleFileZip("a/b.txt", "stored payload");
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    QString error;
    QVERIFY(zto_update_test::writeFile(path, archive, error));
    QList<ExtractedFile> files;
    ArchiveLimits limits;
    QVERIFY2(UpdateArchive::extract(path, temporary.filePath(QStringLiteral("stage")), limits, files,
                                    error),
             qPrintable(error));
    QCOMPARE(files.size(), 1);
    QCOMPARE(files.first().path, QStringLiteral("a/b.txt"));
    QCOMPARE(files.first().size, qint64(14));
    QCOMPARE(files.first().sha256, zto_update_test::sha256Hex("stored payload"));
}

void ArchiveTests::rejectsTraversal()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    QString error;
    QVERIFY(zto_update_test::writeFile(path, singleFileZip("../evil.txt", "x"), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
    QVERIFY2(error.contains(QStringLiteral("安全")) || error.contains(QStringLiteral("路径")),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("evil.txt"))));
}

void ArchiveTests::rejectsAbsoluteNames()
{
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    for (const auto &name : {QByteArrayLiteral("/evil.txt"), QByteArrayLiteral("C:/evil.txt"),
                             QByteArrayLiteral("\\\\server\\share\\evil.txt")}) {
        QTemporaryDir temporary;
        const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
        QString error;
        QVERIFY(zto_update_test::writeFile(path, singleFileZip(name, "x"), error));
        QVERIFY2(!UpdateArchive::validate(path, limits, entries, error), name.constData());
    }
}

void ArchiveTests::rejectsBackslashTraversal()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    QString error;
    QVERIFY(zto_update_test::writeFile(path, singleFileZip("..\\evil.txt", "x"), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
}

void ArchiveTests::rejectsDuplicateNamesCaseInsensitively()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec first;
    first.name = "A.dll";
    first.data = "a";
    zto_update_test::ZipEntrySpec second;
    second.name = "a.dll";
    second.data = "b";
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({first, second}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
    QVERIFY2(error.contains(QStringLiteral("重复")), qPrintable(error));
}

void ArchiveTests::rejectsSymlinkEntry()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec entry;
    entry.name = "link.dll";
    entry.data = "target";
    entry.externalAttributes = 0xA1FF0000u; // Unix S_IFLNK | 0777
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({entry}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
    QVERIFY2(error.contains(QStringLiteral("符号链接")), qPrintable(error));
}

void ArchiveTests::rejectsReparseAttribute()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec entry;
    entry.name = "link.dll";
    entry.data = "target";
    entry.externalAttributes = 0x400; // FILE_ATTRIBUTE_REPARSE_POINT
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({entry}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
    QVERIFY2(error.contains(QStringLiteral("重解析")), qPrintable(error));
}

void ArchiveTests::rejectsDirectoryWithData()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec entry;
    entry.name = "dir/";
    entry.data = "payload";
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({entry}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
}

void ArchiveTests::rejectsEncryptedEntries()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec entry;
    entry.name = "a.dll";
    entry.data = "payload";
    entry.flags = 0x0001;
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({entry}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
    QVERIFY2(error.contains(QStringLiteral("加密")), qPrintable(error));
}

void ArchiveTests::rejectsUnsupportedCompression()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec entry;
    entry.name = "a.dll";
    entry.data = "payload";
    entry.method = 12;
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({entry}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
    QVERIFY2(error.contains(QStringLiteral("压缩算法")), qPrintable(error));
}

void ArchiveTests::rejectsOversizedEntry()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec entry;
    entry.name = "a.dll";
    entry.data = QByteArray(2048, 'x');
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({entry}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    limits.maxEntryUncompressed = 1024;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
    QVERIFY2(error.contains(QStringLiteral("超过限制")), qPrintable(error));
}

void ArchiveTests::rejectsEntryCountLimit()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec first;
    first.name = "a.dll";
    first.data = "a";
    zto_update_test::ZipEntrySpec second;
    second.name = "b.dll";
    second.data = "b";
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({first, second}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    limits.maxEntries = 1;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
}

void ArchiveTests::rejectsTotalSizeLimit()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec first;
    first.name = "a.dll";
    first.data = QByteArray(600, 'a');
    zto_update_test::ZipEntrySpec second;
    second.name = "b.dll";
    second.data = QByteArray(600, 'b');
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({first, second}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    limits.maxTotalUncompressed = 1000;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
}

void ArchiveTests::rejectsZip64Marker()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec entry;
    entry.name = "a.dll";
    entry.data = "x";
    entry.uncompressedSize = 0xFFFFFFFFu;
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({entry}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
    QVERIFY2(error.contains(QStringLiteral("ZIP64")), qPrintable(error));
}

void ArchiveTests::rejectsMissingEndOfCentralDirectory()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    auto archive = fixture(QStringLiteral("deflate-package.zip"));
    QVERIFY(!archive.isEmpty());
    QString error;
    QVERIFY(zto_update_test::writeFile(path, archive.left(archive.size() - 30), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
}

void ArchiveTests::rejectsNonZip()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    QString error;
    QVERIFY(zto_update_test::writeFile(path, "this is not a zip file", error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
    QVERIFY(error.contains(QStringLiteral("ZIP")));
}

void ArchiveTests::rejectsLocalNameMismatch()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    auto archive = singleFileZip("a.dll", "payload");
    archive = patchLocalName(archive, "a.dll", "b.dll");
    QString error;
    QVERIFY(zto_update_test::writeFile(path, archive, error));
    QList<ExtractedFile> files;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::extract(path, temporary.filePath(QStringLiteral("stage")), limits, files,
                                    error));
    QVERIFY2(error.contains(QStringLiteral("本地文件头")), qPrintable(error));
}

void ArchiveTests::rejectsCorruptCrc()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    auto archive = singleFileZip("a.dll", "payload");
    // The stored payload starts at offset 30 + nameLength; flip one byte.
    const int dataOffset = 30 + 5;
    archive[dataOffset] = archive[dataOffset] == 'p' ? 'q' : 'p';
    QString error;
    QVERIFY(zto_update_test::writeFile(path, archive, error));
    QList<ExtractedFile> files;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::extract(path, temporary.filePath(QStringLiteral("stage")), limits, files,
                                    error));
    QVERIFY2(error.contains(QStringLiteral("CRC")), qPrintable(error));
}

void ArchiveTests::rejectsCorruptDeflate()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    auto archive = fixture(QStringLiteral("deflate-package.zip"));
    QVERIFY(!archive.isEmpty());
    // Flip a byte in the middle of the compressed payload.
    archive[archive.size() / 2] = static_cast<char>(archive.at(archive.size() / 2) ^ 0x5a);
    QString error;
    QVERIFY(zto_update_test::writeFile(path, archive, error));
    QList<ExtractedFile> files;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::extract(path, temporary.filePath(QStringLiteral("stage")), limits, files,
                                    error));
}

void ArchiveTests::rejectsFileDirectoryConflict()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    zto_update_test::ZipEntrySpec file;
    file.name = "a";
    file.data = "file";
    zto_update_test::ZipEntrySpec nested;
    nested.name = "a/b.dll";
    nested.data = "nested";
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({file, nested}), error));
    QList<ArchiveEntry> entries;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::validate(path, limits, entries, error));
}

void ArchiveTests::rejectsEmptyPackage()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath(QStringLiteral("pkg.zip"));
    QString error;
    QVERIFY(zto_update_test::writeFile(path, zto_update_test::buildZip({}), error));
    QList<ExtractedFile> files;
    ArchiveLimits limits;
    QVERIFY(!UpdateArchive::extract(path, temporary.filePath(QStringLiteral("stage")), limits, files,
                                    error));
}

void ArchiveTests::rejectsNonEmptyStaging()
{
    QTemporaryDir temporary;
    const auto staging = temporary.filePath(QStringLiteral("stage"));
    QString error;
    QVERIFY(zto_update_test::writeFile(QDir(staging).filePath(QStringLiteral("existing.txt")), "x",
                                       error));
    QList<ExtractedFile> files;
    ArchiveLimits limits;
    const auto package = QDir(fixturesDirectory).filePath(QStringLiteral("deflate-package.zip"));
    QVERIFY(!UpdateArchive::extract(package, staging, limits, files, error));
    QVERIFY(error.contains(QStringLiteral("空")));
}

QTEST_GUILESS_MAIN(ArchiveTests)
#include "ArchiveTests.moc"
