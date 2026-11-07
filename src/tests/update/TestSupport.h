#pragma once

// Shared helpers for the isolated update tests. Everything here works on
// temporary directories and a loopback HTTP server; no real deployment and no
// real installation is ever touched.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QTcpServer>

#include <functional>

class QTcpSocket;
class QTimer;

namespace zto_update_test {

struct ZipEntrySpec {
    QByteArray name;
    QByteArray data;
    bool directory = false;
    quint16 flags = 0;
    quint16 method = 0; // 0 = stored; 8 requires pre-compressed data
    qint64 crc = -1;    // -1 = compute from data
    qint64 compressedSize = -1;
    qint64 uncompressedSize = -1;
    quint16 versionMadeBy = 0x031e; // Unix 3.0
    quint32 externalAttributes = 0;
    qint64 localHeaderOffset = -1;
};

QByteArray buildZip(const QList<ZipEntrySpec> &entries);

struct TestResponse {
    int status = 200;
    QByteArray contentType = "application/octet-stream";
    QByteArray body;
    QString location;
    int delayMs = 0;
    bool stallBeforeHeaders = false;
    bool stallAfterHeaders = false;
    int sendBytes = -1; // body bytes actually sent (Content-Length keeps full size)
    QList<QPair<QByteArray, QByteArray>> headers;
};

class TestHttpServer : public QTcpServer {
public:
    using Handler = std::function<TestResponse(const QString &path)>;
    explicit TestHttpServer(QObject *parent = nullptr);
    bool start();
    QString baseUrl() const;
    void setHandler(Handler handler) { m_handler = std::move(handler); }
    int requestCount() const { return m_requestCount; }
    int requestCountFor(const QString &path) const { return m_pathCounts.value(path); }
    QList<QByteArray> requestAgentsFor(const QString &path) const { return m_agents.value(path); }

protected:
    void incomingConnection(qintptr descriptor) override;

private:
    void handleSocket(QTcpSocket *socket);
    Handler m_handler;
    int m_requestCount = 0;
    QHash<QString, int> m_pathCounts;
    QHash<QString, QList<QByteArray>> m_agents;
};

struct TestKeyPair {
    QByteArray seed;
    QByteArray publicKey;
    static TestKeyPair load(const QString &fixturesDirectory, QString &error);
};

// Detached Ed25519 signature produced with the throwaway test key. libsodium is
// loaded through QLibrary exactly like UpdateCrypto does.
QByteArray signDetached(const QByteArray &seed, const QByteArray &message, QString &error);

QByteArray readFile(const QString &path);
bool writeFile(const QString &path, const QByteArray &bytes, QString &error);
QString sha256Hex(const QByteArray &bytes);
QString sha256FileHex(const QString &path);

// Creates a temporary install tree below `root` with the given relative files.
bool writeTree(const QString &root, const QHash<QString, QByteArray> &files, QString &error);

} // namespace zto_update_test
