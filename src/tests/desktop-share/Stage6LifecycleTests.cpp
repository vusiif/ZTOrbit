// Stage-6 page/lifecycle controller tests.
//
// They use the real NetworkClient (real TCP control socket and network thread),
// a scripted control host, and the public media lease contract with a fake
// provider. No host-private header and no media implementation are linked.
//
// Covered requirements:
//  * A: opening/initializing, editing the address, choosing a room code and
//    refreshing sources never connect, capture or decode (a listening control
//    host records zero accepted connections and zero control requests).
//  * B: repeated start/join clicks are coalesced; connecting/creating/joining
//    are cancellable and a cancel closes the control connection, never starts
//    capture, and leaves the page in its real remaining state.
//  * C: unload during capture waits for the network drain and for every media
//    track removal before it completes; a viewer track create that completes
//    after the stop is removed and never started; repeated stop requests share
//    one drain; the GUI heartbeat keeps ticking throughout.
//  * E: late network/media results after a stop cannot start capture, decode or
//    overwrite the terminal state.
//  * Preferences: field merging keeps foreign fields, malformed files are never
//    overwritten, and a save failure is visible in the controller.
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
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

#include <atomic>
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

QString phaseOf(DeskShareController &controller, const char *name)
{
    return controller.lifecycleStatistics().value(QLatin1String(name)).toString();
}

// Fake public media provider. It records the exact call order, can hold
// track-create/remove completions open, and exposes the bulk channel handshake
// so MediaSession::open() completes exactly as it does with the real worker.
class FakeMediaLease final : public util::ServiceLease {
public:
    struct Create {
        QString id;
        QJsonObject config;
        QPointer<QObject> context;
        util::ServiceCallback done;
    };

    vusiif::zto::protocol::service::BulkChannel channel;
    QStringList trace;
    QList<Create> creates;
    QStringList started, removed, stopped, keyframes;
    bool deferCreate = false;
    bool deferRemove = false;
    bool hideSelectedSource = false;
    bool failRemove = false;
    QList<QPair<QString, util::ServiceCallback>> pendingRemoves;
    qint64 removedAtMs = -1;

    quint64 generation() const override { return 1; }
    bool available() const override { return true; }
    void subscribe(QObject *, util::ServiceCallback) override {}

    void close(QObject *, util::ServiceCallback done) override
    {
        trace.append(QStringLiteral("close"));
        channel.close();
        if (done) done({});
    }

    void call(const QString &method, const QJsonObject &data, const QByteArray &, QObject *context,
              util::ServiceCallback done) override
    {
        trace.append(method);
        if (method == QStringLiteral("$channel")) {
            util::ServiceResult channelResult;
            channelResult.data = channel.listen(QString(32, 'a'), QStringLiteral("1"));
            done(channelResult);
            return;
        }
        if (method == QStringLiteral("sources")) {
            QJsonArray sources;
            if (!hideSelectedSource) {
                sources.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("screen-1")},
                                           {QStringLiteral("kind"), QStringLiteral("screen")},
                                           {QStringLiteral("description"), QStringLiteral("Synthetic screen")},
                                           {QStringLiteral("default"), true}});
            }
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
            trace.append(QStringLiteral("remove:") + id);
            removed.append(id);
            if (deferRemove) {
                pendingRemoves.append({id, std::move(done)});
                return;
            }
            if (done)
                QTimer::singleShot(0, context, [done, failRemove = failRemove] {
                    done(failRemove ? util::ServiceResult{{}, {}, QStringLiteral("remove failed")}
                                    : util::ServiceResult{{}, {}, {}});
                });
            return;
        }
        if (method == QStringLiteral("track.start")) {
            started.append(data.value(QStringLiteral("id")).toString());
        } else if (method == QStringLiteral("track.stop")) {
            stopped.append(data.value(QStringLiteral("id")).toString());
        } else if (method == QStringLiteral("track.keyframe")) {
            keyframes.append(data.value(QStringLiteral("id")).toString());
        }
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

    void completeRemoves()
    {
        removedAtMs = QDateTime::currentMSecsSinceEpoch();
        const auto pending = std::move(pendingRemoves);
        pendingRemoves.clear();
        for (const auto &entry : pending) {
            if (entry.second) entry.second({});
        }
    }
};

// Scripted control host. It answers hello/create/join and records stop/leave
// and connection closes. create/join answers can be delayed so a cancel races
// against a successful server-side operation.
class ScriptedRoomHost final : public QObject {
public:
    explicit ScriptedRoomHost(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = m_server.nextPendingConnection()) {
                ++m_accepted;
                m_buffers.insert(socket, QByteArray());
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this] {
                    ++m_closed;
                    trace.append(QStringLiteral("client-closed"));
                });
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
    void sendRoomClosed(quint64 roomId, CloseReason reason)
    {
        for (auto *socket : m_buffers.keys()) {
            if (!socket || socket->state() != QAbstractSocket::ConnectedState) continue;
            Event event;
            event.kind = EventKind::RoomClosed;
            event.roomId = roomId;
            event.reason = reason;
            socket->write(wire::frame(QByteArray(Route), encodeEvent(event)));
        }
    }

    QList<Operation> operations;
    QStringList trace;
    int createCount = 0;
    int joinCount = 0;
    int stopCount = 0;
    int leaveCount = 0;
    int acceptedCount() const { return m_accepted; }
    int closeCount() const { return m_closed; }
    bool silent = false;
    int createDelayMs = 0;
    int joinDelayMs = 0;

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
            operations.append(request.op);
            trace.append(operationName(request.op));
            if (request.op == Operation::Hello) {
                if (silent) continue;
                Response response;
                response.id = request.id;
                response.ok = true;
                response.op = Operation::Hello;
                response.hello.sessionId = 4242;
                response.hello.udpPort = 18199;
                response.hello.maxViewers = 8;
                response.hello.profiles = {QStringLiteral("720p30"), QStringLiteral("1080p30")};
                respond(socket, response);
                continue;
            }
            if (request.op == Operation::Create) {
                ++createCount;
                const quint32 id = request.id;
                const QString profile = request.profile;
                const int delay = createDelayMs;
                const QPointer<QTcpSocket> guard(socket);
                QTimer::singleShot(delay, this, [this, guard, id, profile] {
                    if (!guard) return;
                    Response response;
                    response.id = id;
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
                const quint32 id = request.id;
                const int delay = joinDelayMs;
                const QPointer<QTcpSocket> guard(socket);
                QTimer::singleShot(delay, this, [this, guard, id] {
                    if (!guard) return;
                    Response response;
                    response.id = id;
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
            if (request.op == Operation::Stop || request.op == Operation::Leave
                || request.op == Operation::Ping || request.op == Operation::StreamReady
                || request.op == Operation::StreamKeyframe
                || request.op == Operation::StreamFeedback) {
                Response response;
                response.id = request.id;
                response.ok = true;
                response.op = request.op;
                if (request.op == Operation::StreamReady
                    || request.op == Operation::StreamKeyframe) {
                    response.roomId = request.roomId;
                    response.streamEpoch = request.streamEpoch;
                }
                respond(socket, response);
                continue;
            }
            if (request.op == Operation::StreamConfigure) {
                Response response;
                response.id = request.id;
                response.ok = true;
                response.op = Operation::StreamConfigure;
                response.roomId = request.roomId;
                response.streamEpoch = 5;
                respond(socket, response);
                continue;
            }
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    quint64 m_roomId = 77;
    int m_accepted = 0;
    int m_closed = 0;
};

// Public host file service with the optional field-merging transaction. It
// models a second process changing a foreign field between our reads.
class MemoryStore final : public util::PluginFiles, public util::PluginStorage {
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
        m_files.insert(path, data);
        return {};
    }
    util::PluginFileTransaction updateFile(const QString &path,
                                           const util::PluginFileUpdater &updater) override
    {
        QMutexLocker lock(&m_mutex);
        util::PluginFileTransaction result;
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
        result.ok = true;
        if (update.action == util::PluginFileUpdate::Action::Keep) {
            result.data = snapshot.data;
            return result;
        }
        if (update.action == util::PluginFileUpdate::Action::Fail) {
            result.ok = false;
            result.error = update.error;
            return result;
        }
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

    bool failWrites() const { return m_failWrites; }

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
    void broadcastDataBalancedTo(const QString &, const QStringList &, const QByteArray &, QObject *,
                                 util::SendCallback done) override { done({}); }
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
    explicit FakeHost(MemoryStore *store) : m_store(store) {}
    util::PluginFiles &files() override { return *m_store; }
    util::ZeroTierService &zeroTier() override { return m_zt; }
    util::PluginLogger &logs() override { return m_logger; }

private:
    MemoryStore *m_store;
    NullZeroTier m_zt;
    NullLogger m_logger;
};

QString staticType = QStringLiteral("screen:screen-1");

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

} // namespace

class Stage6LifecycleTests final : public QObject {
    Q_OBJECT

private slots:
    // A
    void openingEditingAddressAndRefreshingSourcesNeverConnectOrCapture()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setServerAddress(
            QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.setJoinCode(QStringLiteral("00123456"));
        QTest::qWait(1200);
        QCOMPARE(host.acceptedCount(), 0);
        QCOMPARE(controller.statistics().value(QStringLiteral("tcpRequestsSent")).toString(),
                 QStringLiteral("0"));
        QCOMPARE(controller.phase(), QStringLiteral("disconnected"));
        QCOMPARE(controller.capturing(), false);
        QCOMPARE(controller.decodedFrames(), 0);
        QCOMPARE(controller.publishing(), false);
        QCOMPARE(controller.watching(), false);
        QCOMPARE(lease->creates.size(), 0);
        // A refresh is still only an enumeration.
        controller.refreshSources();
        QTest::qWait(500);
        QCOMPARE(host.acceptedCount(), 0);
        QCOMPARE(lease->creates.size(), 0);
        // Start sharing without an explicit connection must not auto-connect.
        controller.setSourceValue(staticType);
        controller.startSharing();
        QTest::qWait(300);
        QCOMPARE(host.acceptedCount(), 0);
        QCOMPARE(controller.phase(), QStringLiteral("disconnected"));
        QVERIFY(!controller.errorText().isEmpty());
        QVERIFY(!controller.nextAction().isEmpty());
        QVERIFY(controller.shutdown());
    }

    // B
    void repeatedStartJoinStopLeaveAreCoalesced()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setSourceValue(staticType);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        QCOMPARE(controller.phase(), QStringLiteral("ready"));

        controller.startSharing();
        controller.startSharing();
        controller.startSharing();
        QVERIFY(waitUntil([&] { return controller.publishing(); }, 3000));
        QCOMPARE(host.createCount, 1);
        QCOMPARE(controller.roomCode(), QStringLiteral("00000042"));
        QVERIFY(waitUntil([&] { return lease->creates.size() == 1; }, 3000));
        QCOMPARE(lease->started.size(), 1);
        QCOMPARE(controller.capturing(), true);

        // A second start while sharing must not create another room or track.
        controller.startSharing();
        QCOMPARE(host.createCount, 1);
        QCOMPARE(lease->creates.size(), 1);

        controller.stopSharing();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("ended"); }, 5000));
        QCOMPARE(host.stopCount, 1);
        QCOMPARE(controller.publishing(), false);
        QVERIFY(lease->removed.contains(QStringLiteral("track-0")));
        QCOMPARE(controller.state(), QStringLiteral("idle"));
        controller.stopSharing();
        QTest::qWait(200);
        QCOMPARE(host.stopCount, 1);
        QCOMPARE(lease->removed.size(), 1);

        // Join: repeated clicks create one member; repeated leave sends one leave.
        controller.setJoinCode(QStringLiteral("00000042"));
        controller.joinRoomExplicit();
        controller.joinRoomExplicit();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("viewerWaiting"); }, 3000));
        QCOMPARE(host.joinCount, 1);
        controller.joinRoomExplicit();
        QCOMPARE(host.joinCount, 1);
        controller.leaveRoom();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("ended"); }, 5000));
        QVERIFY(waitUntil([&] { return host.leaveCount == 1; }, 3000));
        QVERIFY(waitUntil([&] { return controller.watching() == false; }, 3000));
        QCOMPARE(controller.watching(), false);
        QVERIFY(controller.shutdown());
    }

    // B
    void cancelWhileCreatingClosesConnectionAndNeverCaptures()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        host.createDelayMs = 600;
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setSourceValue(staticType);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.startSharing();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("creating"); }, 2000));
        QVERIFY(controller.canCancel());
        controller.cancelOperation();
        QCOMPARE(controller.phase(), QStringLiteral("disconnected"));
        QCOMPARE(controller.publishing(), false);
        QCOMPARE(controller.capturing(), false);
        QVERIFY(waitUntil([&] { return host.closeCount() >= 1; }, 3000));
        QTest::qWait(900); // the delayed create answer must not revive the session
        QCOMPARE(controller.publishing(), false);
        QCOMPARE(controller.phase(), QStringLiteral("disconnected"));
        QCOMPARE(lease->creates.size(), 0);
        QCOMPARE(controller.roomCode(), QString());
        QCOMPARE(controller.statistics().value(QStringLiteral("roomActive")).toBool(), false);
        QVERIFY(controller.shutdown());
    }

    // B
    void cancelWhileConnectingThenExplicitRetrySucceeds()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        host.silent = true;
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("connecting"); }, 1000));
        QVERIFY(controller.canCancel());
        controller.cancelOperation();
        QCOMPARE(controller.phase(), QStringLiteral("disconnected"));
        QCOMPARE(controller.capturing(), false);
        QVERIFY(waitUntil([&] { return host.closeCount() >= 1; }, 3000));
        // Explicit retry against an answering host; still no capture.
        host.silent = false;
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        QCOMPARE(controller.phase(), QStringLiteral("connected"));
        QCOMPARE(lease->creates.size(), 0);
        QVERIFY(controller.shutdown());
    }

    // C + H ordering
    void unloadDuringCaptureWaitsForNetworkAndTrackDrain()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        lease->deferRemove = true;
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setSourceValue(staticType);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.startSharing();
        QVERIFY(waitUntil([&] { return controller.publishing(); }, 3000));
        QVERIFY(waitUntil([&] { return lease->creates.size() == 1 && lease->started.size() == 1; }, 3000));
        QCOMPARE(controller.capturing(), true);

        int heartbeat = 0;
        QTimer ticker;
        connect(&ticker, &QTimer::timeout, this, [&] { ++heartbeat; });
        ticker.start(10);
        bool done = false;
        bool ok = false;
        qint64 doneAt = -1;
        controller.shutdownAsync([&](bool result) {
            done = true;
            ok = result;
            doneAt = QDateTime::currentMSecsSinceEpoch();
        }, 8000);
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("stopping"); }, 1000));
        QTest::qWait(400);
        QVERIFY2(!done, "stop completed before media track removal was confirmed");
        QVERIFY2(heartbeat >= 8, qPrintable(QStringLiteral("GUI heartbeat stalled: %1").arg(heartbeat)));
        QCOMPARE(host.stopCount, 1);
        QVERIFY(lease->trace.contains(QStringLiteral("remove:track-0")));
        QVERIFY(controller.lifecycleStatistics().value(QStringLiteral("networkDrained")).toBool());
        QCOMPARE(controller.lifecycleStatistics().value(QStringLiteral("tracksPending")).toBool(), false);
        lease->completeRemoves();
        QVERIFY(waitUntil([&] { return done; }, 3000));
        QVERIFY(ok);
        QVERIFY(lease->removedAtMs > 0);
        QVERIFY2(doneAt >= lease->removedAtMs,
                 "shutdown completion was reported before the media removal callback");
        QCOMPARE(controller.state(), QStringLiteral("stopped"));
        QCOMPARE(controller.phase(), QStringLiteral("ended"));
        QCOMPARE(controller.lifecycleStatistics().value(QStringLiteral("mediaDrained")).toBool(), true);
        QCOMPARE(controller.lifecycleStatistics().value(QStringLiteral("sendTrack")).toString(), QString());
        QCOMPARE(controller.publishing(), false);
        QVERIFY(waitUntil([&] { return host.closeCount() >= 1; }, 3000));
        ticker.stop();

        // Repeated exit requests complete immediately and never send a second
        // stop or remove request.
        int second = 0;
        controller.shutdownAsync([&](bool result) {
            ++second;
            QVERIFY(result);
        }, 1000);
        QVERIFY(waitUntil([&] { return second == 1; }, 1000));
        QCOMPARE(host.stopCount, 1);
        QCOMPARE(lease->removed.size(), 1);
    }

    // C + E: a viewer track create that completes after the stop is removed and
    // never started.
    void viewerTrackCreatedAfterStopIsRemovedNotStarted()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        lease->deferCreate = true;
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.setJoinCode(QStringLiteral("00000042"));
        controller.joinRoomExplicit();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("viewerWaiting"); }, 3000));
        StreamFormat format;
        format.codec = QStringLiteral("h264");
        format.width = 1280;
        format.height = 720;
        format.fps = 30;
        format.bitrate = 2000000;
        format.streamEpoch = 3;
        auto *viewerNetwork = controller.findChild<NetworkClient *>();
        QVERIFY(viewerNetwork);
        emit viewerNetwork->streamConfigReceived(host.roomId(), format);
        QVERIFY(waitUntil([&] { return lease->creates.size() == 1; }, 3000));
        bool done = false;
        controller.shutdownAsync([&](bool) { done = true; }, 5000);
        QVERIFY(waitUntil([&] { return done; }, 4000));
        QCOMPARE(controller.phase(), QStringLiteral("ended"));
        // Late create completion: the controller context is still alive but the
        // generation is invalid, so it must remove the track and never start it.
        lease->completeCreate(0, QStringLiteral("late-viewer-track"));
        QVERIFY(waitUntil([&] {
            return lease->removed.contains(QStringLiteral("late-viewer-track"));
        }, 2000));
        QVERIFY(!lease->started.contains(QStringLiteral("late-viewer-track")));
        QCOMPARE(controller.watching(), false);
        QCOMPARE(controller.decodedFrames(), 0);
    }

    // E
    void lateNetworkAndMediaResultsCannotRestartOrOverwriteStoppedState()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setSourceValue(staticType);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.startSharing();
        QVERIFY(waitUntil([&] { return controller.publishing(); }, 3000));
        QVERIFY(waitUntil([&] { return lease->creates.size() == 1; }, 3000));
        controller.stopSharing();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("ended"); }, 5000));
        const int captured = controller.capturedFrames();
        const int decoded = controller.decodedFrames();
        const int creates = lease->creates.size();
        auto *network = controller.findChild<NetworkClient *>();
        QVERIFY(network);
        QVERIFY(controller.connected());

        // Old socket/room/stream notifications after the stop.
        emit network->connected(999, 1234);
        emit network->roomCreated(999, QStringLiteral("99999999"), QStringLiteral("720p30"),
                                  QByteArray(16, 'z'), false, {});
        emit network->roomJoined(999, QStringLiteral("99999999"), QStringLiteral("720p30"),
                                 QByteArray(16, 'z'), false, {});
        StreamFormat format;
        format.codec = QStringLiteral("bgra-tiles");
        format.width = 1280;
        format.height = 720;
        format.fps = 30;
        format.bitrate = 2000000;
        format.streamEpoch = 9;
        emit network->streamConfigReceived(999, format);
        emit network->roomClosed(999, int(CloseReason::OwnerStopped));
        // Old media callbacks for the removed send track.
        appendSyntheticPacket(*session, QStringLiteral("track-0"), 1000, true);
        appendSyntheticPacket(*session, QStringLiteral("track-0"), 2000, false);
        const qint64 uploadsBefore =
            controller.statistics().value(QStringLiteral("uploadPackets")).toString().toLongLong();
        QTest::qWait(500);
        const qint64 uploadsAfter =
            controller.statistics().value(QStringLiteral("uploadPackets")).toString().toLongLong();
        QCOMPARE(uploadsAfter, uploadsBefore);
        QCOMPARE(controller.capturedFrames(), captured);
        QCOMPARE(controller.decodedFrames(), decoded);
        QCOMPARE(lease->creates.size(), creates);
        QCOMPARE(controller.publishing(), false);
        QCOMPARE(controller.roomCode(), QString());
        QCOMPARE(controller.statistics().value(QStringLiteral("roomActive")).toBool(), false);
        QVERIFY2(controller.phase() == QStringLiteral("ended")
                     || controller.phase() == QStringLiteral("failed"),
                 qPrintable(controller.phase()));
        QVERIFY(controller.shutdown());
    }

    // D: TCP loss while sharing cleans the room identity, stops media delivery
    // and never resumes capture on its own.
    void tcpDisconnectDuringSharingCleansRoomAndStopsCapture()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setSourceValue(staticType);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.startSharing();
        QVERIFY(waitUntil([&] { return controller.publishing(); }, 3000));
        QVERIFY(waitUntil([&] { return lease->started.size() == 1; }, 3000));
        const int captured = controller.capturedFrames();
        host.disconnectClients();
        QVERIFY(waitUntil([&] { return !controller.publishing(); }, 5000));
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("disconnected"); }, 3000));
        QVERIFY(!controller.errorText().isEmpty());
        QVERIFY(!controller.nextAction().isEmpty());
        QCOMPARE(controller.roomCode(), QString());
        QCOMPARE(controller.statistics().value(QStringLiteral("roomActive")).toBool(), false);
        QVERIFY(waitUntil([&] { return !lease->removed.isEmpty(); }, 3000));
        QCOMPARE(controller.capturing(), false);
        appendSyntheticPacket(*session, QStringLiteral("track-0"), 1000, true);
        QTest::qWait(300);
        QCOMPARE(controller.capturedFrames(), captured);
        const auto uploads =
            controller.statistics().value(QStringLiteral("uploadPackets")).toString();
        appendSyntheticPacket(*session, QStringLiteral("track-0"), 2000, false);
        QTest::qWait(300);
        QCOMPARE(controller.statistics().value(QStringLiteral("uploadPackets")).toString(), uploads);
        QVERIFY(controller.shutdown());
    }

    // D: an unreachable UDP media endpoint ends the share instead of leaving a
    // half-alive room, and the control connection remains reusable.
    void udpRegistrationFailureEndsSharingAndCleansTracks()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setSourceValue(staticType);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.startSharing();
        QVERIFY(waitUntil([&] { return controller.publishing(); }, 3000));
        QVERIFY(waitUntil([&] { return lease->started.size() == 1; }, 3000));
        // The scripted host never answers UDP registration, so the media endpoint
        // is declared unavailable after the bounded retry window.
        QVERIFY2(waitUntil([&] { return !controller.publishing(); }, 12000),
                 qPrintable(controller.statusText()));
        QVERIFY(!controller.errorText().isEmpty());
        QVERIFY2(controller.errorText().contains(QStringLiteral("媒体端点")),
                 qPrintable(controller.errorText()));
        QCOMPARE(controller.roomCode(), QString());
        QVERIFY(waitUntil([&] { return !lease->removed.isEmpty(); }, 3000));
        QVERIFY(controller.connected());
        QCOMPARE(controller.statistics().value(QStringLiteral("roomActive")).toBool(), false);
        QVERIFY(controller.shutdown());
    }

    void staticSilenceWaitsButObservedLossStillInterrupts()
    {
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        StreamFormat format;
        format.codec = QStringLiteral("h264");
        format.width = 1280; format.height = 720; format.fps = 30;
        format.bitrate = 2000000; format.streamEpoch = 3;
        auto *network = controller.findChild<NetworkClient *>();
        QVERIFY(network);
        emit network->roomJoined(42, QStringLiteral("00000042"), QStringLiteral("720p30"),
                                 QByteArray(16, 'a'), true, format);
        QVERIFY(waitUntil([&] { return controller.watching(); }, 3000));
        media::FrameView frame;
        frame.track = lease->started.first(); frame.decoded = true;
        frame.video.width = format.width; frame.video.height = format.height;
        session->frame(frame);
        QTest::qWait(11000);
        QCOMPARE(controller.phase(), QStringLiteral("waitingNewFrame"));
        QVERIFY(controller.watching());
        emit network->streamGapDetected();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("mediaInterrupted"); }, 1000));
        session->frame(frame);
        QCOMPARE(controller.phase(), QStringLiteral("viewing"));
        QVERIFY(controller.shutdown());
    }

    // D: a server-side room close ends the viewer role and releases the track.
    void serverRoomCloseEndsWatchingAndReleasesTrack()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.setJoinCode(QStringLiteral("00000042"));
        controller.joinRoomExplicit();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("viewerWaiting"); }, 3000));
        StreamFormat format;
        format.codec = QStringLiteral("h264");
        format.width = 1280;
        format.height = 720;
        format.fps = 30;
        format.bitrate = 2000000;
        format.streamEpoch = 3;
        auto *network = controller.findChild<NetworkClient *>();
        QVERIFY(network);
        emit network->streamConfigReceived(host.roomId(), format);
        QVERIFY(waitUntil([&] { return lease->started.size() == 1; }, 3000));
        QCOMPARE(controller.watching(), true);
        const auto active = controller.recoveryObservation();
        QCOMPARE(active.value(QStringLiteral("state")).toString(), QStringLiteral("active"));
        const auto stream = active.value(QStringLiteral("stream")).toMap();
        QCOMPARE(stream.value(QStringLiteral("streamEpoch")).toString(), QStringLiteral("3"));
        QVERIFY(active.value(QStringLiteral("failure")).toMap().isEmpty());
        host.sendRoomClosed(host.roomId(), CloseReason::OwnerStopped);
        QVERIFY(waitUntil([&] { return !controller.watching(); }, 5000));
        QCOMPARE(controller.phase(), QStringLiteral("failed"));
        QVERIFY(!controller.errorText().isEmpty());
        QVERIFY(waitUntil([&] { return !lease->removed.isEmpty(); }, 3000));
        QCOMPARE(controller.roomCode(), QString());
        const auto terminal = controller.recoveryObservation();
        QCOMPARE(terminal.value(QStringLiteral("state")).toString(), QStringLiteral("terminal"));
        const auto failure = terminal.value(QStringLiteral("failure")).toMap();
        QCOMPARE(failure.value(QStringLiteral("reasonCode")).toString(), QStringLiteral("ROOM_CLOSED"));
        QCOMPARE(failure.value(QStringLiteral("stream")).toMap(), stream);
        QVERIFY(failure.value(QStringLiteral("visibleEpochMs")).toDouble()
                >= failure.value(QStringLiteral("occurredEpochMs")).toDouble());
        controller.joinRoomExplicit();
        QVERIFY(controller.recoveryObservation().value(QStringLiteral("failure")).toMap().isEmpty());
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("viewerWaiting"); }, 3000));
        emit network->streamConfigReceived(host.roomId(), format);
        QVERIFY(waitUntil([&] { return controller.watching(); }, 3000));
        const auto nextStream = controller.recoveryObservation().value(QStringLiteral("stream")).toMap();
        QVERIFY(nextStream != stream);
        emit network->udpReady(false, QStringLiteral("probe timeout"));
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("failed"); }, 5000));
        const auto udp = controller.recoveryObservation().value(QStringLiteral("failure")).toMap();
        QCOMPARE(udp.value(QStringLiteral("reasonCode")).toString(), QStringLiteral("UDP_ENDPOINT_UNAVAILABLE"));
        QCOMPARE(udp.value(QStringLiteral("stream")).toMap(), nextStream);
        QVERIFY(controller.shutdown());
    }

    // D: a media-provider failure ends the session, releases tracks and does not
    // silently restart capture.
    void mediaWorkerFailureEndsSharingWithoutAutoRestart()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setSourceValue(staticType);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.startSharing();
        QVERIFY(waitUntil([&] { return controller.publishing(); }, 3000));
        QVERIFY(waitUntil([&] { return lease->started.size() == 1; }, 3000));
        const int creates = lease->creates.size();
        QVERIFY(session->error);
        session->error({QStringLiteral("worker"), QStringLiteral("media worker exited")});
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("failed"); }, 5000));
        QCOMPARE(controller.phase(), QStringLiteral("failed"));
        QVERIFY(controller.errorText().contains(QStringLiteral("媒体")));
        QVERIFY(waitUntil([&] { return !lease->removed.isEmpty(); }, 3000));
        QTest::qWait(800);
        QCOMPARE(lease->creates.size(), creates);
        QCOMPARE(controller.capturing(), false);
        QVERIFY(controller.shutdown());
    }

    // D: the selected source disappearing for longer than the grace period ends
    // the share; it never switches to another source.
    void sourceWindowClosingEndsSharingWithoutSwitchingSource()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setSourceValue(staticType);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.startSharing();
        QVERIFY(waitUntil([&] { return controller.publishing(); }, 3000));
        QVERIFY(waitUntil([&] { return lease->started.size() == 1; }, 3000));
        lease->hideSelectedSource = true;
        controller.refreshSources();
        // The selection is preserved during capture while the source is missing.
        QTest::qWait(500);
        QCOMPARE(controller.sourceValue(), staticType);
        QVERIFY(controller.publishing());
        QVERIFY2(waitUntil([&] { return !controller.publishing(); }, 6000),
                 qPrintable(controller.statusText()));
        QCOMPARE(controller.phase(), QStringLiteral("failed"));
        QVERIFY2(controller.errorText().contains(QStringLiteral("来源")),
                 qPrintable(controller.errorText()));
        QCOMPARE(controller.sourceValue(), staticType);
        QVERIFY(waitUntil([&] { return !lease->removed.isEmpty(); }, 3000));
        QVERIFY(!controller.canStartShare());
        QVERIFY(controller.shutdown());
    }

    // H: after a successful stop, no new capture or upload happens for at least
    // five seconds even when old media packets are replayed.
    void noCaptureOrUploadForFiveSecondsAfterStop()
    {
        ScriptedRoomHost host;
        QVERIFY(host.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setSourceValue(staticType);
        controller.setServerAddress(QStringLiteral("127.0.0.1:%1").arg(host.port()));
        controller.connectServer();
        QVERIFY(waitUntil([&] { return controller.connected(); }, 3000));
        controller.startSharing();
        QVERIFY(waitUntil([&] { return controller.publishing(); }, 3000));
        QVERIFY(waitUntil([&] { return lease->started.size() == 1; }, 3000));
        appendSyntheticPacket(*session, QStringLiteral("track-0"), 1000, true);
        QTest::qWait(400);
        controller.stopSharing();
        QVERIFY(waitUntil([&] { return controller.phase() == QStringLiteral("ended"); }, 5000));
        const int captured = controller.capturedFrames();
        const auto uploads =
            controller.statistics().value(QStringLiteral("uploadPackets")).toString();
        QElapsedTimer timer;
        timer.start();
        qint64 pts = 2000;
        while (timer.elapsed() < 5000) {
            appendSyntheticPacket(*session, QStringLiteral("track-0"), pts, true);
            pts += 33333;
            QTest::qWait(100);
        }
        QCOMPARE(controller.capturedFrames(), captured);
        QCOMPARE(controller.statistics().value(QStringLiteral("uploadPackets")).toString(), uploads);
        QCOMPARE(controller.lifecycleStatistics().value(QStringLiteral("acceptingFrames")).toBool(),
                 false);
        QVERIFY(controller.shutdown());
    }

    // Preferences: field merge keeps fields owned by other writers/processes.
    void preferencesMergePreservesForeignFields()
    {
        MemoryStore store;
        FakeHost host(&store);
        store.preset(QStringLiteral("preferences.json"),
                     QByteArray("{\"foreign\":\"keep\",\"serverAddress\":\"old:1\",\"profile\":\"720p30\"}"));
        Preferences prefs(&host);
        const auto loaded = prefs.load();
        QVERIFY2(loaded.error.isEmpty(), qPrintable(loaded.error));
        QCOMPARE(loaded.serverAddress, QStringLiteral("old:1"));
        QCOMPARE(loaded.profile, QStringLiteral("720p30"));
        QString error;
        Preferences::Change change;
        change.serverAddress = QStringLiteral("10.0.0.5:18100");
        change.profile = QStringLiteral("1080p30");
        change.serverAddressDirty = true;
        change.profileDirty = true;
        QVERIFY2(prefs.save(change, nullptr, &error), qPrintable(error));
        auto object = QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json"))).object();
        QCOMPARE(object.value(QStringLiteral("foreign")).toString(), QStringLiteral("keep"));
        QCOMPARE(object.value(QStringLiteral("serverAddress")).toString(),
                 QStringLiteral("10.0.0.5:18100"));
        QCOMPARE(object.value(QStringLiteral("profile")).toString(), QStringLiteral("1080p30"));
        // Another process changes a foreign field and the profile; our next
        // address-only update must not restore the stale snapshot.
        store.preset(QStringLiteral("preferences.json"),
                     QByteArray("{\"foreign\":\"other-process\",\"serverAddress\":\"10.0.0.5:18100\","
                                "\"profile\":\"1080p30\"}"));
        // Address-only edit: the profile belongs to the other process now, so
        // this transaction must not write it back.
        Preferences::Change addressOnly;
        addressOnly.serverAddress = QStringLiteral("10.0.0.6:18100");
        addressOnly.serverAddressDirty = true;
        addressOnly.profile = QStringLiteral("720p30");
        QVERIFY(prefs.save(addressOnly, nullptr, &error));
        object = QJsonDocument::fromJson(store.data(QStringLiteral("preferences.json"))).object();
        QCOMPARE(object.value(QStringLiteral("foreign")).toString(),
                 QStringLiteral("other-process"));
        QCOMPARE(object.value(QStringLiteral("serverAddress")).toString(),
                 QStringLiteral("10.0.0.6:18100"));
        // Room codes and tokens are never written.
        QVERIFY(!object.contains(QStringLiteral("roomCode")));
        QVERIFY(!object.contains(QStringLiteral("token")));
        QVERIFY(!object.contains(QStringLiteral("joinCode")));
    }

    void malformedPreferencesAreNotOverwrittenAndFailureIsVisible()
    {
        MemoryStore store;
        FakeHost host(&store);
        const QByteArray corrupt("{not json");
        store.preset(QStringLiteral("preferences.json"), corrupt);
        Preferences prefs(&host);
        const auto loaded = prefs.load();
        QVERIFY(!loaded.error.isEmpty());
        QString error;
        Preferences::Change change;
        change.serverAddress = QStringLiteral("10.0.0.5:18100");
        change.profile = QStringLiteral("720p30");
        change.serverAddressDirty = true;
        change.profileDirty = true;
        QVERIFY(!prefs.save(change, nullptr, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(store.data(QStringLiteral("preferences.json")), corrupt);

        // A controller exposes a failed save instead of dropping it silently.
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.setPreferences(&prefs);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        controller.setServerAddress(QStringLiteral("10.0.0.7:18100"));
        QVERIFY(waitUntil([&] {
            return controller.preferencesError().contains(QStringLiteral("偏好保存失败"));
        }, 3000));
        QVERIFY2(controller.preferencesError().contains(QStringLiteral("偏好")),
                 qPrintable(controller.preferencesError()));
        QCOMPARE(store.data(QStringLiteral("preferences.json")), corrupt);
        QVERIFY(controller.shutdown());
    }

    // A saved address is loaded on the next page; opening it still does not
    // connect or capture.
    void savedAddressAndProfileLoadWithoutNetwork()
    {
        MemoryStore store;
        FakeHost host(&store);
        store.preset(QStringLiteral("preferences.json"),
                     QByteArray("{\"serverAddress\":\"10.9.8.7:18100\",\"profile\":\"1080p30\"}"));
        Preferences prefs(&host);
        ScriptedRoomHost control;
        QVERIFY(control.listen());
        auto lease = std::make_shared<FakeMediaLease>();
        auto session = std::make_shared<media::MediaSession>(lease);
        DeskShareController controller(session);
        controller.setPreferences(&prefs);
        controller.initialize();
        QVERIFY(waitUntil([&] { return controller.sources().size() >= 2; }, 3000));
        QCOMPARE(controller.serverAddress(), QStringLiteral("10.9.8.7:18100"));
        QCOMPARE(controller.profile(), QStringLiteral("1080p30"));
        QTest::qWait(400);
        QCOMPARE(controller.phase(), QStringLiteral("disconnected"));
        QCOMPARE(lease->creates.size(), 0);
        QCOMPARE(controller.capturing(), false);
        QCOMPARE(control.acceptedCount(), 0);
        QVERIFY(controller.shutdown());
    }
};

QTEST_GUILESS_MAIN(Stage6LifecycleTests)
#include "Stage6LifecycleTests.moc"
