#include "TestSupport.h"
#include "UpdateCrypto.h"
#include "UpdateManifest.h"
#include "UpdateVersion.h"
#include "UpdaterPlan.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

using namespace vusiif::zto::update;
using namespace vusiif::zto::update::updater;

namespace {
const QString fixturesDirectory = QStringLiteral(UPDATE_FIXTURES_DIR);

QByteArray readFixture(const QString &relative)
{
    return zto_update_test::readFile(QDir(fixturesDirectory).filePath(relative));
}

updater::Plan validPlan(const QTemporaryDir &temporary)
{
    updater::Plan plan;
    plan.packageVersion = "0.5.4";
    plan.installDir = QDir::toNativeSeparators(temporary.filePath(QStringLiteral("install"))).toStdString();
    plan.stagingDir = QDir::toNativeSeparators(temporary.filePath(QStringLiteral("staging"))).toStdString();
    plan.transactionDir = QDir::toNativeSeparators(temporary.filePath(QStringLiteral("transaction"))).toStdString();
    plan.runtimeDir = QDir::toNativeSeparators(temporary.filePath(QStringLiteral("runtime"))).toStdString();
    plan.logFile = QDir::toNativeSeparators(temporary.filePath(QStringLiteral("updater.log"))).toStdString();
    plan.resultFile = QDir::toNativeSeparators(temporary.filePath(QStringLiteral("last-result.txt"))).toStdString();
    plan.parentPid = 4242;
    plan.restartRelative = "Z2.exe";
    PlanFile file;
    file.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    file.size = 12;
    file.path = "sub dir/a.dll";
    plan.files.push_back(file);
    plan.removals.push_back("old-plugin.dll");
    return plan;
}
} // namespace

class ProtocolTests : public QObject {
    Q_OBJECT
private slots:
    // Manifest
    void generatorOutputVerifies();
    void manifestParsesFixture();
    void manifestRejectsInvalidDocuments_data();
    void manifestRejectsInvalidDocuments();
    void selectsArtifactByPlatform();
    void rejectsWrongPlatformOrArch();
    // Signature
    void rejectsTamperedManifest();
    void rejectsWrongPublicKey();
    void rejectsMalformedSignature();
    // Plan
    void planRoundTrips();
    void planAllowsSpacesInPaths();
    void planRejectsUnsafeRelativePaths_data();
    void planRejectsUnsafeRelativePaths();
    void planRejectsStructuralProblems_data();
    void planRejectsStructuralProblems();
    void allowsNormalReleasePaths();
    void protectedInstallPaths();
    void rejectsPlanTrailingGarbage();
};

void ProtocolTests::generatorOutputVerifies()
{
    // The fixture was produced by scripts/update-manifest.py. Verifying it here
    // proves the generator's output against the published public key using the
    // same libsodium path the client uses.
    const auto manifestBytes = readFixture(QStringLiteral("example/manifest.json"));
    const auto signatureText = readFixture(QStringLiteral("example/manifest.json.sig"));
    const auto publicKeyText = readFixture(QStringLiteral("keys/update-signing.pub"));
    QVERIFY(!manifestBytes.isEmpty());
    QVERIFY(!signatureText.isEmpty());
    QVERIFY(!publicKeyText.isEmpty());

    QByteArray signature;
    QString error;
    QVERIFY2(UpdateCrypto::decodeSignature(signatureText, signature, error), qPrintable(error));
    QByteArray publicKey;
    QVERIFY2(UpdateCrypto::decodePublicKey(QString::fromLatin1(publicKeyText).trimmed(), publicKey, error),
             qPrintable(error));
    QVERIFY2(UpdateCrypto::verifyDetached(manifestBytes, signature, publicKey, error), qPrintable(error));
}

void ProtocolTests::manifestParsesFixture()
{
    const auto bytes = readFixture(QStringLiteral("example/manifest.json"));
    UpdateManifest manifest;
    QString error;
    QVERIFY2(parseManifest(bytes, manifest, error), qPrintable(error));
    QCOMPARE(manifest.schemaVersion, 1);
    QCOMPARE(manifest.channel, QStringLiteral("stable"));
    QCOMPARE(manifest.minUpdatableVersion, QStringLiteral("0.5.0"));
    QCOMPARE(manifest.artifacts.size(), 1);
    const auto &artifact = manifest.artifacts.first();
    QCOMPARE(artifact.platform, QStringLiteral("windows"));
    QCOMPARE(artifact.arch, QStringLiteral("x64"));
    QCOMPARE(artifact.version, QStringLiteral("0.5.4"));
    QCOMPARE(artifact.removeFiles, QStringList{QStringLiteral("old-plugin.dll")});
    QVERIFY(artifact.size > 0);
    QCOMPARE(artifact.sha256.size(), 64);
    QCOMPARE(zto_update_test::sha256FileHex(
                 QDir(fixturesDirectory).filePath(QStringLiteral("deflate-package.zip"))),
             artifact.sha256);
}

void ProtocolTests::manifestRejectsInvalidDocuments_data()
{
    QTest::addColumn<QByteArray>("document");
    QTest::addColumn<QString>("expectedFragment");

    const auto base = QByteArrayLiteral(
        "{\"schemaVersion\":1,\"channel\":\"stable\",\"publishedAt\":\"2026-11-02T12:00:00Z\","
        "\"minUpdatableVersion\":\"0.5.0\",\"artifacts\":[{\"platform\":\"windows\",\"arch\":\"x64\","
        "\"version\":\"0.5.4\",\"url\":\"https://updates.example.invalid/z2/Z2.zip\",\"size\":481,"
        "\"sha256\":\"99520eb11110084c6ee9f880652ddfb7685436409b9780e5f83f0e7ff24931d7\"}]}");
    QTest::newRow("valid baseline") << base << QString();
    QTest::newRow("empty") << QByteArray() << QStringLiteral("空");
    QTest::newRow("not json") << QByteArrayLiteral("not json") << QStringLiteral("JSON");
    QTest::newRow("not object") << QByteArrayLiteral("[]") << QStringLiteral("对象");
    QTest::newRow("schema version") << QByteArray(base).replace("\"schemaVersion\":1", "\"schemaVersion\":2")
                                    << QStringLiteral("schemaVersion");
    QTest::newRow("missing minimum") << QByteArray(base).replace("\"minUpdatableVersion\":\"0.5.0\",", "")
                                     << QStringLiteral("minUpdatableVersion");
    QTest::newRow("bad minimum") << QByteArray(base).replace("\"0.5.0\"", "\"0.5\"")
                                 << QStringLiteral("minUpdatableVersion");
    QTest::newRow("no artifacts") << QByteArray(base).replace(
                                         "\"artifacts\":[{\"platform\":\"windows\",\"arch\":\"x64\",\"version\":\"0.5.4\",\"url\":\"https://updates.example.invalid/z2/Z2.zip\",\"size\":481,\"sha256\":\"99520eb11110084c6ee9f880652ddfb7685436409b9780e5f83f0e7ff24931d7\"}]",
                                         "\"artifacts\":[]")
                                  << QStringLiteral("artifacts");
    QTest::newRow("bad sha") << QByteArray(base).replace("99520eb11110084c6ee9f880652ddfb7685436409b9780e5f83f0e7ff24931d7",
                                                         "zz520eb11110084c6ee9f880652ddfb7685436409b9780e5f83f0e7ff24931d7")
                             << QStringLiteral("SHA-256");
    QTest::newRow("zero size") << QByteArray(base).replace("\"size\":481", "\"size\":0")
                               << QStringLiteral("大小");
    QTest::newRow("oversize") << QByteArray(base).replace("\"size\":481", "\"size\":999999999999")
                              << QStringLiteral("大小");
    QTest::newRow("ftp url") << QByteArray(base).replace("https://updates.example.invalid/z2/Z2.zip",
                                                         "ftp://updates.example.invalid/z2/Z2.zip")
                             << QStringLiteral("HTTP");
    QTest::newRow("credentials") << QByteArray(base).replace("https://updates.example.invalid/z2/Z2.zip",
                                                             "https://user:pass@updates.example.invalid/z2/Z2.zip")
                                 << QStringLiteral("凭据");
    QTest::newRow("bad version") << QByteArray(base).replace("\"version\":\"0.5.4\"", "\"version\":\"0.5.4.1\"")
                                 << QStringLiteral("SemVer");
    QTest::newRow("traversal remove") << QByteArray(base).replace(
                                             "\"sha256\":\"99520eb11110084c6ee9f880652ddfb7685436409b9780e5f83f0e7ff24931d7\"",
                                             "\"sha256\":\"99520eb11110084c6ee9f880652ddfb7685436409b9780e5f83f0e7ff24931d7\",\"removeFiles\":[\"../escape.dll\"]")
                                      << QStringLiteral("不安全");
    QTest::newRow("protected remove") << QByteArray(base).replace(
                                                 "\"sha256\":\"99520eb11110084c6ee9f880652ddfb7685436409b9780e5f83f0e7ff24931d7\"",
                                                 "\"sha256\":\"99520eb11110084c6ee9f880652ddfb7685436409b9780e5f83f0e7ff24931d7\",\"removeFiles\":[\"plugins/evil.dll\"]")
                                          << QStringLiteral("不安全");
    QTest::newRow("bad published") << QByteArray(base).replace("2026-11-02T12:00:00Z", "yesterday")
                                   << QStringLiteral("publishedAt");
    QTest::newRow("bad channel") << QByteArray(base).replace("\"stable\"", "\"Stable Channel\"")
                                 << QStringLiteral("channel");
}

void ProtocolTests::manifestRejectsInvalidDocuments()
{
    QFETCH(QByteArray, document);
    QFETCH(QString, expectedFragment);
    UpdateManifest manifest;
    QString error;
    const bool parsed = parseManifest(document, manifest, error);
    if (expectedFragment.isEmpty()) {
        QVERIFY2(parsed, qPrintable(error));
        return;
    }
    QVERIFY(!parsed);
    QVERIFY2(error.contains(expectedFragment), qPrintable(error));
}

void ProtocolTests::selectsArtifactByPlatform()
{
    const auto bytes = readFixture(QStringLiteral("example/manifest.json"));
    UpdateManifest manifest;
    QString error;
    QVERIFY(parseManifest(bytes, manifest, error));
    UpdateArtifact artifact;
    QVERIFY(selectArtifact(manifest, QStringLiteral("windows"), QStringLiteral("x64"), artifact, error));
    QCOMPARE(artifact.version, QStringLiteral("0.5.4"));
}

void ProtocolTests::rejectsWrongPlatformOrArch()
{
    const auto bytes = readFixture(QStringLiteral("example/manifest.json"));
    UpdateManifest manifest;
    QString error;
    QVERIFY(parseManifest(bytes, manifest, error));
    UpdateArtifact artifact;
    QVERIFY(!selectArtifact(manifest, QStringLiteral("linux"), QStringLiteral("x64"), artifact, error));
    QVERIFY2(error.contains(QStringLiteral("linux")), qPrintable(error));
    QVERIFY(!selectArtifact(manifest, QStringLiteral("windows"), QStringLiteral("arm64"), artifact, error));
    QVERIFY2(error.contains(QStringLiteral("arm64")), qPrintable(error));
}

void ProtocolTests::rejectsTamperedManifest()
{
    auto bytes = readFixture(QStringLiteral("example/manifest.json"));
    bytes[10] = bytes[10] == '0' ? '1' : '0';
    const auto signatureText = readFixture(QStringLiteral("example/manifest.json.sig"));
    const auto publicKeyText = readFixture(QStringLiteral("keys/update-signing.pub"));
    QByteArray signature;
    QByteArray publicKey;
    QString error;
    QVERIFY(UpdateCrypto::decodeSignature(signatureText, signature, error));
    QVERIFY(UpdateCrypto::decodePublicKey(QString::fromLatin1(publicKeyText).trimmed(), publicKey, error));
    QVERIFY(!UpdateCrypto::verifyDetached(bytes, signature, publicKey, error));
    QVERIFY2(error.contains(QStringLiteral("签名")), qPrintable(error));
}

void ProtocolTests::rejectsWrongPublicKey()
{
    const auto manifestBytes = readFixture(QStringLiteral("example/manifest.json"));
    const auto signatureText = readFixture(QStringLiteral("example/manifest.json.sig"));
    QByteArray signature;
    QString error;
    QVERIFY(UpdateCrypto::decodeSignature(signatureText, signature, error));
    // A different, valid 32-byte key must not verify the release signature.
    const QByteArray otherKey(32, '\x42');
    QVERIFY(!UpdateCrypto::verifyDetached(manifestBytes, signature, otherKey, error));
}

void ProtocolTests::rejectsMalformedSignature()
{
    QByteArray signature;
    QString error;
    QVERIFY(!UpdateCrypto::decodeSignature(QByteArrayLiteral("not base64 ***"), signature, error));
    QVERIFY(!UpdateCrypto::decodeSignature(QByteArrayLiteral("AAAA"), signature, error));
    QVERIFY(error.contains(QStringLiteral("64")));

    QByteArray key;
    QVERIFY(!UpdateCrypto::decodePublicKey(QStringLiteral("AAAA"), key, error));
    QVERIFY(error.contains(QStringLiteral("32")));
}

void ProtocolTests::planRoundTrips()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto plan = validPlan(temporary);
    std::string error;
    QVERIFY2(validatePlan(plan, error), error.c_str());
    std::string text;
    QVERIFY2(serializePlan(plan, text, error), error.c_str());
    Plan parsed;
    QVERIFY2(parsePlan(text, parsed, error), error.c_str());
    QCOMPARE(QString::fromStdString(parsed.installDir), QString::fromStdString(plan.installDir));
    QCOMPARE(parsed.files.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(parsed.files[0].path), QStringLiteral("sub dir/a.dll"));
    QCOMPARE(parsed.removals.size(), std::size_t(1));
    QCOMPARE(parsed.parentPid, quint64(4242));
}

void ProtocolTests::planAllowsSpacesInPaths()
{
    QTemporaryDir temporary;
    const auto plan = validPlan(temporary);
    std::string text;
    std::string error;
    QVERIFY(serializePlan(plan, text, error));
    QVERIFY(text.find("sub dir/a.dll") != std::string::npos);
}

void ProtocolTests::planRejectsUnsafeRelativePaths_data()
{
    QTest::addColumn<QString>("path");
    QTest::newRow("parent") << QStringLiteral("../escape.dll");
    QTest::newRow("nested parent") << QStringLiteral("a/../../escape.dll");
    QTest::newRow("absolute posix") << QStringLiteral("/escape.dll");
    QTest::newRow("absolute windows") << QStringLiteral("C:/Windows/escape.dll");
    QTest::newRow("drive relative") << QStringLiteral("C:escape.dll");
    QTest::newRow("unc") << QStringLiteral("\\\\server\\share\\escape.dll");
    QTest::newRow("backslash parent") << QStringLiteral("..\\escape.dll");
    QTest::newRow("ads") << QStringLiteral("a.dll:stream");
    QTest::newRow("trailing dot") << QStringLiteral("a.dll.");
    QTest::newRow("trailing space") << QStringLiteral("a.dll ");
    QTest::newRow("reserved con") << QStringLiteral("CON");
    QTest::newRow("reserved nul") << QStringLiteral("sub/NUL.txt");
    QTest::newRow("reserved com1") << QStringLiteral("COM1.dll");
    QTest::newRow("empty") << QString();
}

void ProtocolTests::planRejectsUnsafeRelativePaths()
{
    QFETCH(QString, path);
    std::string error;
    QVERIFY2(!isValidRelativePath(path.toStdString(), error), qPrintable(path));
    QVERIFY(!error.empty());
}

void ProtocolTests::planRejectsStructuralProblems_data()
{
    QTest::addColumn<QString>("problem");
    QTest::newRow("missing end") << QStringLiteral("missing-end");
    QTest::newRow("unknown key") << QStringLiteral("unknown-key");
    QTest::newRow("duplicate file") << QStringLiteral("duplicate-file");
    QTest::newRow("missing header") << QStringLiteral("missing-header");
    QTest::newRow("bad parent pid") << QStringLiteral("bad-pid");
    QTest::newRow("relative install") << QStringLiteral("relative-install");
    QTest::newRow("bad restart") << QStringLiteral("bad-restart");
    QTest::newRow("bad hash") << QStringLiteral("bad-hash");
    QTest::newRow("no files") << QStringLiteral("no-files");
}

void ProtocolTests::planRejectsStructuralProblems()
{
    QFETCH(QString, problem);
    QTemporaryDir temporary;
    auto plan = validPlan(temporary);
    std::string text;
    std::string error;
    QVERIFY(serializePlan(plan, text, error));
    if (problem == QLatin1String("missing-end")) {
        const auto position = text.find("end\n");
        QVERIFY(position != std::string::npos);
        text.erase(position, 4);
    } else if (problem == QLatin1String("unknown-key")) {
        text.insert(0, "surprise=1\n");
    } else if (problem == QLatin1String("duplicate-file")) {
        plan.files.push_back(plan.files.front());
        text.clear();
        QVERIFY(!serializePlan(plan, text, error));
        return;
    } else if (problem == QLatin1String("missing-header")) {
        text = text.substr(text.find('\n') + 1);
    } else if (problem == QLatin1String("bad-pid")) {
        plan.parentPid = 0;
        text.clear();
        QVERIFY(!serializePlan(plan, text, error));
        return;
    } else if (problem == QLatin1String("relative-install")) {
        plan.installDir = "relative\\install";
        text.clear();
        QVERIFY(!serializePlan(plan, text, error));
        return;
    } else if (problem == QLatin1String("bad-restart")) {
        plan.restartRelative = "sub/Z2.exe";
        text.clear();
        QVERIFY(!serializePlan(plan, text, error));
        return;
    } else if (problem == QLatin1String("bad-hash")) {
        plan.files[0].sha256 = "XYZ";
        text.clear();
        QVERIFY(!serializePlan(plan, text, error));
        return;
    } else if (problem == QLatin1String("no-files")) {
        plan.files.clear();
        plan.removals.clear();
        text.clear();
        QVERIFY(!serializePlan(plan, text, error));
        return;
    }
    Plan parsed;
    QVERIFY(!parsePlan(text, parsed, error));
    QVERIFY(!error.empty());
}

void ProtocolTests::allowsNormalReleasePaths()
{
    for (const auto &path : {QStringLiteral("Z2.exe"), QStringLiteral("Qt6Core.dll"),
                             QStringLiteral("qml/QtQuick/Controls/Basic/Button.qml"),
                             QStringLiteral("licenses/libsodium-LICENSE.txt"),
                             QStringLiteral("plugins/../readme.txt")}) {
        std::string error;
        if (path == QLatin1String("plugins/../readme.txt")) {
            QVERIFY(!isValidRelativePath(path.toStdString(), error));
            continue;
        }
        QVERIFY2(isValidRelativePath(path.toStdString(), error), qPrintable(path));
    }
}

void ProtocolTests::protectedInstallPaths()
{
    for (const auto &path : {QStringLiteral("settings.ini"), QStringLiteral("plugins/x.dll"),
                             QStringLiteral("plugins-data/id-a/b.json"), QStringLiteral("logs/a.log"),
                             QStringLiteral("cache/x.bin"), QStringLiteral("updates/plan/a.plan"),
                             QStringLiteral(".zto/plugins/x.json")}) {
        QVERIFY2(isProtectedInstallPath(path.toStdString()), qPrintable(path));
        std::string error;
        QVERIFY2(!isValidRelativePath(path.toStdString(), error), qPrintable(path));
    }
}

void ProtocolTests::rejectsPlanTrailingGarbage()
{
    QTemporaryDir temporary;
    const auto plan = validPlan(temporary);
    std::string text;
    std::string error;
    QVERIFY2(serializePlan(plan, text, error), error.c_str());
    Plan parsed;
    // A second document after the end marker is rejected instead of being
    // silently ignored, so a plan file cannot carry hidden extra instructions.
    const std::string newline(1, static_cast<char>(10));
    const auto trailing = text + "install-dir=C:/other" + newline + "end" + newline;
    QVERIFY(!parsePlan(trailing, parsed, error));
    QVERIFY(!error.empty());
}

QTEST_GUILESS_MAIN(ProtocolTests)
#include "ProtocolTests.moc"
