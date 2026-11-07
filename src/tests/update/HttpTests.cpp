#include "TestSupport.h"
#include "UpdateCrypto.h"
#include "UpdateHttp.h"
#include "UpdateManifest.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include <memory>
#include <QtTest>

using namespace vusiif::zto::update;

namespace {

QByteArray buildManifest(const QString &version, const QString &platform, const QString &arch,
                         qint64 size, const QString &sha256, const QString &url,
                         const QString &minimum = QStringLiteral("0.5.0"))
{
    QJsonObject artifact;
    artifact.insert(QStringLiteral("platform"), platform);
    artifact.insert(QStringLiteral("arch"), arch);
    artifact.insert(QStringLiteral("version"), version);
    artifact.insert(QStringLiteral("url"), url);
    artifact.insert(QStringLiteral("size"), size);
    artifact.insert(QStringLiteral("sha256"), sha256);
    QJsonObject document;
    document.insert(QStringLiteral("schemaVersion"), 1);
    document.insert(QStringLiteral("channel"), QStringLiteral("stable"));
    document.insert(QStringLiteral("publishedAt"), QStringLiteral("2026-11-02T12:00:00Z"));
    document.insert(QStringLiteral("minUpdatableVersion"), minimum);
    document.insert(QStringLiteral("artifacts"), QJsonArray{artifact});
    return QJsonDocument(document).toJson(QJsonDocument::Indented);
}

QString toBase64(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toBase64());
}

} // namespace

class HttpTests : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();
    void checkManifestSucceeds();
    void rejectsBadSignature();
    void rejectsMissingSignature();
    void rejectsWrongPlatform();
    void followsRedirect();
    void rejectsTooManyRedirects();
    void redirectPolicy();
    void rejectsNon2xx();
    void rejectsWrongContentType();
    void rejectsOversizedManifest();
    void connectTimeout();
    void idleTimeout();
    void isBusyWhileChecking();

private:
    struct ManifestResult {
        bool done = false;
        bool ok = false;
        UpdateManifest manifest;
        UpdateArtifact artifact;
        QByteArray bytes;
        QString error;
    };
    std::shared_ptr<ManifestResult> check(const QString &path);

    zto_update_test::TestHttpServer *m_server = nullptr;
    UpdateHttp *m_http = nullptr;
    zto_update_test::TestKeyPair m_keys;
    QByteArray m_package;
    QByteArray m_manifest;
    QByteArray m_signature;
};

void HttpTests::init()
{
    QString error;
    m_keys = zto_update_test::TestKeyPair::load(QStringLiteral(UPDATE_FIXTURES_DIR), error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    m_server = new zto_update_test::TestHttpServer(this);
    QVERIFY(m_server->start());
    m_http = new UpdateHttp(this);
    m_http->setTimeoutsForTesting(2000, 2000);
    m_package = QByteArrayLiteral("PK\x03\x04 fake package payload for transport tests");
}

void HttpTests::cleanup()
{
    delete m_http;
    m_http = nullptr;
    delete m_server;
    m_server = nullptr;
}

std::shared_ptr<HttpTests::ManifestResult> HttpTests::check(const QString &path)
{
    auto result = std::make_shared<ManifestResult>();
    connect(m_http, &UpdateHttp::manifestFinished, this,
            [result](bool ok, const QByteArray &bytes, const UpdateManifest &manifest,
                     const UpdateArtifact &artifact, const QString &error) {
                result->done = true;
                result->ok = ok;
                result->bytes = bytes;
                result->manifest = manifest;
                result->artifact = artifact;
                result->error = error;
            });
    m_http->checkManifest(path, m_keys.publicKey);
    return result;
}

void HttpTests::checkManifestSucceeds()
{
    m_manifest = buildManifest(QStringLiteral("0.5.4"), QStringLiteral("windows"),
                               QStringLiteral("x64"), m_package.size(),
                               zto_update_test::sha256Hex(m_package),
                               m_server->baseUrl() + QStringLiteral("/package.zip"));
    {
        QString signError;
        m_signature = zto_update_test::signDetached(m_keys.seed, m_manifest, signError);
        QVERIFY2(signError.isEmpty(), qPrintable(signError));
    }
    const auto manifestUrl = m_server->baseUrl() + QStringLiteral("/manifest.json");
    m_server->setHandler([this](const QString &path) {
        if (path == QLatin1String("/manifest.json"))
            return zto_update_test::TestResponse{200, "application/json", m_manifest, {}, 0, false, false, -1, {{"X-Z2-Signature", m_signature.toBase64()}}};
        return zto_update_test::TestResponse{404, "text/plain", "not found", {}, 0, false, false, -1, {}};
    });
    auto result = check(manifestUrl);
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY2(result->ok, qPrintable(result->error));
    QCOMPARE(result->manifest.artifacts.size(), 1);
    QCOMPARE(result->artifact.version, QStringLiteral("0.5.4"));
    QCOMPARE(result->bytes, m_manifest);
}

void HttpTests::rejectsBadSignature()
{
    m_manifest = buildManifest(QStringLiteral("0.5.4"), QStringLiteral("windows"),
                               QStringLiteral("x64"), m_package.size(),
                               zto_update_test::sha256Hex(m_package),
                               m_server->baseUrl() + QStringLiteral("/package.zip"));
    QByteArray signature(64, '\x11');
    const auto manifestUrl = m_server->baseUrl() + QStringLiteral("/manifest.json");
    m_server->setHandler([this, signature](const QString &path) {
        if (path == QLatin1String("/manifest.json"))
            return zto_update_test::TestResponse{200, "application/json", m_manifest, {}, 0, false, false, -1, {{"X-Z2-Signature", signature.toBase64()}}};
        return zto_update_test::TestResponse{404, "text/plain", "not found", {}, 0, false, false, -1, {}};
    });
    auto result = check(manifestUrl);
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY(!result->ok);
    QVERIFY2(result->error.contains(QStringLiteral("签名")), qPrintable(result->error));
}

void HttpTests::rejectsWrongPlatform()
{
    m_manifest = buildManifest(QStringLiteral("0.5.4"), QStringLiteral("linux"),
                               QStringLiteral("x64"), m_package.size(),
                               zto_update_test::sha256Hex(m_package),
                               m_server->baseUrl() + QStringLiteral("/package.zip"));
    QString signError;
    m_signature = zto_update_test::signDetached(m_keys.seed, m_manifest, signError);
    const auto manifestUrl = m_server->baseUrl() + QStringLiteral("/manifest.json");
    m_server->setHandler([this](const QString &path) {
        if (path == QLatin1String("/manifest.json"))
            return zto_update_test::TestResponse{200, "application/json", m_manifest, {}, 0, false, false, -1, {{"X-Z2-Signature", m_signature.toBase64()}}};
        return zto_update_test::TestResponse{404, "text/plain", "not found", {}, 0, false, false, -1, {}};
    });
    auto result = check(manifestUrl);
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY(!result->ok);
    QVERIFY2(result->error.contains(QStringLiteral("linux")), qPrintable(result->error));
}

void HttpTests::rejectsMissingSignature()
{
    m_server->setHandler([](const QString &) {
        return zto_update_test::TestResponse{200, "application/json", "{}"};
    });
    const auto result = check(m_server->baseUrl() + QStringLiteral("/updater"));
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY(!result->ok);
    QVERIFY(result->error.contains(QStringLiteral("签名")));
    QCOMPARE(m_server->requestCount(), 1);
}

void HttpTests::followsRedirect()
{
    m_manifest = buildManifest(QStringLiteral("0.5.4"), QStringLiteral("windows"),
                               QStringLiteral("x64"), m_package.size(),
                               zto_update_test::sha256Hex(m_package),
                               m_server->baseUrl() + QStringLiteral("/package.zip"));
    QString signError;
    m_signature = zto_update_test::signDetached(m_keys.seed, m_manifest, signError);
    m_server->setHandler([this](const QString &path) {
        if (path == QLatin1String("/manifest.json"))
            return zto_update_test::TestResponse{302, "text/plain", {}, QStringLiteral("/real/manifest.json"),
                                0, false, false, -1, {}};
        if (path == QLatin1String("/real/manifest.json"))
            return zto_update_test::TestResponse{200, "application/json", m_manifest, {}, 0, false, false, -1, {{"X-Z2-Signature", m_signature.toBase64()}}};
        return zto_update_test::TestResponse{404, "text/plain", "not found", {}, 0, false, false, -1, {}};
    });
    auto result = check(m_server->baseUrl() + QStringLiteral("/manifest.json"));
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY2(result->ok, qPrintable(result->error));
    QCOMPARE(m_server->requestCountFor(QStringLiteral("/real/manifest.json")), 1);
}

void HttpTests::rejectsTooManyRedirects()
{
    m_server->setHandler([](const QString &path) {
        return zto_update_test::TestResponse{302, "text/plain", {}, path, 0, false, false, -1, {}};
    });
    auto result = check(m_server->baseUrl() + QStringLiteral("/manifest.json"));
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY(!result->ok);
    QVERIFY2(result->error.contains(QStringLiteral("重定向")), qPrintable(result->error));
    QVERIFY(m_server->requestCount() <= UpdateHttp::MaximumRedirects + 1);
}

void HttpTests::redirectPolicy()
{
    const QUrl https(QStringLiteral("https://example.invalid/a.json"));
    const QUrl http(QStringLiteral("http://example.invalid/a.json"));
    QVERIFY(UpdateHttp::isRedirectAllowed(https, https));
    QVERIFY(UpdateHttp::isRedirectAllowed(http, https));
    QVERIFY(UpdateHttp::isRedirectAllowed(http, http));
    QVERIFY(!UpdateHttp::isRedirectAllowed(https, http));
    QVERIFY(!UpdateHttp::isRedirectAllowed(https, QUrl(QStringLiteral("ftp://example.invalid/a"))));
    QVERIFY(!UpdateHttp::isRedirectAllowed(https, QUrl()));
}

void HttpTests::rejectsNon2xx()
{
    m_server->setHandler([](const QString &) {
        return zto_update_test::TestResponse{404, "text/plain", "missing", {}, 0, false, false, -1, {}};
    });
    auto result = check(m_server->baseUrl() + QStringLiteral("/manifest.json"));
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY(!result->ok);
    QVERIFY2(result->error.contains(QStringLiteral("404")), qPrintable(result->error));
}

void HttpTests::rejectsWrongContentType()
{
    m_manifest = buildManifest(QStringLiteral("0.5.4"), QStringLiteral("windows"),
                               QStringLiteral("x64"), m_package.size(),
                               zto_update_test::sha256Hex(m_package),
                               m_server->baseUrl() + QStringLiteral("/package.zip"));
    QString signError;
    m_signature = zto_update_test::signDetached(m_keys.seed, m_manifest, signError);
    m_server->setHandler([this](const QString &path) {
        if (path == QLatin1String("/manifest.json"))
            return zto_update_test::TestResponse{200, "text/html", m_manifest, {}, 0, false, false, -1, {{"X-Z2-Signature", m_signature.toBase64()}}};
        return zto_update_test::TestResponse{200, "text/plain", m_signature.toBase64(), {}, 0, false, false, -1, {}};
    });
    auto result = check(m_server->baseUrl() + QStringLiteral("/manifest.json"));
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY(!result->ok);
    QVERIFY2(result->error.contains(QStringLiteral("内容类型")), qPrintable(result->error));
}

void HttpTests::rejectsOversizedManifest()
{
    m_manifest = QByteArray(UpdateHttp::MaximumManifestSize + 16, 'x');
    m_server->setHandler([this](const QString &) {
        return zto_update_test::TestResponse{200, "application/json", m_manifest, {}, 0, false, false, -1, {{"X-Z2-Signature", m_signature.toBase64()}}};
    });
    auto result = check(m_server->baseUrl() + QStringLiteral("/manifest.json"));
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY(!result->ok);
    QVERIFY2(result->error.contains(QStringLiteral("大小")), qPrintable(result->error));
}

void HttpTests::connectTimeout()
{
    m_http->setTimeoutsForTesting(300, 300);
    m_server->setHandler([](const QString &) {
        return zto_update_test::TestResponse{200, "application/json", {}, {}, 0, true, false, -1, {}};
    });
    auto result = check(m_server->baseUrl() + QStringLiteral("/manifest.json"));
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY(!result->ok);
    QVERIFY2(result->error.contains(QStringLiteral("超时")), qPrintable(result->error));
}

void HttpTests::idleTimeout()
{
    m_http->setTimeoutsForTesting(2000, 300);
    m_server->setHandler([](const QString &) {
        return zto_update_test::TestResponse{200, "application/json", "{\"x\":", {}, 0, false, true, -1, {}};
    });
    auto result = check(m_server->baseUrl() + QStringLiteral("/manifest.json"));
    QTRY_VERIFY_WITH_TIMEOUT(result->done, 5000);
    QVERIFY(!result->ok);
    QVERIFY2(result->error.contains(QStringLiteral("超时")), qPrintable(result->error));
}

void HttpTests::isBusyWhileChecking()
{
    m_server->setHandler([](const QString &) {
        return zto_update_test::TestResponse{200, "application/json", "{}", {}, 0, true, false, -1, {}};
    });
    m_http->setTimeoutsForTesting(5000, 5000);
    auto result = check(m_server->baseUrl() + QStringLiteral("/manifest.json"));
    QVERIFY(m_http->busy());
    m_http->abort();
    QVERIFY(!m_http->busy());
    QTest::qWait(100);
    QVERIFY(!result->done);
}

QTEST_GUILESS_MAIN(HttpTests)
#include "HttpTests.moc"
