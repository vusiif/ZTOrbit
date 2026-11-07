#pragma once

#include "UpdateManifest.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>


class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace vusiif::zto::update {

// HTTPS/HTTP transport for the update protocol.
//
// - The signed /updater response is fetched with a connect timeout (10 s) and an
//   idle timeout (30 s), a 1 MiB manifest cap and a 4 KiB signature cap.
// - Redirects are followed manually: at most five, and an HTTPS request may
//   never be downgraded to HTTP.
class UpdateHttp : public QObject {
    Q_OBJECT
public:
    static constexpr int DefaultConnectTimeoutMs = 10000;
    static constexpr int DefaultIdleTimeoutMs = 30000;
    static constexpr int MaximumRedirects = 5;
    static constexpr qint64 MaximumManifestSize = MaximumManifestBytes;
    static constexpr qint64 MaximumSignatureSize = 4 * 1024;

    explicit UpdateHttp(QObject *parent = nullptr);
    ~UpdateHttp() override;

    // Fetches /updater with its X-Z2-Signature header, verifies detached Ed25519
    // signature over the exact manifest bytes and selects the artifact for this
    // platform/architecture. Emits manifestFinished exactly once.
    void checkManifest(const QString &manifestUrl, const QByteArray &publicKey);

    void abort();

    bool busy() const;

    // Redirect policy helper, exposed for the isolated transport tests.
    static bool isRedirectAllowed(const QUrl &from, const QUrl &to);

    // Test hook: shorten the timers. Values <= 0 keep the defaults.
    void setTimeoutsForTesting(int connectMs, int idleMs);

signals:
    void manifestFinished(bool ok, const QByteArray &manifestBytes, const UpdateManifest &manifest,
                          const UpdateArtifact &artifact, const QString &error);

private:
    enum class Purpose { None, Manifest };

    void startGet(const QUrl &url);
    void handleReadyRead();
    void handleFinished();
    void handleRedirect(const QUrl &target);
    void fail(Purpose purpose, const QString &error);
    void finishManifest();
    void resetState();
    void armTimers(bool waitingForHeaders);
    void disarmTimers();
    bool acceptContentType(const QString &contentType) const;
    bool consumeData(const QByteArray &data);

    QNetworkAccessManager *m_manager = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_connectTimer = nullptr;
    QTimer *m_idleTimer = nullptr;
    Purpose m_purpose = Purpose::None;
    QByteArray m_manifestBytes;
    QByteArray m_signatureBytes;
    QByteArray m_publicKey;
    QUrl m_currentUrl;
    int m_redirects = 0;
    bool m_awaitingHeaders = false;


    int m_connectTimeoutMs = DefaultConnectTimeoutMs;
    int m_idleTimeoutMs = DefaultIdleTimeoutMs;
};

} // namespace vusiif::zto::update
