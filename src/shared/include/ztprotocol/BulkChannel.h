#pragma once
#include <ztprotocol/ServiceWire.h>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QDateTime>
#include <QUuid>
#include <functional>
#include <memory>
#include <QPointer>

namespace vusiif::zto::protocol::service {
// One authenticated channel per lease. Large messages are bounded and fragmented;
// no application/media types are part of this transport.
class BulkChannel final : public QObject {
  public:
    static constexpr qsizetype ChunkBytes = 64 * 1024, MessageBytes = 8 * 1024 * 1024,
                               QueueBytes = 16 * 1024 * 1024;
    std::function<void(const QJsonObject &, const QByteArray &)> received;
    std::function<void()> connected;
    std::function<void(const QString &)> failed;
    explicit BulkChannel(QObject *parent = nullptr) : QObject(parent) {
        connect(&server, &QLocalServer::newConnection, this, [this] {
            auto *s = server.nextPendingConnection();
            if (socket) {
                s->deleteLater();
                return;
            }
            attach(s);
        });
        timer.setInterval(250);
        connect(&timer, &QTimer::timeout, this, [this] {
            if ((!authenticated || !partial.isEmpty()) && deadline < now())
                fail("Bulk channel timed out");
        });
    }
    QJsonObject listen(const QString &owner, const QString &epoch) {
        lease = owner;
        generation = epoch;
        token = QUuid::createUuid().toString(QUuid::Id128) + QUuid::createUuid().toString(QUuid::Id128);
        auto name = "zto-bulk-" + QUuid::createUuid().toString(QUuid::Id128);
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server.listen(name))
            return {};
        deadline = now() + 10000;
        timer.start();
        return {{"name", name},
                {"token", token},
                {"lease", lease},
                {"generation", generation},
                {"messageBytes", double(MessageBytes)}};
    }
    void open(const QJsonObject &d) {
        lease = d["lease"].toString();
        generation = d["generation"].toString();
        token = d["token"].toString();
        if (lease.size() != 32 || token.size() != 64 || generation.isEmpty()) {
            fail("Invalid bulk descriptor");
            return;
        }
        outgoing = true;
        auto *s = new QLocalSocket(this);
        attach(s);
        deadline = now() + 10000;
        timer.start();
        connect(s, &QLocalSocket::connected, this,
                [this] { write({{"hello", token}, {"lease", lease}, {"generation", generation}}, {}); });
        s->connectToServer(d["name"].toString());
    }
    bool ready() const { return authenticated && socket; }
    bool send(const QJsonObject &header, const QByteArray &bytes = {}) {
        if (!ready() || bytes.size() > MessageBytes ||
            socket->bytesToWrite() + bytes.size() + MaxEnvelope > QueueBytes)
            return false;
        auto h = header;
        h["serial"] = QString::number(++sent);
        h["total"] = double(bytes.size());
        for (qsizetype offset = 0;; offset += ChunkBytes) {
            h["offset"] = double(offset);
            if (!write(h, bytes.mid(offset, ChunkBytes)))
                return false;
            if (offset + ChunkBytes >= bytes.size())
                break;
        }
        return true;
    }
    void close() {
        authenticated = false;
        timer.stop();
        server.close();
        if (socket) {
            socket->disconnect(this);
            socket->abort();
            socket->deleteLater();
            socket = nullptr;
        }
        partial.clear();
        buffer.clear();
        received = {};
        connected = {};
    }
    ~BulkChannel() override { close(); }

  private:
    QLocalServer server;
    QLocalSocket *socket = nullptr;
    QTimer timer;
    QString lease, generation, token;
    QByteArray buffer, partial;
    QJsonObject partialHeader;
    quint64 sent = 0, expected = 1;
    qint64 deadline = 0;
    bool authenticated = false, outgoing = false;
    static qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
    void attach(QLocalSocket *s) {
        socket = s;
        s->setParent(this);
        s->setReadBufferSize(MaxFrame * 2);
        connect(s, &QLocalSocket::readyRead, this, [this] { read(); });
        connect(s, &QLocalSocket::disconnected, this, [this] { fail("Bulk channel disconnected"); });
        connect(s, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            fail(socket ? socket->errorString() : QString("Bulk channel failed"));
        });
    }
    bool write(const QJsonObject &h, const QByteArray &p) {
        auto bytes = frame(h, p);
        return socket && !bytes.isEmpty() && socket->write(bytes) == bytes.size();
    }
    void fail(const QString &s) {
        auto callback = failed;
        failed = {};
        close();
        if (callback) {
            try{callback(s);}catch(...){qWarning("Bulk channel failure callback threw an exception");}
        }
    }
    void read() {
        buffer += socket->readAll();
        for (;;) {
            Message m;
            int status = take(buffer, m);
            if (!status)
                break;
            if (status < 0) {
                fail("Invalid bulk frame");
                return;
            }
            if (!authenticated) {
                if (!m.payload.isEmpty() || m.header["lease"] != lease ||
                    m.header["generation"] != generation ||
                    (outgoing ? m.header["accepted"] != true : m.header["hello"] != token)) {
                    fail("Bulk authentication failed");
                    return;
                }
                if (!outgoing) {
                    server.close();
                    write({{"accepted", true}, {"lease", lease}, {"generation", generation}}, {});
                }
                authenticated = true;
                token.clear();
                QPointer<BulkChannel> alive(this);
                auto callback = connected;
                if(callback){try{callback();}catch(...){if(alive)fail("Bulk connection callback failed");return;}}
                if (!alive)
                    return;
                continue;
            }
            const auto total = m.header["total"].toInteger(-1), offset = m.header["offset"].toInteger(-1);
            bool ok = false;
            auto serial = m.header["serial"].toString().toULongLong(&ok);
            auto identity = m.header;
            identity.remove("offset");
            if (!ok || serial != expected || total < 0 || total > MessageBytes || offset != partial.size() ||
                m.payload.size() > ChunkBytes || offset + m.payload.size() > total ||
                (offset && identity != partialHeader) || (m.payload.isEmpty() && total)) {
                fail("Invalid bulk fragment");
                return;
            }
            if (!offset) {
                partialHeader = identity;
                deadline = now() + 10000;
            }
            partial += m.payload;
            if (partial.size() == total) {
                auto bytes = std::move(partial);
                partial.clear();
                ++expected;
                auto header = partialHeader;
                partialHeader = {};
                QPointer<BulkChannel> alive(this);
                auto callback = received;
                if(callback){try{callback(header,bytes);}catch(...){if(alive)fail("Bulk message callback failed");return;}}
                if (!alive || !socket)
                    return;
            }
        }
        if (buffer.size() > MaxFrame)
            fail("Bulk read limit exceeded");
    }
};
} // namespace vusiif::zto::protocol::service
