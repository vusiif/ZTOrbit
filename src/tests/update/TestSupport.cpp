#include "TestSupport.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QLibrary>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <algorithm>

namespace zto_update_test {
namespace {

quint32 crc32(const QByteArray &bytes)
{
    static quint32 table[256];
    static bool ready = false;
    if (!ready) {
        for (quint32 index = 0; index < 256; ++index) {
            quint32 value = index;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1) ? (0xedb88320u ^ (value >> 1)) : (value >> 1);
            table[index] = value;
        }
        ready = true;
    }
    quint32 crc = 0xffffffffu;
    for (const char raw : bytes) {
        crc = table[(crc ^ static_cast<unsigned char>(raw)) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

void append16(QByteArray &out, quint16 value)
{
    char buffer[2];
    qToLittleEndian(value, buffer);
    out.append(buffer, 2);
}

void append32(QByteArray &out, quint32 value)
{
    char buffer[4];
    qToLittleEndian(value, buffer);
    out.append(buffer, 4);
}

const char *reasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default: return "Status";
    }
}

using SeedKeypair = int (*)(unsigned char *, unsigned char *, const unsigned char *);
using SignDetached = int (*)(unsigned char *, unsigned long long *, const unsigned char *,
                             unsigned long long, const unsigned char *);

struct Sodium {
    bool ok = false;
    QString error;
    SeedKeypair keypair = nullptr;
    SignDetached sign = nullptr;
};

Sodium &sodium()
{
    static Sodium instance;
    static bool tried = false;
    if (tried) return instance;
    tried = true;
    QLibrary library(QStringLiteral("libsodium"));
    if (!library.load()) {
        instance.error = QStringLiteral("cannot load libsodium for the test signer");
        return instance;
    }
    instance.keypair = reinterpret_cast<SeedKeypair>(library.resolve("crypto_sign_seed_keypair"));
    instance.sign = reinterpret_cast<SignDetached>(library.resolve("crypto_sign_detached"));
    if (!instance.keypair || !instance.sign) {
        instance.error = QStringLiteral("libsodium is missing the Ed25519 symbols");
        return instance;
    }
    instance.ok = true;
    return instance;
}

} // namespace

QByteArray buildZip(const QList<ZipEntrySpec> &entries)
{
    QByteArray output;
    QList<qint64> offsets;
    for (const auto &entry : entries) {
        offsets.append(output.size());
        const auto data = entry.directory ? QByteArray() : entry.data;
        const auto crc = entry.crc >= 0 ? static_cast<quint32>(entry.crc) : crc32(data);
        const auto compressed = entry.compressedSize >= 0
            ? static_cast<quint32>(entry.compressedSize) : static_cast<quint32>(data.size());
        const auto uncompressed = entry.uncompressedSize >= 0
            ? static_cast<quint32>(entry.uncompressedSize) : static_cast<quint32>(data.size());
        append32(output, 0x04034b50);
        append16(output, 20);
        append16(output, entry.flags);
        append16(output, entry.method);
        append16(output, 0);
        append16(output, 0);
        append32(output, crc);
        append32(output, compressed);
        append32(output, uncompressed);
        append16(output, static_cast<quint16>(entry.name.size()));
        append16(output, 0);
        output.append(entry.name);
        if (!entry.directory) output.append(data);
    }
    const auto centralOffset = output.size();
    for (int index = 0; index < entries.size(); ++index) {
        const auto &entry = entries.at(index);
        const auto data = entry.directory ? QByteArray() : entry.data;
        const auto crc = entry.crc >= 0 ? static_cast<quint32>(entry.crc) : crc32(data);
        const auto compressed = entry.compressedSize >= 0
            ? static_cast<quint32>(entry.compressedSize) : static_cast<quint32>(data.size());
        const auto uncompressed = entry.uncompressedSize >= 0
            ? static_cast<quint32>(entry.uncompressedSize) : static_cast<quint32>(data.size());
        const auto offset = entry.localHeaderOffset >= 0
            ? static_cast<quint32>(entry.localHeaderOffset)
            : static_cast<quint32>(offsets.at(index));
        append32(output, 0x02014b50);
        append16(output, entry.versionMadeBy);
        append16(output, 20);
        append16(output, entry.flags);
        append16(output, entry.method);
        append16(output, 0);
        append16(output, 0);
        append32(output, crc);
        append32(output, compressed);
        append32(output, uncompressed);
        append16(output, static_cast<quint16>(entry.name.size()));
        append16(output, 0);
        append16(output, 0);
        append16(output, 0);
        append16(output, 0);
        append32(output, entry.externalAttributes
                             | (entry.directory ? 0x10u : 0u));
        append32(output, offset);
        output.append(entry.name);
    }
    const auto centralSize = output.size() - centralOffset;
    append32(output, 0x06054b50);
    append16(output, 0);
    append16(output, 0);
    append16(output, static_cast<quint16>(entries.size()));
    append16(output, static_cast<quint16>(entries.size()));
    append32(output, static_cast<quint32>(centralSize));
    append32(output, static_cast<quint32>(centralOffset));
    append16(output, 0);
    return output;
}

TestHttpServer::TestHttpServer(QObject *parent) : QTcpServer(parent) {}

bool TestHttpServer::start()
{
    return listen(QHostAddress::LocalHost, 0);
}

QString TestHttpServer::baseUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
}

void TestHttpServer::incomingConnection(qintptr descriptor)
{
    auto *socket = new QTcpSocket(this);
    if (!socket->setSocketDescriptor(descriptor)) {
        socket->deleteLater();
        return;
    }
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { handleSocket(socket); });
    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
}

void TestHttpServer::handleSocket(QTcpSocket *socket)
{
    auto buffer = socket->property("ztoRequestBuffer").toByteArray();
    buffer += socket->readAll();
    socket->setProperty("ztoRequestBuffer", buffer);
    const auto end = buffer.indexOf("\r\n\r\n");
    if (end < 0) return;
    const auto requestLine = buffer.left(buffer.indexOf("\r\n"));
    const auto parts = QString::fromLatin1(requestLine).split(QLatin1Char(' '));
    if (parts.size() < 2) {
        socket->disconnectFromHost();
        return;
    }
    const auto path = parts.at(1);
    ++m_requestCount;
    ++m_pathCounts[path];
    for (const auto &line : buffer.left(end).split('\n')) {
        if (line.toLower().startsWith("user-agent:")) m_agents[path].append(line.mid(11).trimmed());
    }
    const TestResponse response = m_handler ? m_handler(path) : TestResponse{404, "text/plain", "missing"};
    if (response.stallBeforeHeaders) return;

    QByteArray header;
    header += "HTTP/1.1 " + QByteArray::number(response.status) + " "
        + reasonPhrase(response.status) + "\r\n";
    if (!response.contentType.isEmpty())
        header += "Content-Type: " + response.contentType + "\r\n";
    header += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
    header += "Connection: close\r\n";
    if (!response.location.isEmpty())
        header += "Location: " + response.location.toLatin1() + "\r\n";
    for (const auto &pair : response.headers)
        header += pair.first + ": " + pair.second + "\r\n";
    header += "\r\n";
    socket->write(header);
    socket->flush();
    if (response.stallAfterHeaders) return;

    auto send = [socket, response] {
        if (!socket->isOpen()) return;
        int count = response.body.size();
        if (response.sendBytes >= 0) count = std::min(count, response.sendBytes);
        if (count > 0) socket->write(response.body.left(count));
        socket->flush();
        socket->disconnectFromHost();
    };
    if (response.delayMs > 0) QTimer::singleShot(response.delayMs, socket, send);
    else send();
}

TestKeyPair TestKeyPair::load(const QString &fixturesDirectory, QString &error)
{
    TestKeyPair pair;
    const QDir directory(fixturesDirectory);
    const auto seedText = readFile(directory.filePath(QStringLiteral("keys/update-signing.key"))).trimmed();
    const auto publicText = readFile(directory.filePath(QStringLiteral("keys/update-signing.pub"))).trimmed();
    if (seedText.isEmpty() || publicText.isEmpty()) {
        error = QStringLiteral("cannot read the test key pair");
        return pair;
    }
    pair.seed = QByteArray::fromBase64(seedText);
    pair.publicKey = QByteArray::fromBase64(publicText);
    if (pair.seed.size() != 32 || pair.publicKey.size() != 32) {
        error = QStringLiteral("invalid test key pair");
        pair.seed.clear();
        pair.publicKey.clear();
    }
    return pair;
}

QByteArray signDetached(const QByteArray &seed, const QByteArray &message, QString &error)
{
    auto &backend = sodium();
    if (!backend.ok) {
        error = backend.error;
        return {};
    }
    unsigned char publicKey[32] = {};
    unsigned char secretKey[64] = {};
    if (backend.keypair(publicKey, secretKey,
                        reinterpret_cast<const unsigned char *>(seed.constData())) != 0) {
        error = QStringLiteral("crypto_sign_seed_keypair failed");
        return {};
    }
    unsigned char signature[64] = {};
    unsigned long long length = 0;
    if (backend.sign(signature, &length,
                     reinterpret_cast<const unsigned char *>(message.constData()),
                     static_cast<unsigned long long>(message.size()), secretKey) != 0) {
        error = QStringLiteral("crypto_sign_detached failed");
        return {};
    }
    return QByteArray(reinterpret_cast<const char *>(signature), 64);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &bytes, QString &error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = file.errorString();
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        error = file.errorString();
        return false;
    }
    file.close();
    return true;
}

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString sha256FileHex(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool writeTree(const QString &root, const QHash<QString, QByteArray> &files, QString &error)
{
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (!writeFile(QDir(root).filePath(it.key()), it.value(), error)) return false;
    }
    return true;
}

} // namespace zto_update_test
