#include "TestSupport.h"
#include "UpdateConfig.h"
#include "UpdateController.h"
#include "UpdateManifest.h"
#include "UpdaterPlan.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace vusiif::zto::update;

namespace {

QByteArray buildPackage(const QByteArray &payload = QByteArrayLiteral("new payload"))
{
    zto_update_test::ZipEntrySpec first;
    first.name = "Z2.exe";
    first.data = payload;
    zto_update_test::ZipEntrySpec second;
    second.name = "licenses/LICENSE.txt";
    second.data = "license";
    return zto_update_test::buildZip({first, second});
}

QByteArray buildManifest(const QString &version, const QString &platform, const QString &arch,
                         const QByteArray &package, const QString &url,
                         const QString &minimum = QStringLiteral("0.5.0"))
{
    QJsonObject artifact;
    artifact.insert(QStringLiteral("platform"), platform);
    artifact.insert(QStringLiteral("arch"), arch);
    artifact.insert(QStringLiteral("version"), version);
    artifact.insert(QStringLiteral("url"), url);
    artifact.insert(QStringLiteral("size"), package.size());
    artifact.insert(QStringLiteral("sha256"),
                    zto_update_test::sha256Hex(package));
    QJsonObject document;
    document.insert(QStringLiteral("schemaVersion"), 1);
    document.insert(QStringLiteral("channel"), QStringLiteral("stable"));
    document.insert(QStringLiteral("publishedAt"), QStringLiteral("2026-11-02T12:00:00Z"));
    document.insert(QStringLiteral("minUpdatableVersion"), minimum);
    document.insert(QStringLiteral("artifacts"), QJsonArray{artifact});
    return QJsonDocument(document).toJson(QJsonDocument::Indented);
}

struct Fixture {
    QTemporaryDir temporary;
    zto_update_test::TestHttpServer *server = nullptr;
    zto_update_test::TestKeyPair keys;
    QByteArray package;
    QByteArray manifest;
    QByteArray signature;
    QString installDirectory;
    QString storageRoot;
    QString settingsFile;
    QString pseudoExecutable;

    bool initialize(QObject *parent, QString &error)
    {
        keys = zto_update_test::TestKeyPair::load(QStringLiteral(UPDATE_FIXTURES_DIR), error);
        if (!error.isEmpty()) return false;
        server = new zto_update_test::TestHttpServer(parent);
        if (!server->start()) {
            error = QStringLiteral("cannot start the local HTTP server");
            return false;
        }
        installDirectory = temporary.filePath(QStringLiteral("install"));
        storageRoot = temporary.filePath(QStringLiteral("storage"));
        settingsFile = temporary.filePath(QStringLiteral("settings.ini"));
        pseudoExecutable = QDir(installDirectory).filePath(QStringLiteral("Z2.exe"));
        QDir().mkpath(installDirectory);
        QDir().mkpath(storageRoot);
        zto_update_test::writeFile(pseudoExecutable, "old payload", error);
        return error.isEmpty() && QFile::copy(QStringLiteral(UPDATE_NATIVE_HELPER), QDir(installDirectory).filePath(QStringLiteral("z2_updater.exe")));
    }

    void serveUpdate(const QByteArray &manifestBytes, const QByteArray &signatureBytes,
                     const QByteArray &packageBytes)
    {
        const auto base = server->baseUrl();
        server->setHandler([manifestBytes, signatureBytes, packageBytes, base](const QString &path) {
            if (path == QLatin1String("/updater"))
                return zto_update_test::TestResponse{200, "application/json", manifestBytes, {}, 0, false, false, -1, {{"X-Z2-Signature", signatureBytes.toBase64()}}};
            if (path.startsWith(QLatin1String("/file?version=")))
                return zto_update_test::TestResponse{200, "application/zip", packageBytes, {}, 0, false, false, -1, {}};
            Q_UNUSED(base)
            return zto_update_test::TestResponse{404, "text/plain", "not found", {}, 0, false, false, -1, {}};
        });
    }

    QString manifestUrl() const { return server->baseUrl() + QStringLiteral("/updater"); }
    QString packageUrl(const QString &version) const { return server->baseUrl() + QStringLiteral("/file?version=") + version; }

    void writeSettingsUrl(const QString &url)
    {
        QSettings settings(settingsFile, QSettings::IniFormat);
        QUrl address(url.trimmed());
        address.setPath(QString());
        settings.setValue(QStringLiteral("update/server"), address.toString());
        settings.sync();
    }

    void prepareUpdate(const QString &version = QStringLiteral("0.5.4"),
                       const QString &platform = QStringLiteral("windows"),
                       const QString &arch = QStringLiteral("x64"),
                       const QString &minimum = QStringLiteral("0.5.0"),
                       const QByteArray &payload = QByteArrayLiteral("new payload"))
    {
        package = buildPackage(payload);
        manifest = buildManifest(version, platform, arch, package, packageUrl(version), minimum);
        QString error;
        signature = zto_update_test::signDetached(keys.seed, manifest, error);
        serveUpdate(manifest, signature, package);
        writeSettingsUrl(manifestUrl());
    }
};

} // namespace

class ControllerTests : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void startsUnconfigured();
    void manualUnconfiguredNotifies();
    void savesServerAndClearsToDisable();
    void rejectsInvalidServerWithoutChangingSettings();
    void clearCancelsDeferredStartupCheck();
    void nativeDownloadRejectsInvalidResponses_data();
    void nativeDownloadRejectsInvalidResponses();
    void noUpdateIsQuiet();
    void installsVerifiedUpdate();
    void rejectsBelowMinimumVersion();
    void rejectsWrongPlatform();
    void rejectsBadSignature();
    void rejectsCorruptPackage();
    void guardsReentrancy();
    void respectsHostBusy();
    void surfacesInstalledResult();
    void surfacesFailedResult();
    void recoversInterruptedTransaction();

private:
    Fixture *m_fixture = nullptr;
};

void ControllerTests::init()
{
    QCoreApplication::setApplicationVersion(QStringLiteral("0.5.3"));
    m_fixture = new Fixture;
    QString error;
    QVERIFY2(m_fixture->initialize(this, error), qPrintable(error));
}

void ControllerTests::cleanup()
{
    delete m_fixture;
    m_fixture = nullptr;
}

void ControllerTests::startsUnconfigured()
{
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    QSignalSpy notifications(&controller, &UpdateController::notificationRequested);
    controller.checkOnStartup();
    QCOMPARE(controller.state(), QStringLiteral("unconfigured"));
    QVERIFY(controller.status().contains(QStringLiteral("未配置")));
    QVERIFY(!controller.configured());
    QCOMPARE(notifications.count(), 0);
    QCOMPARE(m_fixture->server->requestCount(), 0);
}

void ControllerTests::manualUnconfiguredNotifies()
{
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    QSignalSpy notifications(&controller, &UpdateController::notificationRequested);
    controller.checkManually();
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notifications.first().at(1).toBool(), true);
    QCOMPARE(controller.state(), QStringLiteral("unconfigured"));
    QCOMPARE(m_fixture->server->requestCount(), 0);
}

void ControllerTests::savesServerAndClearsToDisable()
{
    m_fixture->prepareUpdate(QStringLiteral("0.5.3"));
    m_fixture->writeSettingsUrl(QStringLiteral("   "));
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    controller.checkOnStartup();
    QVERIFY(controller.feedUrl().isEmpty());
    QCOMPARE(m_fixture->server->requestCount(), 0);

    QVERIFY(controller.setServer(QStringLiteral(" 127.0.0.1 "), m_fixture->server->serverPort()));
    QCOMPARE(controller.feedUrl(), m_fixture->manifestUrl());
    QVERIFY(controller.configured());
    QCOMPARE(controller.state(), QStringLiteral("idle"));
    QCOMPARE(m_fixture->server->requestCount(), 0); // Saving is not a check.
    {
        UpdateController reopened(m_fixture->installDirectory, m_fixture->storageRoot,
                                  m_fixture->settingsFile);
        QCOMPARE(reopened.feedUrl(), m_fixture->manifestUrl());
    }
    controller.checkManually();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), QStringLiteral("uptodate"), 5000);
    const auto requests = m_fixture->server->requestCount();
    QVERIFY(requests > 0);

    QVERIFY(controller.setServer(QStringLiteral("   ")));
    QVERIFY(controller.feedUrl().isEmpty());
    QVERIFY(!controller.configured());
    QCOMPARE(controller.state(), QStringLiteral("unconfigured"));
    QVERIFY(controller.lastError().isEmpty());
    QSettings settings(m_fixture->settingsFile, QSettings::IniFormat);
    QVERIFY(!settings.contains(QStringLiteral("update/server")));
    controller.checkManually();
    UpdateController reopened(m_fixture->installDirectory, m_fixture->storageRoot,
                              m_fixture->settingsFile);
    reopened.checkOnStartup();
    QTest::qWait(100);
    QCOMPARE(m_fixture->server->requestCount(), requests);
}

void ControllerTests::rejectsInvalidServerWithoutChangingSettings()
{
    m_fixture->prepareUpdate(QStringLiteral("0.5.3"));
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    const QStringList invalidUrls = {
        QStringLiteral("not a URL"), QStringLiteral("file:///tmp/manifest.json"),
        QStringLiteral("http://user:password@127.0.0.1/manifest.json"),
        QStringLiteral("https://example.invalid/manifest.json#fragment"),
        QStringLiteral("https://example.invalid/")};
    for (const auto &url : invalidUrls) {
        QVERIFY(!controller.setServer(url));
        QCOMPARE(controller.feedUrl(), m_fixture->manifestUrl());
        QSettings settings(m_fixture->settingsFile, QSettings::IniFormat);
        QCOMPARE(settings.value(QStringLiteral("update/server")).toString(), m_fixture->server->baseUrl());
    }
    QVERIFY(!controller.setServer(QStringLiteral("127.0.0.1"), 0));
    QVERIFY(!controller.setServer(QStringLiteral("127.0.0.1"), 65536));
    QCOMPARE(controller.feedUrl(), m_fixture->manifestUrl());
    QCOMPARE(m_fixture->server->requestCount(), 0);
    QVERIFY(controller.setServer(QString()));
    QCOMPARE(controller.state(), QStringLiteral("unconfigured"));
    QVERIFY(controller.lastError().isEmpty());
}

void ControllerTests::clearCancelsDeferredStartupCheck()
{
    m_fixture->prepareUpdate();
    bool hostBusy = true;
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    controller.setHostBusyProvider([&hostBusy] { return hostBusy; });
    controller.checkOnStartup();
    QVERIFY(controller.setServer(QString()));
    hostBusy = false;
    QTest::qWait(2200);
    QCOMPARE(m_fixture->server->requestCount(), 0);
    QVERIFY(!controller.busy());
    QCOMPARE(controller.state(), QStringLiteral("unconfigured"));
}

void ControllerTests::noUpdateIsQuiet()
{
    m_fixture->prepareUpdate(QStringLiteral("0.5.3"));
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    QSignalSpy notifications(&controller, &UpdateController::notificationRequested);
    QSignalSpy quits(&controller, &UpdateController::quitRequested);
    controller.checkOnStartup();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), QStringLiteral("uptodate"), 5000);
    QCOMPARE(notifications.count(), 0);
    QCOMPARE(quits.count(), 0);
    QVERIFY(controller.status().contains(QStringLiteral("已是最新")));
    QCOMPARE(controller.currentVersion(), QStringLiteral("0.5.3"));
}

void ControllerTests::nativeDownloadRejectsInvalidResponses_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("mime");
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<qint64>("expectedSize");
    QTest::newRow("non-2xx") << 404 << QByteArray("application/zip") << QByteArray("payload") << qint64(7);
    QTest::newRow("html") << 200 << QByteArray("text/html") << QByteArray("payload") << qint64(7);
    QTest::newRow("oversized") << 200 << QByteArray("application/zip") << QByteArray("payload") << qint64(2);
    QTest::newRow("truncated") << 200 << QByteArray("application/zip") << QByteArray("payload") << qint64(100);
    QTest::newRow("bad-sha") << 200 << QByteArray("application/zip") << QByteArray("payload") << qint64(7);
}

void ControllerTests::nativeDownloadRejectsInvalidResponses()
{
    QFETCH(int, status);
    QFETCH(QByteArray, mime);
    QFETCH(QByteArray, payload);
    QFETCH(qint64, expectedSize);
    m_fixture->server->setHandler([=](const QString &) {
        return zto_update_test::TestResponse{status, mime, payload};
    });
    const auto target = m_fixture->temporary.filePath(QStringLiteral("download.zip"));
    QString error;
    QVERIFY(zto_update_test::writeFile(target, "preserved", error));
    QProcess process;
    process.start(QStringLiteral(UPDATE_NATIVE_HELPER),
        {QStringLiteral("--download"), m_fixture->packageUrl(QStringLiteral("0.5.4")),
         QStringLiteral("--output"), target, QStringLiteral("--size"), QString::number(expectedSize),
         QStringLiteral("--sha256"), QString(64, QLatin1Char('0'))});
    QTRY_VERIFY_WITH_TIMEOUT(process.state() == QProcess::NotRunning, 10000);
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY(process.exitCode() != 0);
    QVERIFY(!process.readAllStandardError().isEmpty());
    QCOMPARE(zto_update_test::readFile(target), QByteArray("preserved"));
    QVERIFY(QDir(m_fixture->temporary.path()).entryList({QStringLiteral("*.part-*")}, QDir::Files).isEmpty());
}

void ControllerTests::installsVerifiedUpdate()
{
    m_fixture->prepareUpdate();
    QFile::remove(QDir(m_fixture->installDirectory).filePath(QStringLiteral("fake-helper-ran.txt")));
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    controller.setHelperExecutableForTesting(QStringLiteral(UPDATE_FAKE_HELPER));
    controller.setNoRestartForTesting(true);
    QSignalSpy notifications(&controller, &UpdateController::notificationRequested);
    QSignalSpy quits(&controller, &UpdateController::quitRequested);
    controller.checkOnStartup();
    QTRY_VERIFY_WITH_TIMEOUT(quits.count() == 1, 15000);
    QCOMPARE(controller.state(), QStringLiteral("preparing"));
    QVERIFY(controller.lastError().isEmpty());
    QVERIFY(controller.status().contains(QStringLiteral("退出")));
    QVERIFY(!notifications.isEmpty());

    // The plan written for the helper must be valid, reference the staged
    // hashes and never touch the live install tree before the helper runs.
    const QDir plans(QDir(m_fixture->storageRoot).filePath(QStringLiteral("updates/plan")));
    const auto planFiles = plans.entryList(QDir::Files);
    QCOMPARE(planFiles.size(), 1);
    QString readError;
    const auto planText = zto_update_test::readFile(plans.filePath(planFiles.first()));
    Q_UNUSED(readError)
    QByteArray asText(planText);
    updater::Plan plan;
    std::string planError;
    QVERIFY2(updater::parsePlan(asText.toStdString(), plan, planError), planError.c_str());
    QCOMPARE(QString::fromStdString(plan.packageVersion), QStringLiteral("0.5.4"));
    QCOMPARE(plan.files.size(), std::size_t(2));
    QCOMPARE(QString::fromStdString(plan.installDir),
             QDir::toNativeSeparators(m_fixture->installDirectory));
    for (const auto &file : plan.files) {
        const auto staged = QDir(QString::fromStdString(plan.stagingDir)).filePath(QString::fromStdString(file.path));
        QVERIFY2(QFileInfo::exists(staged), qPrintable(staged));
        QCOMPARE(QString::fromStdString(file.sha256), zto_update_test::sha256FileHex(staged));
    }
    // The helper was launched with the install directory as its working
    // directory; the test stub drops a marker there.
    QTRY_VERIFY_WITH_TIMEOUT(
        QFileInfo::exists(QDir(m_fixture->installDirectory).filePath(QStringLiteral("fake-helper-ran.txt"))),
        5000);
    QCOMPARE(m_fixture->server->requestAgentsFor(QStringLiteral("/updater")),
             QList<QByteArray>{QByteArrayLiteral("Z2-Updater/1")});
    QCOMPARE(m_fixture->server->requestAgentsFor(QStringLiteral("/file?version=0.5.4")),
             QList<QByteArray>{QByteArrayLiteral("Z2-Updater/2")});
    QCOMPARE(m_fixture->server->requestCount(), 2);
    // Nothing was replaced by the host itself.
    QCOMPARE(zto_update_test::readFile(m_fixture->pseudoExecutable), QByteArrayLiteral("old payload"));
}

void ControllerTests::rejectsBelowMinimumVersion()
{
    m_fixture->prepareUpdate(QStringLiteral("0.5.4"), QStringLiteral("windows"),
                             QStringLiteral("x64"), QStringLiteral("0.5.4"));
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    controller.setHelperExecutableForTesting(QStringLiteral(UPDATE_FAKE_HELPER));
    QSignalSpy quits(&controller, &UpdateController::quitRequested);
    controller.checkOnStartup();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), QStringLiteral("failed"), 5000);
    QVERIFY2(controller.lastError().contains(QStringLiteral("最低版本")), qPrintable(controller.lastError()));
    QCOMPARE(quits.count(), 0);
    QCOMPARE(m_fixture->server->requestCountFor(QStringLiteral("/file?version=0.5.4")), 0);
}

void ControllerTests::rejectsWrongPlatform()
{
    m_fixture->prepareUpdate(QStringLiteral("0.5.4"), QStringLiteral("linux"));
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    controller.checkOnStartup();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), QStringLiteral("failed"), 5000);
    QVERIFY(controller.lastError().contains(QStringLiteral("linux")));
}

void ControllerTests::rejectsBadSignature()
{
    m_fixture->prepareUpdate();
    m_fixture->signature = QByteArray(64, '\x22');
    m_fixture->serveUpdate(m_fixture->manifest, m_fixture->signature, m_fixture->package);
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    QSignalSpy quits(&controller, &UpdateController::quitRequested);
    controller.checkOnStartup();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), QStringLiteral("failed"), 5000);
    QVERIFY2(controller.lastError().contains(QStringLiteral("签名")), qPrintable(controller.lastError()));
    QCOMPARE(quits.count(), 0);
    QCOMPARE(m_fixture->server->requestCountFor(QStringLiteral("/file?version=0.5.4")), 0);
}

void ControllerTests::rejectsCorruptPackage()
{
    m_fixture->prepareUpdate();
    // Same manifest, different bytes from the server: the SHA-256 check must
    // stop the install before any staging or helper launch.
    m_fixture->serveUpdate(m_fixture->manifest, m_fixture->signature, QByteArrayLiteral("corrupted"));
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    QSignalSpy quits(&controller, &UpdateController::quitRequested);
    controller.checkOnStartup();
    QTRY_COMPARE_WITH_TIMEOUT(controller.state(), QStringLiteral("failed"), 5000);
    QVERIFY(controller.lastError().contains(QStringLiteral("SHA-256"))
            || controller.lastError().contains(QStringLiteral("大小")) || controller.lastError().contains(QStringLiteral("size")));
    QCOMPARE(quits.count(), 0);
}

void ControllerTests::guardsReentrancy()
{
    m_fixture->prepareUpdate();
    const auto base = m_fixture->server->baseUrl();
    m_fixture->server->setHandler([base](const QString &) {
        return zto_update_test::TestResponse{200, "application/json", "{}", {}, 0, true, false, -1, {}};
    });
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    QSignalSpy notifications(&controller, &UpdateController::notificationRequested);
    controller.checkManually();
    QCOMPARE(controller.state(), QStringLiteral("checking"));
    QVERIFY(controller.busy());
    QVERIFY(!controller.setServer(QString()));
    QCOMPARE(controller.feedUrl(), m_fixture->manifestUrl());
    QCOMPARE(controller.state(), QStringLiteral("checking"));
    notifications.clear();
    QTRY_COMPARE_WITH_TIMEOUT(m_fixture->server->requestCountFor(QStringLiteral("/updater")), 1, 5000);
    controller.checkManually();
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notifications.first().at(0).toString(), QStringLiteral("正在检查更新，请稍候"));
    QCOMPARE(m_fixture->server->requestCountFor(QStringLiteral("/updater")), 1);
    Q_UNUSED(base)
}

void ControllerTests::respectsHostBusy()
{
    m_fixture->prepareUpdate();
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    controller.setHostBusyProvider([] { return true; });
    QSignalSpy notifications(&controller, &UpdateController::notificationRequested);
    controller.checkManually();
    QCOMPARE(notifications.count(), 1);
    QVERIFY(notifications.first().at(0).toString().contains(QStringLiteral("其他操作")));
    QCOMPARE(m_fixture->server->requestCount(), 0);
    controller.checkOnStartup();
    QTest::qWait(200);
    QCOMPARE(m_fixture->server->requestCount(), 0);
}

void ControllerTests::surfacesInstalledResult()
{
    const auto updates = QDir(m_fixture->storageRoot).filePath(QStringLiteral("updates"));
    QDir().mkpath(updates);
    QString error;
    QVERIFY(zto_update_test::writeFile(QDir(updates).filePath(QStringLiteral("last-result.txt")),
                                       "result=ok\nstep=commit\nmessage=\nversion=0.5.4\n"
                                       "restart=started\ntime=2026-11-02 12:00:00\n",
                                       error));
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    QCOMPARE(controller.installedMessage(), QStringLiteral("已更新到 0.5.4"));
    QSignalSpy notifications(&controller, &UpdateController::notificationRequested);
    controller.checkOnStartup();
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notifications.first().at(0).toString(), QStringLiteral("已更新到 0.5.4"));
    QCOMPARE(notifications.first().at(1).toBool(), false);
    QVERIFY(!QFileInfo::exists(QDir(updates).filePath(QStringLiteral("last-result.txt"))));
}

void ControllerTests::surfacesFailedResult()
{
    const auto updates = QDir(m_fixture->storageRoot).filePath(QStringLiteral("updates"));
    QDir().mkpath(updates);
    QString error;
    QVERIFY(zto_update_test::writeFile(QDir(updates).filePath(QStringLiteral("last-result.txt")),
                                       "result=failed\nstep=replace\n"
                                       "message=cannot remove the old file: access denied\n"
                                       "version=0.5.4\nrestart=not-requested\n"
                                       "time=2026-11-02 12:00:00\n",
                                       error));
    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    QSignalSpy notifications(&controller, &UpdateController::notificationRequested);
    controller.checkOnStartup();
    QCOMPARE(controller.state(), QStringLiteral("failed"));
    QVERIFY(controller.lastError().contains(QStringLiteral("access denied")));
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notifications.first().at(1).toBool(), true);
}

void ControllerTests::recoversInterruptedTransaction()
{
    const auto updates = QDir(m_fixture->storageRoot).filePath(QStringLiteral("updates"));
    const auto transaction = QDir(updates).filePath(QStringLiteral("transaction/tx-interrupted"));
    const auto backup = QDir(transaction).filePath(QStringLiteral("backup"));
    QDir().mkpath(backup);
    QString error;
    QVERIFY(zto_update_test::writeFile(QDir(backup).filePath(QStringLiteral("Z2.exe")),
                                       "old payload", error));
    QVERIFY(zto_update_test::writeFile(QDir(m_fixture->installDirectory).filePath(QStringLiteral("Z2.exe")),
                                       "corrupted new payload", error));

    updater::Plan plan;
    plan.packageVersion = "0.5.4";
    plan.installDir = QDir::toNativeSeparators(m_fixture->installDirectory).toStdString();
    plan.stagingDir = QDir::toNativeSeparators(QDir(updates).filePath(QStringLiteral("staging/tx"))).toStdString();
    plan.transactionDir = QDir::toNativeSeparators(transaction).toStdString();
    plan.runtimeDir = QDir::toNativeSeparators(QDir(updates).filePath(QStringLiteral("runtime"))).toStdString();
    plan.logFile = QDir::toNativeSeparators(QDir(updates).filePath(QStringLiteral("log/updater.log"))).toStdString();
    plan.resultFile = QDir::toNativeSeparators(QDir(updates).filePath(QStringLiteral("last-result.txt"))).toStdString();
    plan.parentPid = 1234;
    plan.restartRelative = "Z2.exe";
    updater::PlanFile file;
    file.path = "Z2.exe";
    file.size = 9;
    file.sha256 = zto_update_test::sha256Hex("old payload").toStdString();
    plan.files.push_back(file);
    std::string planText;
    std::string planError;
    QVERIFY2(updater::serializePlan(plan, planText, planError), planError.c_str());
    QVERIFY(zto_update_test::writeFile(QDir(transaction).filePath(QStringLiteral("plan.txt")),
                                       QByteArray::fromStdString(planText), error));
    QVERIFY(zto_update_test::writeFile(QDir(transaction).filePath(QStringLiteral("journal.txt")),
                                       "started\nbackup|Z2.exe\ninstalled|Z2.exe\n", error));

    UpdateController controller(m_fixture->installDirectory, m_fixture->storageRoot,
                                m_fixture->settingsFile);
    controller.checkOnStartup();
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(transaction), 5000);
    QCOMPARE(zto_update_test::readFile(QDir(m_fixture->installDirectory).filePath(QStringLiteral("Z2.exe"))),
             QByteArrayLiteral("old payload"));
}

QTEST_GUILESS_MAIN(ControllerTests)
#include "ControllerTests.moc"
