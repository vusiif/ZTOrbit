// Stage-6 remediation regression (R1-R4).
//
// These tests are the formal home of the six failing assertions from the
// independent stage-6 review plus the additional failure branches that review
// demanded: explicit/partial removal failure, drain timeout, late results,
// successful retry, stale stop completion versus a newer session, blocking of
// conflicting room operations while a track release is unconfirmed, and
// field-level preference merging across two processes.
//
// R2 note: the redesign refuses a new create/join while an older track release
// is not yet confirmed, so the original "join issued, then an old stop
// completion arrives" interleaving no longer exists. The equivalent contract
// proven here is:
//   * no create/join is issued while the drain is unconfirmed,
//   * an explicit operation succeeds after the drain is confirmed,
//   * a stale completion cannot clear a newer connect operation, overwrite the
//     state or cancel/abandon the successful join that follows.
#include "DeskShareController.h"
#include "NetworkClient.h"
#include "Preferences.h"

#include <desktop-share/ControlProtocol.h>
#include <zto/HostServices.h>
#include <zto/PluginStorage.h>
#include <ztprotocol/BulkChannel.h>
#include <ztprotocol/WireProtocol.h>

#include <QAbstractEventDispatcher>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

#include <cstring>
#include <memory>

using namespace vusiif::zto;
using namespace vusiif::zto::desktop_share;
namespace wire = vusiif::zto::protocol;
namespace util = vusiif::zto::util;
namespace media = vusiif::zto::media;

namespace {

template <class Predicate> bool waitUntil(Predicate predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) QTest::qWait(20);
    return predicate();
}

QString stat(DeskShareController &controller, const char *name)
{
    return controller.lifecycleStatistics().value(QLatin1String(name)).toString();
}

bool flag(DeskShareController &controller, const char *name)
{
    return controller.lifecycleStatistics().value(QLatin1String(name)).toBool();
}

// Public media provider double. It can hold every track.remove completion, fail
// all of them, fail only selected ids, and then answer the held callbacks
// explicitly, so a test controls the exact completion order.
class ReleaseLease final : public util::ServiceLease {
public:
    struct Create {
        QString id;
        QJsonObject config;
        QPointer<QObject> context;
        util::ServiceCallback done;
    };
    struct Pending {
        QString id;
        util::ServiceCallback done;
    };

    vusiif::zto::protocol::service::BulkChannel channel;
    QList<Create> creates;
    QStringList started, removed, stopped, keyframes;
    QList<Pending> pendingRemoves;
    bool deferCreate = false;
    bool deferRemove = false;
    bool failRemove = false;
    QSet<QString> failRemoveFor;
    qint64 lastRemovedAtMs = -1;

    quint64 generation() const override { return 1; }
    bool available() const override { return true; }
    void subscribe(QObject *, util::ServiceCallback) override {}

    void close(QObject *, util::ServiceCallback done) override
    {
        channel.close();
        if (done) done({});
    }

    void call(const QString &method, const QJsonObject &data, const QByteArray &, QObject *context,
              util::ServiceCallback done) override
    {
        if (method == QStringLiteral("$channel")) {
            util::ServiceResult channelResult;
            channelResult.data = channel.listen(QString(32, 'a'), QStringLiteral("1"));
            done(channelResult);
            return;
        }
        if (method == QStringLiteral("sources")) {
            QJsonArray sources;
            sources.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("screen-1")},
                                       {QStringLiteral("kind"), QStringLiteral("screen")},
                                       {QStringLiteral("description"), QStringLiteral("Synthetic screen")},
                                       {QStringLiteral("default"), true}});
            sources.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("window-1")},
                                       {QStringLiteral("kind"), QStringLiteral("window")},
                                       {QStringLiteral("description"), QStringLiteral("Synthetic window")},
                                       {QStringLiteral("default"), false}});
            util::ServiceResult sourceResult;
            sourceResult.data.insert(QStringLiteral("sources"), sources);
            done(sourceResult);
            return;
        }
        if (method == QStringLiteral("track.create")) {
            Create create;
            create.id = QStringLiteral("track-%1").arg(creates.size());
            create.config = data.value(QStringLiteral("config")).toObject();
            create.context = context;
            create.done = std::move(done);
            creates.append(create);
            if (!deferCreate) completeCreate(creates.size() - 1, create.id);
            return;
        }
        if (method == QStringLiteral("track.remove")) {
            const auto id = data.value(QStringLiteral("id")).toString();
            removed.append(id);
            if (deferRemove) {
                pendingRemoves.append({id, std::move(done)});
                return;
            }
            if (done) {
                const bool fail = failRemove || failRemoveFor.contains(id);
                QTimer::singleShot(0, context, [done, fail] {
                    done(fail ? util::ServiceResult{{}, {}, QStringLiteral("remove failed")}
                              : util::ServiceResult{{}, {}, {}});
                });
            }
            return;
        }
        if (method == QStringLiteral("track.start"))
            started.append(data.value(QStringLiteral("id")).toString());
        else if (method == QStringLiteral("track.stop"))
            stopped.append(data.value(QStringLiteral("id")).toString());
        else if (method == QStringLiteral("track.keyframe"))
            keyframes.append(data.value(QStringLiteral("id")).toString());
        if (done) QTimer::singleShot(0, context, [done] { done({}); });
    }

    void completeCreate(int index, const QString &id)
    {
        const auto pending = creates.at(index);
        if (pending.context && pending.done) {
            QJsonObject response{{QStringLiteral("id"), id},
                                 {QStringLiteral("configuration"), QStringLiteral("1")},
                                 {QStringLiteral("config"), pending.config}};
            util::ServiceResult created;
            created.data = response;
            pending.done(created);
        }
    }

    // Answers every held removal callback with the requested outcome.
    void completeRemoves(bool ok = true)
    {
        lastRemovedAtMs = QDateTime::currentMSecsSinceEpoch();
        const auto pending = std::move(pendingRemoves);
        pendingRemoves.clear();
        for (const auto &entry : pending) {
            if (!entry.done) continue;
            entry.done(ok ? util::ServiceResult{{}, {}, {}}
                          : util::ServiceResult{{}, {}, QStringLiteral("remove failed")});
        }
    }
};

// Scripted control host with per-operation delays so an old completion can be
// delivered while a new control operation is in flight.
class RoomHost final : public QObject {
public:
    explicit RoomHost(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = m_server.nextPendingConnection()) {
                ++m_accepted;
                m_buffers.insert(socket, QByteArray());
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this] { ++m_closed; });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_server.serverPort(); }
    quint64 roomId() const { return m_roomId; }
    void disconnectClients()
    {
        for (auto *socket : m_buffers.keys()) socket->abort();
    }

    int createCount = 0;
    int joinCount = 0;
    int stopCount = 0;
    int leaveCount = 0;
    int helloDelayMs = 0;
    int createDelayMs = 0;
    int joinDelayMs = 0;
    int acceptedCount() const { return m_accepted; }
    int closeCount() const { return m_closed; }

private:
    void respond(QTcpSocket *socket, const Response &response)
    {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState) return;
        socket->write(wire::frame(QByteArray(Route), encodeResponse(response)));
    }

    void read(QTcpSocket *socket)
    {
        auto &buffer = m_buffers[socket];
        buffer += socket->readAll();
        for (;;) {
            wire::Packet frame;
            const auto status = wire::takeFrame(buffer, frame);
            if (status == wire::ReadResult::Incomplete) return;
            if (status == wire::ReadResult::Invalid) {
                socket->abort();
                return;
            }
            if (frame.plugin != QByteArray(Route)) continue;
            Request request;
            if (!decodeRequest(frame.payload, request).ok) continue;
            const QPointer<QTcpSocket> guard(socket);
            if (request.op == Operation::Hello) {
                QTimer::singleShot(helloDelayMs, this, [this, guard, request] {
                    Response response;
                    response.id = request.id;
                    response.ok = true;
                    response.op = Operation::Hello;
                    response.hello.sessionId = 4242;
                    response.hello.udpPort = 18199;
                    response.hello.maxViewers = 8;
                    response.hello.profiles = {QStringLiteral("720p30"),
                                               QStringLiteral("1080p30")};
                    respond(guard, response);
                });
                continue;
            }
            if (request.op == Operation::Create) {
                ++createCount;
                const auto profile = request.profile;
                QTimer::singleShot(createDelayMs, this, [this, guard, request, profile] {
                    Response response;
                    response.id = request.id;
                    response.ok = true;
                    response.op = Operation::Create;
                    response.room.roomId = m_roomId;
                    response.room.roomCode = QStringLiteral("00000042");
                    response.room.profile = profile;
                    response.room.role = Role::Publisher;
                    response.room.token = QByteArray(16, 'a');
                    respond(guard, response);
                });
                continue;
            }
            if (request.op == Operation::Join) {
                ++joinCount;
                QTimer::singleShot(joinDelayMs, this, [this, guard, request] {
                    Response response;
                    response.id = request.id;
                    response.ok = true;
                    response.op = Operation::Join;
                    response.room.roomId = m_roomId;
                    response.room.roomCode = QStringLiteral("00000042");
                    response.room.profile = QStringLiteral("720p30");
                    response.room.role = Role::Viewer;
                    response.room.token = QByteArray(16, 'b');
                    respond(guard, response);
                });
                continue;
            }
            if (request.op == Operation::Stop) ++stopCount;
            if (request.op == Operation::Leave) ++leaveCount;
            if (request.op == Operation::StreamConfigure) {
                Response response;
                response.id = request.id;
                response.ok = true;
                response.op = Operation::StreamConfigure;
                response.roomId = request.roomId;
                response.streamEpoch = 5;
                respond(guard, response);
                continue;
            }
            Response response;
            response.id = request.id;
            response.ok = true;
            response.op = request.op;
            if (request.op == Operation::StreamReady
                || request.op == Operation::StreamKeyframe) {
                response.roomId = request.roomId;
                response.streamEpoch = request.streamEpoch;
            }
            respond(guard, response);
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    quint64 m_roomId = 77;
    int m_accepted = 0;
    int m_closed = 0;
};

// Public host file service with the optional field-merging transaction and an
// injectable write failure.
class FileStore final : public util::PluginFiles, public util::PluginStorage {
public:
    util::FileResult readFile(const QString &path) override
    {
        QMutexLocker lock(&m_mutex);
        if (!m_files.contains(path)) return {{}, QStringLiteral("no such file")};
        return {m_files.value(path), {}};
    }
    util::FileResult writeFile(const QString &path, const QByteArray &data) override
    {
        QMutexLocker lock(&m_mutex);
        if (m_failWrites) return {{}, QStringLiteral("disk full")};
        m_files.insert(path, data);
        return {};
    }
    util::PluginFileTransaction updateFile(const QString &path,
                                           const util::PluginFileUpdater &updater) override
    {
        QMutexLocker lock(&m_mutex);
        util::PluginFileTransaction result;
        if (m_failWrites) {
            result.error = QStringLiteral("disk full");
            return result;
        }
        util::PluginFileSnapshot snapshot;
        snapshot.exists = m_files.contains(path);
        if (snapshot.exists) snapshot.data = m_files.value(path);
        util::PluginFileUpdate update;
        try {
            update = updater(snapshot);
        } catch (...) {
            result.error = QStringLiteral("updater threw");
            return result;
        }
        if (update.action == util::PluginFileUpdate::Action::Keep) {
            result.ok = true;
            result.data = snapshot.data;
            return result;
        }
        if (update.action == util::PluginFileUpdate::Action::Fail) {
            result.ok = false;
            result.error = update.error;
            return result;
        }
        result.ok = true;
        if (snapshot.data != update.data) {
            m_files.insert(path, update.data);
            result.changed = true;
        }
        result.data = update.data;
        return result;
    }
    util::LegacyImportState legacyImportAllowed() override { return {}; }
    util::StorageResult completeLegacyImport() override { return {}; }

    void preset(const QString &path, const QByteArray &data)
    {
        QMutexLocker lock(&m_mutex);
        m_files.insert(path, data);
    }
    QByteArray data(const QString &path)
    {
        QMutexLocker lock(&m_mutex);
        return m_files.value(path);
    }
    void setFailWrites(bool fail) { m_failWrites = fail; }

private:
    QMutex m_mutex;
    QHash<QString, QByteArray> m_files;
    bool m_failWrites = false;
};

class NullZeroTier final : public util::ZeroTierService {
public:
    void getZTInfo(QObject *, util::JsonCallback done) override { done({}); }
    void getZTPeers(QObject *, util::JsonCallback done) override { done({}); }
    void getZTNetworks(QObject *, util::JsonCallback done) override { done({}); }
    void getZTMoons(QObject *, util::JsonCallback done) override { done({}); }
    void sendDataTo(const QString &, const QString &, const QByteArray &, QObject *,
                    util::SendCallback done) override { done({}); }
    void broadcastDataTo(const QString &, const QStringList &, const QByteArray &, QObject *,
                         util::SendCallback done) override { done({}); }
    void broadcastDataBalancedTo(const QString &, const QStringList &, const QByteArray &,
                                 QObject *, util::SendCallback done) override { done({}); }
    void getNetworkRelays(const QString &, QObject *, util::RelayCallback done) override { done({}); }
    util::NetworkSubscription *subscribeNetworkData(const QString &, QObject *,
                                                    util::NetworkCallback) override { return nullptr; }
};

class NullLogger final : public util::PluginLogger {
public:
    util::LogResult write(util::LogLevel, const QString &) override { return {}; }
};

class FakeHost final : public util::HostServices {
public:
    explicit FakeHost(FileStore *store) : m_store(store) {}
    util::PluginFiles &files() override { return *m_store; }
    util::ZeroTierService &zeroTier() override { return m_zt; }
    util::PluginLogger &logs() override { return m_logger; }

private:
    FileStore *m_store;
    NullZeroTier m_zt;
    NullLogger m_logger;
};

const QString Source = QStringLiteral("screen:screen-1");

void appendSyntheticPacket(media::MediaSession &session, const QString &track, qint64 pts,
                           bool keyFrame)
{
    media::EncodedPacket packet;
    packet.track = track;
    packet.codec = QStringLiteral("bgra-tiles");
    packet.sequence = quint64(pts / 1000);
    packet.pts = pts;
    packet.duration = 33333;
    packet.timeBase = {1, 1000000};
    packet.keyFrame = keyFrame;
    packet.discontinuity = keyFrame;
    packet.data = QByteArray(64, 't');
    if (session.packet) session.packet(packet);
}

StreamFormat h264Format()
{
    StreamFormat format;
    format.codec = QStringLiteral("h264");
    format.width = 1280;
    format.height = 720;
    format.fps = 30;
    format.bitrate = 2000000;
    format.streamEpoch = 3;
    return format;
}

// Everything a publishing test needs.
struct Publisher {
    RoomHost host;
    std::shared_ptr<ReleaseLease> lease = std::make_shared<ReleaseLease>();
    std::shared_ptr<media::MediaSession> session =
        std::make_shared<media::MediaSession>(lease);
    DeskShareController controller{session};
    bool listening = false;

    Publisher()
    {
        listening = host.listen();
        if (!listening) return;
        controller.initialize();
        if (!waitUntil([this] { return controller.sources().size() >= 2; }, 3000)) return;
        controller.setSourceValue(Source);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        if (!waitUntil([this] { return controller.connected(); }, 3000)) return;
        controller.startSharing();
        waitUntil([this] { return lease->started.size() == 1; }, 3000);
    }
    bool ready() const { return listening && lease->started.size() == 1; }
};

} // namespace

class Stage6RemediationTests final : public QObject {
    Q_OBJECT

private slots:
    // ---------------- R1: removal result is authoritative ----------------
    void sendTrackRemoveFailureIsVisibleAndRetryUsesSameTrack()
    {
        Publisher p;
        QVERIFY(p.ready());
        p.lease->failRemove = true;
        p.controller.stopSharing();
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("stopFailed"); },
                          4000));
        QCOMPARE(p.controller.state(), QStringLiteral("stop-failed"));
        QCOMPARE(stat(p.controller, "tracksPending"), QStringLiteral("true"));
        QCOMPARE(stat(p.controller, "releasingTracks"), QStringLiteral("1"));
        QVERIFY2(stat(p.controller, "pendingReleaseSummary").contains(QStringLiteral("send:track-0")),
                 qPrintable(stat(p.controller, "pendingReleaseSummary")));
        QVERIFY(!p.controller.canStartShare());
        QVERIFY(!p.controller.canJoin());
        QVERIFY(p.controller.errorText().contains(QStringLiteral("释放")));
        QVERIFY(!p.controller.nextAction().isEmpty());
        QCOMPARE(p.lease->removed.size(), 1);

        // Retry after the provider is healthy again: the same identity is
        // targeted and the stop completes only after it is confirmed.
        p.lease->failRemove = false;
        p.controller.stopSharing();
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("ended"); }, 4000));
        QCOMPARE(p.controller.state(), QStringLiteral("idle"));
        QCOMPARE(stat(p.controller, "tracksPending"), QStringLiteral("false"));
        QCOMPARE(stat(p.controller, "releasingTracks"), QStringLiteral("0"));
        QCOMPARE(stat(p.controller, "releaseFailures"), QStringLiteral("1"));
        QCOMPARE(p.lease->removed.size(), 2);
        QCOMPARE(p.lease->removed.at(0), QStringLiteral("track-0"));
        QCOMPARE(p.lease->removed.at(1), QStringLiteral("track-0"));
        QVERIFY(p.controller.shutdown());
    }

    void receiveTrackRemoveFailureKeepsIdentityAndRetrySucceeds()
    {
        RoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<ReleaseLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.setJoinCode(QStringLiteral("00000042"));
        controller.joinRoomExplicit();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("viewerWaiting"); },
                          3000));
        auto *network = controller.findChild<NetworkClient *>();
        QVERIFY(network);
        emit network->streamConfigReceived(host.roomId(), h264Format());
        QVERIFY(waitUntil([&] { return lease->started.size() == 1; }, 3000));
        QCOMPARE(controller.watching(), true);

        lease->failRemove = true;
        controller.leaveRoom();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("stopFailed"); },
                          4000));
        QCOMPARE(stat(controller, "tracksPending"), QStringLiteral("true"));
        QVERIFY2(stat(controller, "pendingReleaseSummary")
                     .contains(QStringLiteral("receive:track-0")),
                 qPrintable(stat(controller, "pendingReleaseSummary")));
        QVERIFY(!controller.canJoin());
        QVERIFY(!controller.canStartShare());

        lease->failRemove = false;
        controller.stopSharing();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("ended"); }, 4000));
        QCOMPARE(stat(controller, "releasingTracks"), QStringLiteral("0"));
        QCOMPARE(controller.watching(), false);
        QVERIFY(controller.shutdown());
    }

    void partialSuccessKeepsOnlyTheUnconfirmedTrack()
    {
        // Local loopback creates both a send and a receive track. Only the
        // receive removal fails, so the send identity must be dropped while the
        // receive identity stays retryable.
        RoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<ReleaseLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setSourceValue(Source);
        controller.startCapture();
        QVERIFY(waitUntil([&] { return lease->started.size() == 1; }, 3000));
        appendSyntheticPacket(*session, QStringLiteral("track-0"), 1000, true);
        QVERIFY(waitUntil([&] { return lease->creates.size() == 2; }, 3000));
        QVERIFY(waitUntil([&] { return lease->started.size() == 2; }, 3000));

        lease->failRemoveFor.insert(QStringLiteral("track-1"));
        controller.stopCapture();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("stopFailed"); },
                          4000));
        QCOMPARE(stat(controller, "releasingTracks"), QStringLiteral("1"));
        QVERIFY2(stat(controller, "pendingReleaseSummary")
                     .contains(QStringLiteral("receive:track-1")),
                 qPrintable(stat(controller, "pendingReleaseSummary")));
        QVERIFY(!stat(controller, "pendingReleaseSummary").contains(QStringLiteral("track-0")));
        QCOMPARE(stat(controller, "sendTrack"), QStringLiteral(""));
        QCOMPARE(stat(controller, "receiveTrack"), QStringLiteral(""));

        lease->failRemoveFor.clear();
        controller.stopSharing();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("ended"); }, 4000));
        QCOMPARE(stat(controller, "releasingTracks"), QStringLiteral("0"));
        // The retry only re-issued the unconfirmed identity.
        QCOMPARE(lease->removed.count(QStringLiteral("track-0")), 1);
        QCOMPARE(lease->removed.count(QStringLiteral("track-1")), 2);
        QVERIFY(controller.shutdown());
    }

    void drainTimeoutKeepsTracksAndLateSuccessRecovers()
    {
        Publisher p;
        QVERIFY(p.ready());
        p.lease->deferRemove = true;
        p.controller.stopSharing();
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("stopFailed"); },
                          4500));
        QCOMPARE(stat(p.controller, "releasingTracks"), QStringLiteral("1"));
        QVERIFY(!stat(p.controller, "mediaDrained").isEmpty());
        QCOMPARE(flag(p.controller, "mediaDrained"), false);
        QVERIFY(!p.controller.canStartShare());
        QCOMPARE(stat(p.controller, "lastReleaseError"), QString());

        // The provider answers late: the identity is confirmed, the failure is
        // lifted and no second remove was issued.
        p.lease->completeRemoves(true);
        QVERIFY(waitUntil([&] { return stat(p.controller, "releasingTracks") == QStringLiteral("0"); },
                          2000));
        QCOMPARE(stat(p.controller, "tracksPending"), QStringLiteral("false"));
        QCOMPARE(p.controller.phase(), QStringLiteral("ready"));
        QCOMPARE(p.controller.state(), QStringLiteral("idle"));
        QCOMPARE(p.lease->removed.size(), 1);
        QVERIFY(p.controller.canStartShare());
        QVERIFY(p.controller.shutdown());
    }

    void explicitFailureIsReportedAndLateResultCannotWriteTerminalState()
    {
        Publisher p;
        QVERIFY(p.ready());
        p.lease->deferRemove = true;
        p.controller.stopSharing();
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("stopFailed"); },
                          4500));
        // Explicit provider failure for the held request.
        p.lease->completeRemoves(false);
        QVERIFY(waitUntil([&] { return !stat(p.controller, "lastReleaseError").isEmpty(); }, 2000));
        QCOMPARE(stat(p.controller, "releasingTracks"), QStringLiteral("1"));
        QCOMPARE(stat(p.controller, "releaseFailures"), QStringLiteral("1"));
        QVERIFY(p.controller.phase() == QStringLiteral("stopFailed"));

        // A retry issues one fresh request for the retained identity and only
        // completes after that request is confirmed.
        p.lease->failRemove = false;
        p.controller.stopSharing();
        QCOMPARE(p.controller.phase(), QStringLiteral("stopping"));
        QCOMPARE(p.lease->pendingRemoves.size(), 1);
        p.lease->completeRemoves(true);
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("ended"); }, 4000));
        // A duplicate/late completion cannot rewrite the terminal state.
        const auto generation = stat(p.controller, "stopGeneration");
        p.lease->completeRemoves(true);
        QTest::qWait(200);
        QCOMPARE(p.controller.phase(), QStringLiteral("ended"));
        QCOMPARE(stat(p.controller, "stopGeneration"), generation);
        QCOMPARE(p.controller.state(), QStringLiteral("idle"));
        QVERIFY(p.controller.shutdown());
    }

    void unloadRemoveFailureRetainsControllerAndRetrySucceeds()
    {
        Publisher p;
        QVERIFY(p.ready());
        p.lease->failRemove = true;
        bool done = false, ok = true;
        p.controller.shutdownAsync([&](bool result) {
            done = true;
            ok = result;
        }, 5000);
        QVERIFY(waitUntil([&] { return done; }, 6000));
        QVERIFY2(!ok, "an explicit removal failure was reported as a successful unload");
        QCOMPARE(p.controller.state(), QStringLiteral("stop-failed"));
        QCOMPARE(flag(p.controller, "mediaDrained"), false);
        QCOMPARE(stat(p.controller, "releasingTracks"), QStringLiteral("1"));
        QVERIFY(p.controller.errorText().contains(QStringLiteral("释放")));

        p.lease->failRemove = false;
        done = false;
        ok = false;
        p.controller.shutdownAsync([&](bool result) {
            done = true;
            ok = result;
        }, 5000);
        QVERIFY(waitUntil([&] { return done; }, 6000));
        QVERIFY(ok);
        QCOMPARE(p.controller.state(), QStringLiteral("stopped"));
        QCOMPARE(flag(p.controller, "mediaDrained"), true);
        QCOMPARE(stat(p.controller, "releasingTracks"), QStringLiteral("0"));
    }

    // ---------------- R2: stale stop completion versus a new session -------
    void oldStopCompletionCannotOverwriteOrCancelReconnectedSession()
    {
        Publisher p;
        QVERIFY(p.ready());
        p.lease->deferRemove = true;
        p.controller.stopSharing();
        QVERIFY(waitUntil([&] { return stat(p.controller, "releasingTracks") == QStringLiteral("1"); },
                          2000));

        // TCP loss abandons the stop task; the track identity survives.
        p.host.disconnectClients();
        QVERIFY(waitUntil([&] { return !p.controller.connected(); }, 3000));
        QCOMPARE(p.controller.phase(), QStringLiteral("disconnected"));
        QCOMPARE(stat(p.controller, "releasingTracks"), QStringLiteral("1"));
        QCOMPARE(stat(p.controller, "stopTaskActive"), QStringLiteral("false"));

        // A new connection is allowed and its handshake is in flight while the
        // old release is still pending. The old completion must not clear it.
        p.host.helloDelayMs = 500;
        QVERIFY(p.controller.canConnect());
        p.controller.connectServer();
        QVERIFY(waitUntil([&] { return stat(p.controller, "operation") == QStringLiteral("connect"); },
                          1000));
        p.lease->completeRemoves(true);
        QTest::qWait(100);
        QCOMPARE(stat(p.controller, "operation"), QStringLiteral("connect"));
        QCOMPARE(p.controller.phase(), QStringLiteral("connecting"));
        QCOMPARE(stat(p.controller, "releasingTracks"), QStringLiteral("0"));
        QVERIFY(waitUntil([&] { return p.controller.connected(); }, 3000));
        QCOMPARE(p.controller.phase(), QStringLiteral("ready"));

        // No room operation may be created while the drain is unconfirmed: the
        // join is refused with no server request. Then an explicit retry after
        // the drain is confirmed succeeds and keeps its ownership.
        QCOMPARE(p.host.joinCount, 0);
        QCOMPARE(p.host.createCount, 1);
        p.controller.setJoinCode(QStringLiteral("00000042"));
        p.host.joinDelayMs = 200;
        p.controller.joinRoomExplicit();
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("viewerWaiting"); },
                          3000));
        QCOMPARE(p.host.joinCount, 1);
        QCOMPARE(p.host.leaveCount, 0);
        QCOMPARE(p.controller.statistics().value(QStringLiteral("viewer")).toBool(), true);
        // The stale stop task never ran its success branch.
        QVERIFY(p.controller.phase() == QStringLiteral("viewerWaiting"));
        QVERIFY(p.controller.shutdown());
    }

    void conflictingRoomOperationsAreRefusedUntilTheDrainIsConfirmed()
    {
        Publisher p;
        QVERIFY(p.ready());
        p.lease->deferRemove = true;
        p.controller.stopSharing();
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("stopFailed"); },
                          4500));
        const auto creates = p.lease->creates.size();
        const auto removes = p.lease->removed.size();
        const auto profile = p.controller.profile();
        const auto codec = p.controller.codec();
        QVERIFY(!p.controller.canStartShare());
        QVERIFY(!p.controller.canJoin());
        QVERIFY(!p.controller.canSelectSource());
        QVERIFY(!p.controller.canSelectVideo());
        QVERIFY(p.controller.canStopShare());

        // Business entries refuse too, not only the capability flags.
        p.controller.startSharing();
        p.controller.joinRoomExplicit();
        p.controller.startCapture();
        p.controller.setSourceValue(QStringLiteral("screen:screen-1"));
        p.controller.setProfile(QStringLiteral("1080p30"));
        p.controller.setCodec(codec == QStringLiteral("h264") ? QStringLiteral("bgra-tiles") : QStringLiteral("h264"));
        QTest::qWait(300);
        QCOMPARE(p.host.createCount, 1);
        QCOMPARE(p.host.joinCount, 0);
        QCOMPARE(p.lease->creates.size(), creates);
        QCOMPARE(p.lease->removed.size(), removes);
        QCOMPARE(p.controller.profile(), profile);
        QCOMPARE(p.controller.codec(), codec);
        // The pending identity is untouched by the refused initialisation.
        QVERIFY2(stat(p.controller, "pendingReleaseSummary")
                     .contains(QStringLiteral("send:track-0")),
                 qPrintable(stat(p.controller, "pendingReleaseSummary")));

        // Confirmed release restores the entries; the explicit retry succeeds
        // and a new room can be created.
        p.lease->deferRemove = false;
        p.lease->completeRemoves(true);
        QVERIFY(waitUntil([&] { return stat(p.controller, "releasingTracks") == QStringLiteral("0"); },
                          2000));
        QCOMPARE(p.controller.phase(), QStringLiteral("ready"));
        QVERIFY(p.controller.canStartShare());
        p.controller.startSharing();
        QVERIFY(waitUntil([&] { return p.host.createCount == 2; }, 3000));
        QVERIFY(waitUntil([&] { return p.lease->creates.size() == creates + 1; }, 3000));
        QVERIFY(waitUntil([&] { return p.lease->started.size() == 2; }, 3000));
        p.controller.stopSharing();
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("ended"); }, 4000));
        QVERIFY(p.controller.shutdown());
    }

    void repeatedStopRetriesAreMergedIntoOneControlledTransition()
    {
        Publisher p;
        QVERIFY(p.ready());
        p.lease->deferRemove = true;
        p.controller.stopSharing();
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("stopFailed"); },
                          4500));
        QCOMPARE(p.lease->removed.size(), 1);
        QCOMPARE(p.host.stopCount, 1);

        // Two retries in a row: one controlled transition, one fresh request.
        p.controller.stopSharing();
        QCOMPARE(p.controller.phase(), QStringLiteral("stopping"));
        p.controller.stopSharing();
        QTest::qWait(200);
        QCOMPARE(p.controller.phase(), QStringLiteral("stopping"));
        QCOMPARE(p.lease->removed.size(), 2);
        QCOMPARE(p.lease->pendingRemoves.size(), 2);
        QCOMPARE(p.host.stopCount, 1);

        p.lease->completeRemoves(true);
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("ended"); }, 3000));
        QCOMPARE(stat(p.controller, "releasingTracks"), QStringLiteral("0"));
        QCOMPARE(p.lease->removed.size(), 2);
        QVERIFY(p.controller.shutdown());
    }

    // ---------------- R3: no new session before drain resolves -------------
    void failedDrainBlocksNewRoomAndNewInitialisation()
    {
        Publisher p;
        QVERIFY(p.ready());
        p.lease->failRemove = true;
        p.controller.stopSharing();
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("stopFailed"); },
                          4000));
        const auto creates = p.lease->creates.size();
        QVERIFY(!p.controller.canStartShare());
        QVERIFY(!p.controller.canJoin());
        p.controller.startSharing();
        QTest::qWait(300);
        QCOMPARE(p.host.createCount, 1);
        QCOMPARE(p.lease->creates.size(), creates);
        QCOMPARE(stat(p.controller, "sendTrack"), QStringLiteral(""));
        QVERIFY2(stat(p.controller, "pendingReleaseSummary")
                     .contains(QStringLiteral("send:track-0")),
                 qPrintable(stat(p.controller, "pendingReleaseSummary")));
        p.lease->failRemove = false;
        p.controller.stopSharing();
        QVERIFY(waitUntil([&] { return p.controller.phase() == QStringLiteral("ended"); }, 4000));
        QVERIFY(p.controller.canStartShare());
        QVERIFY(p.controller.shutdown());
    }

    // ---------------- R4: dirty-field-only preference merge -----------------
    void addressOnlyEditDoesNotRestoreAnotherProcessesOldProfile()
    {
        FileStore store;
        FakeHost host(&store);
        store.preset(QStringLiteral("preferences.json"),
                     QByteArray("{\"serverAddress\":\"old:1\",\"profile\":\"720p30\"}"));
        Preferences prefs(&host);
        auto lease = std::make_shared<ReleaseLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.setPreferences(&prefs);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        QCOMPARE(controller.profile(), QStringLiteral("720p30"));
        // Another process moves the profile to 1080p30.
        store.preset(QStringLiteral("preferences.json"),
                     QByteArray("{\"serverAddress\":\"old:1\",\"profile\":\"1080p30\"}"));
        controller.setServerAddress(QStringLiteral("new:2"));
        QVERIFY(waitUntil([&] {
            return QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json")))
                       .object()
                       .value(QStringLiteral("serverAddress"))
                       .toString()
                == QStringLiteral("new:2");
        }, 3000));
        const auto object =
            QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json"))).object();
        QCOMPARE(object.value(QStringLiteral("profile")).toString(), QStringLiteral("1080p30"));
        QCOMPARE(object.value(QStringLiteral("serverAddress")).toString(),
                 QStringLiteral("new:2"));
        QVERIFY(!object.contains(QStringLiteral("roomCode")));
        QVERIFY(!object.contains(QStringLiteral("token")));
        QVERIFY(!object.contains(QStringLiteral("joinCode")));
        QVERIFY(!object.contains(QStringLiteral("autoShare")));
        QVERIFY(controller.shutdown());
    }

    void interleavedDifferentFieldEditsBothSurvive()
    {
        FileStore store;
        FakeHost host(&store);
        store.preset(QStringLiteral("preferences.json"), QByteArray("{}"));
        Preferences prefsA(&host);
        Preferences prefsB(&host);
        auto leaseA = std::make_shared<ReleaseLease>();
        auto sessionA = std::make_shared<media::MediaSession>(leaseA);
        DeskShareController a(sessionA);
        a.setPreferences(&prefsA);
        a.initialize();
        auto leaseB = std::make_shared<ReleaseLease>();
        auto sessionB = std::make_shared<media::MediaSession>(leaseB);
        DeskShareController b(sessionB);
        b.setPreferences(&prefsB);
        b.initialize();
        QVERIFY(waitUntil([&] {
            return a.sources().size() >= 2 && b.sources().size() >= 2;
        }, 3000));

        // A writes the address, B writes the profile; both must survive.
        a.setServerAddress(QStringLiteral("10.1.1.1:18100"));
        QVERIFY(waitUntil([&] {
            return !store.data(QStringLiteral("preferences.json")).isEmpty();
        }, 3000));
        b.setProfile(QStringLiteral("1080p30"));
        QVERIFY(waitUntil([&] {
            return QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json")))
                       .object()
                       .value(QStringLiteral("profile"))
                       .toString()
                == QStringLiteral("1080p30");
        }, 3000));
        auto object =
            QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json"))).object();
        QCOMPARE(object.value(QStringLiteral("serverAddress")).toString(),
                 QStringLiteral("10.1.1.1:18100"));
        QCOMPARE(object.value(QStringLiteral("profile")).toString(), QStringLiteral("1080p30"));

        // Reverse order: B writes the address while A changes the profile.
        // A still holds the default 720p30, so 1080p30 is a real edit for it.
        b.setServerAddress(QStringLiteral("10.2.2.2:18100"));
        a.setProfile(QStringLiteral("1080p30"));
        QVERIFY(waitUntil([&] {
            auto current =
                QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json"))).object();
            return current.value(QStringLiteral("serverAddress")).toString()
                       == QStringLiteral("10.2.2.2:18100")
                && current.value(QStringLiteral("profile")).toString()
                       == QStringLiteral("1080p30");
        }, 3000));
        QVERIFY(a.shutdown());
        QVERIFY(b.shutdown());
    }

    void sameFieldConflictUsesLastWriterAndNeverRollsBack()
    {
        FileStore store;
        FakeHost host(&store);
        store.preset(QStringLiteral("preferences.json"), QByteArray("{}"));
        Preferences prefsA(&host);
        Preferences prefsB(&host);
        auto leaseA = std::make_shared<ReleaseLease>();
        auto sessionA = std::make_shared<media::MediaSession>(leaseA);
        DeskShareController a(sessionA);
        a.setPreferences(&prefsA);
        a.initialize();
        auto leaseB = std::make_shared<ReleaseLease>();
        auto sessionB = std::make_shared<media::MediaSession>(leaseB);
        DeskShareController b(sessionB);
        b.setPreferences(&prefsB);
        b.initialize();
        QVERIFY(waitUntil([&] {
            return a.sources().size() >= 2 && b.sources().size() >= 2;
        }, 3000));
        a.setServerAddress(QStringLiteral("10.9.9.9:18100"));
        QVERIFY(waitUntil([&] {
            return QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json")))
                       .object()
                       .value(QStringLiteral("serverAddress"))
                       .toString()
                == QStringLiteral("10.9.9.9:18100");
        }, 3000));
        b.setServerAddress(QStringLiteral("10.8.8.8:18100"));
        QVERIFY(waitUntil([&] {
            return QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json")))
                       .object()
                       .value(QStringLiteral("serverAddress"))
                       .toString()
                == QStringLiteral("10.8.8.8:18100");
        }, 3000));
        // A later edit of the other field must not roll the address back.
        a.setProfile(QStringLiteral("1080p30"));
        QVERIFY(waitUntil([&] {
            return QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json")))
                       .object()
                       .value(QStringLiteral("profile"))
                       .toString()
                == QStringLiteral("1080p30");
        }, 3000));
        const auto object =
            QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json"))).object();
        QCOMPARE(object.value(QStringLiteral("serverAddress")).toString(),
                 QStringLiteral("10.8.8.8:18100"));
        QVERIFY(a.shutdown());
        QVERIFY(b.shutdown());
    }

    void clearingAFieldRemovesOnlyThatKey()
    {
        FileStore store;
        FakeHost host(&store);
        store.preset(QStringLiteral("preferences.json"),
                     QByteArray("{\"serverAddress\":\"old:1\",\"profile\":\"720p30\"}"));
        Preferences prefs(&host);
        auto lease = std::make_shared<ReleaseLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.setPreferences(&prefs);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setServerAddress(QString());
        QVERIFY(waitUntil([&] {
            return !QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json")))
                        .object()
                        .contains(QStringLiteral("serverAddress"));
        }, 3000));
        const auto object =
            QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json"))).object();
        QVERIFY(!object.contains(QStringLiteral("serverAddress")));
        QCOMPARE(object.value(QStringLiteral("profile")).toString(), QStringLiteral("720p30"));
        QVERIFY(controller.shutdown());
    }

    void failedSaveIsVisibleAndRetriedWithoutLosingTheEdit()
    {
        FileStore store;
        FakeHost host(&store);
        Preferences prefs(&host);
        auto lease = std::make_shared<ReleaseLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.setPreferences(&prefs);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));

        store.setFailWrites(true);
        controller.setServerAddress(QStringLiteral("10.5.5.5:18100"));
        QVERIFY(waitUntil([&] {
            return controller.preferencesError().contains(QStringLiteral("偏好保存失败"));
        }, 3000));
        QVERIFY(!controller.preferencesError().isEmpty());
        QVERIFY(!store.data(QStringLiteral("preferences.json")).contains("10.5.5.5"));

        // Recovery: the retained dirty field is written by the next save, which
        // is triggered by an edit of the other field.
        store.setFailWrites(false);
        controller.setProfile(QStringLiteral("1080p30"));
        QVERIFY(waitUntil([&] {
            auto current =
                QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json"))).object();
            return current.value(QStringLiteral("serverAddress")).toString()
                       == QStringLiteral("10.5.5.5:18100")
                && current.value(QStringLiteral("profile")).toString()
                       == QStringLiteral("1080p30");
        }, 3000));
        QVERIFY(waitUntil([&] { return controller.preferencesError().isEmpty(); }, 3000));
        QVERIFY(controller.shutdown());
    }
};

QTEST_GUILESS_MAIN(Stage6RemediationTests)
#include "Stage6RemediationTests.moc"
