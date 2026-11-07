#include "UpdateHttp.h"

#include "UpdateCrypto.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>

namespace vusiif::zto::update {
namespace {

QString replyError(QNetworkReply *reply)
{
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status > 0) {
        return QStringLiteral("服务器返回 HTTP %1").arg(status);
    }
    return QStringLiteral("网络请求失败：%1").arg(reply->errorString());
}

bool isRedirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

} // namespace

UpdateHttp::UpdateHttp(QObject *parent) : QObject(parent)
{
    m_manager = new QNetworkAccessManager(this);
    m_manager->setAutoDeleteReplies(false);
    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, [this] {
        if (!m_reply || m_purpose == Purpose::None) return;
        auto *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        const auto purpose = m_purpose;
        fail(purpose, QStringLiteral("连接更新服务器超时（%1 秒）").arg(m_connectTimeoutMs / 1000.0));
    });
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    connect(m_idleTimer, &QTimer::timeout, this, [this] {
        if (!m_reply || m_purpose == Purpose::None) return;
        auto *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        const auto purpose = m_purpose;
        fail(purpose, QStringLiteral("下载超时：连接空闲超过 %1 秒").arg(m_idleTimeoutMs / 1000.0));
    });
}

UpdateHttp::~UpdateHttp()
{
    abort();
}

bool UpdateHttp::busy() const
{
    return m_purpose != Purpose::None;
}

bool UpdateHttp::isRedirectAllowed(const QUrl &from, const QUrl &to)
{
    if (!to.isValid() || to.host().isEmpty()) return false;
    if (to.scheme() != QLatin1String("https") && to.scheme() != QLatin1String("http")) return false;
    if (from.scheme() == QLatin1String("https") && to.scheme() == QLatin1String("http")) return false;
    return true;
}

void UpdateHttp::setTimeoutsForTesting(int connectMs, int idleMs)
{
    if (connectMs > 0) m_connectTimeoutMs = connectMs;
    if (idleMs > 0) m_idleTimeoutMs = idleMs;
}

void UpdateHttp::abort()
{
    disarmTimers();
    if (m_reply) {
        auto *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    resetState();
}

void UpdateHttp::resetState()
{
    m_purpose = Purpose::None;
    m_redirects = 0;
    m_awaitingHeaders = false;
    m_manifestBytes.clear();
    m_signatureBytes.clear();
    m_publicKey.clear();
    m_currentUrl = QUrl();
}

void UpdateHttp::armTimers(bool waitingForHeaders)
{
    disarmTimers();
    m_awaitingHeaders = waitingForHeaders;
    if (waitingForHeaders) m_connectTimer->start(m_connectTimeoutMs);
    m_idleTimer->start(m_idleTimeoutMs);
}

void UpdateHttp::disarmTimers()
{
    m_connectTimer->stop();
    m_idleTimer->stop();
    m_awaitingHeaders = false;
}

bool UpdateHttp::acceptContentType(const QString &contentType) const
{
    QString type = contentType;
    const auto semicolon = type.indexOf(QLatin1Char(';'));
    if (semicolon >= 0) type = type.left(semicolon);
    type = type.trimmed().toLower();
    if (type.isEmpty()) return true; // Some static hosts omit the header.
    return type == QLatin1String("application/json") || type == QLatin1String("text/json")
        || type == QLatin1String("application/octet-stream") || type == QLatin1String("text/plain");
}

void UpdateHttp::checkManifest(const QString &manifestUrl, const QByteArray &publicKey)
{
    abort();
    m_publicKey = publicKey;
    m_purpose = Purpose::Manifest;
    startGet(QUrl(manifestUrl));
    armTimers(true);
}

void UpdateHttp::startGet(const QUrl &url)
{
    m_currentUrl = url;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QVariant::fromValue(QNetworkRequest::ManualRedirectPolicy));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QVariant::fromValue(QNetworkRequest::AlwaysNetwork));
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Z2-Updater/1"));
    m_reply = m_manager->get(request);
    m_reply->setReadBufferSize(256 * 1024);
    connect(m_reply, &QNetworkReply::readyRead, this, &UpdateHttp::handleReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateHttp::handleFinished);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, [this] {
        if (!m_reply || !m_awaitingHeaders) return;
        m_awaitingHeaders = false;
        m_connectTimer->stop();
    });
}

void UpdateHttp::handleReadyRead()
{
    if (!m_reply || m_purpose == Purpose::None) return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (isRedirect(status)) {
        m_reply->readAll();
        return;
    }
    m_idleTimer->start(m_idleTimeoutMs);
    consumeData(m_reply->readAll());
}

bool UpdateHttp::consumeData(const QByteArray &data)
{
    if (data.isEmpty()) return true;
    switch (m_purpose) {
    case Purpose::Manifest: {
        auto &buffer = m_manifestBytes;
        const qint64 limit = MaximumManifestSize;
        if (buffer.size() + data.size() > limit) {
            fail(m_purpose, QStringLiteral("服务器响应超过允许的大小"));
            return false;
        }
        buffer.append(data);
        return true;
    }
    case Purpose::None:
        return true;
    }
    return true;
}

void UpdateHttp::handleFinished()
{
    if (!m_reply || m_purpose == Purpose::None) return;
    auto *reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    disarmTimers();

    const auto purpose = m_purpose;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto redirectTarget = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const auto replyUrl = reply->url();
    const auto networkError = reply->error();
    const auto errorText = reply->errorString();
    const auto remaining = reply->readAll();
    const auto contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    const auto signatureHeader = reply->rawHeader("X-Z2-Signature");
    reply->deleteLater();

    if (isRedirect(status) && redirectTarget.isValid()) {
        handleRedirect(replyUrl.resolved(redirectTarget));
        return;
    }
    if (networkError != QNetworkReply::NoError && status == 0) {
        fail(purpose, QStringLiteral("网络请求失败：%1").arg(errorText));
        return;
    }
    if (!consumeData(remaining)) return;
    if (status < 200 || status >= 300) {
        fail(purpose, status > 0 ? QStringLiteral("服务器返回 HTTP %1").arg(status)
                                 : QStringLiteral("网络请求失败：%1").arg(errorText));
        return;
    }
    if (!acceptContentType(contentType)) {
        fail(purpose, QStringLiteral("服务器返回了不支持的内容类型：%1").arg(contentType));
        return;
    }
    if (purpose == Purpose::Manifest) {
        if (m_manifestBytes.isEmpty()) {
            fail(purpose, QStringLiteral("更新清单为空"));
            return;
        }
        if (signatureHeader.isEmpty() || signatureHeader.size() > MaximumSignatureSize) {
            fail(purpose, QStringLiteral("更新响应缺少有效签名"));
            return;
        }
        m_signatureBytes = signatureHeader;
        finishManifest();
        return;
    }
}

void UpdateHttp::handleRedirect(const QUrl &target)
{
    if (m_purpose == Purpose::None) return;
    if (++m_redirects > MaximumRedirects) {
        fail(m_purpose, QStringLiteral("更新服务器重定向次数过多"));
        return;
    }
    if (!isRedirectAllowed(m_currentUrl, target)) {
        fail(m_purpose, QStringLiteral("拒绝无效或降级到 HTTP 的重定向地址"));
        return;
    }
    startGet(target);
    armTimers(true);
}

void UpdateHttp::fail(Purpose purpose, const QString &error)
{
    if (purpose == Purpose::None) return;
    if (m_reply) {
        auto *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    disarmTimers();
    switch (purpose) {
    case Purpose::Manifest: {
        resetState();
        emit manifestFinished(false, {}, {}, {}, error);
        break;
    }
    case Purpose::None:
        break;
    }
}

void UpdateHttp::finishManifest()
{
    QByteArray signature;
    QString error;
    if (!UpdateCrypto::decodeSignature(m_signatureBytes, signature, error)) {
        fail(Purpose::Manifest, error);
        return;
    }
    if (!UpdateCrypto::verifyDetached(m_manifestBytes, signature, m_publicKey, error)) {
        fail(Purpose::Manifest, error);
        return;
    }
    UpdateManifest manifest;
    if (!parseManifest(m_manifestBytes, manifest, error)) {
        fail(Purpose::Manifest, error);
        return;
    }
    UpdateArtifact artifact;
    if (!selectArtifact(manifest, targetPlatform(), targetArchitecture(), artifact, error)) {
        fail(Purpose::Manifest, error);
        return;
    }
    const auto bytesForCaller = m_manifestBytes;
    resetState();
    emit manifestFinished(true, bytesForCaller, manifest, artifact, QString());
}


} // namespace vusiif::zto::update
