// Stage-5 remediation network coverage (2026-10-24 round R4/R1-residual/R6).
//
// Every case below drives the real NetworkClient network thread against a real
// loopback UDP receiver that records every ZTS1 datagram. The assertions are
// wire-level: total transmitted ZTS1 bytes must stay inside the burst plus the
// profile rate for the elapsed time, an unadmitted packet may not free budget,
// a deferred admission yields to a fresher key frame, and the GUI notification
// board stays at a fixed plateau while the GUI thread is blocked.

#include "NetworkClient.h"

#include <desktop-share/PublishQueue.h>
#include <ztprotocol/WireProtocol.h>

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkDatagram>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUdpSocket>

#include <atomic>

#include <QtTest>

using namespace vusiif::zto::desktop_share;
namespace wire = vusiif::zto::protocol;

namespace {

constexpr qint64 BurstBytes = 1024 * 1024;
constexpr qint64 MaxAgeUs = 200000;

MediaHeader identity()
{
    MediaHeader header;
    header.type = MediaType::Video;
    header.token.fill(0x41);
    header.sessionId = 7;
    header.roomId = 11;
    header.streamEpoch = 3;
    return header;
}

VideoPacket packet(quint64 sequence, int bytes, bool keyFrame, bool discontinuity = false)
{
    VideoPacket value;
    value.token.fill(0x41);
    value.sessionId = 7;
    value.roomId = 11;
    value.streamEpoch = 3;
    value.sequence = sequence;
    value.pts = qint64(sequence) * 33333;
    value.duration = 33333;
    if (keyFrame) value.flags |= quint16(MediaFlag::KeyFrame);
    if (discontinuity) value.flags |= quint16(MediaFlag::Discontinuity);
    value.data = QByteArray(bytes, char('a' + int(sequence % 26)));
    return value;
}

int fragmentsFor(int payloadBytes)
{
    return fragmentVideo(packet(0, payloadBytes, true)).size();
}

// Rate used by the publisher for a profile bitrate: bitrate / 8 * 1.25.
double profileRate(int bitrate) { return double(bitrate) / 8.0 * 1.25; }

// Wire budget available to a publisher that starts at `startUs`: the full burst
// plus everything the bucket refilled until `endUs`.
qint64 budgetBound(qint64 startUs, qint64 endUs, int bitrate)
{
    const double elapsedUs = double(qMax<qint64>(0, endUs - startUs));
    return BurstBytes + qint64(elapsedUs / 1'000'000.0 * profileRate(bitrate));
}

class RecordingHost : public QObject {
public:
    explicit RecordingHost(const MediaHeader &id, bool rejectControl = false)
        : m_rejectControl(rejectControl), m_identity(id), m_assembler(id)
    {        m_timer.start();
        m_udp.bind(QHostAddress(QHostAddress::LocalHost), 0);
        m_udp.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
        m_tcp.listen(QHostAddress(QHostAddress::LocalHost), 0);
        connect(&m_tcp, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = m_tcp.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    m_buffers[socket] += socket->readAll();
                    wire::Packet frame;
                    while (wire::takeFrame(m_buffers[socket], frame) == wire::ReadResult::Packet) {
                        Request request;
                        if (!decodeRequest(frame.payload, request).ok) continue;
                        Response response;
                        response.id = request.id;
                        response.op = request.op;
                        response.ok = true;
                        if (request.op == Operation::Hello) {
                            response.hello.sessionId = m_identity.sessionId;
                            response.hello.udpPort = m_udp.localPort();
                            response.hello.maxViewers = 8;
                            response.hello.profiles = {QStringLiteral("720p30")};
                        } else if (m_rejectControl && (request.op == Operation::Create
                                                      || request.op == Operation::Join)) {
                            response.ok = false;
                            const auto op = operationName(request.op);
                            const int attempt = ++m_rejected[op];
                            response.error = attempt == 1 ? ErrorCode::Capacity : ErrorCode::RoomFull;
                            response.message = op + QStringLiteral("-%1").arg(attempt);
                        } else if (request.op == Operation::Create) {
                            response.room.roomId = m_identity.roomId;
                            response.room.roomCode = QStringLiteral("12345678");
                            response.room.profile = QStringLiteral("720p30");
                            response.room.role = Role::Publisher;
                            response.room.token = QByteArray(16, 'A');
                        } else if (request.op == Operation::StreamConfigure) {
                            response.roomId = m_identity.roomId;
                            response.streamEpoch = m_identity.streamEpoch;
                        }
                        socket->write(wire::frame(QByteArray(Route), encodeResponse(response)));
                    }
                });
            }
        });
        connect(&m_udp, &QUdpSocket::readyRead, this, [this] {
            while (m_udp.hasPendingDatagrams()) {
                const auto datagram = m_udp.receiveDatagram();
                QByteArray payload;
                if (!unwrapZts1(datagram.data(), payload)) continue;
                DecodedDatagram decoded;
                if (!decodeDatagram(payload, decoded)) continue;
                if (decoded.header.type == MediaType::Register) {
                    if (!allowAck.load()) continue;
                    m_udp.writeDatagram(
                        wrapZts1(encodeRegisterAck(m_identity.token, m_identity.sessionId,
                                                   m_identity.roomId)),
                        datagram.senderAddress(), datagram.senderPort());
                    continue;
                }
                if (decoded.header.type != MediaType::Video) continue;
                wireBytes += datagram.data().size();
                const auto sequence = decoded.header.sequence;
                if (!count.contains(sequence)) first[sequence] = elapsedUs();
                count[sequence]++;
                last[sequence] = elapsedUs();
                VideoPacket complete;
                if (m_assembler.push(payload, last[sequence], complete)
                    == AssemblyResult::Complete) {
                    completed.append(sequence);
                }
            }
        });
    }

    qint64 elapsedUs() const { return m_timer.nsecsElapsed() / 1000; }

    // Withdraws the register acknowledgement so the client observes a real UDP
    // idle failure; used by the R7 generation-isolation cases only.
    std::atomic<bool> allowAck{true};
    QTcpServer m_tcp;
    QUdpSocket m_udp;
    qint64 wireBytes = 0;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QMap<quint64, int> count;
    QMap<quint64, qint64> first;
    QMap<quint64, qint64> last;
    QList<quint64> completed;
    const bool m_rejectControl;
    QHash<QString, int> m_rejected;

private:
    MediaHeader m_identity;
    QElapsedTimer m_timer;
    PacketReassembler m_assembler;
};

struct Fixture {
    RecordingHost host{identity()};
    NetworkClient client;
    QSignalSpy connected{&client, &NetworkClient::connected};
    QSignalSpy created{&client, &NetworkClient::roomCreated};
    QSignalSpy registered{&client, &NetworkClient::udpReady};
    QSignalSpy configured{&client, &NetworkClient::streamConfigured};
    QSignalSpy dropped{&client, &NetworkClient::uploadDropped};

    bool open(int bitrate = 2000000, const QString &codec = QStringLiteral("h264"))
    {
        client.start();
        client.connectToServer(QStringLiteral("127.0.0.1"), host.m_tcp.serverPort());
        if (!QTest::qWaitFor([&] { return connected.size() == 1; }, 3000)) return false;
        client.createRoom(QStringLiteral("720p30"));
        if (!QTest::qWaitFor([&] { return created.size() == 1; }, 3000)) return false;
        if (!QTest::qWaitFor(
                [&] { return registered.size() > 0 && registered.last().at(0).toBool(); }, 3000))
            return false;
        StreamFormat format;
        format.codec = codec;
        format.width = 1280;
        format.height = 720;
        format.fps = 30;
        format.bitrate = bitrate;
        client.configureStream(format);
        return QTest::qWaitFor([&] { return configured.size() == 1; }, 3000);
    }

    // Blocks the network thread's event loop like a long production turn.
    void stallNetworkThread(int milliseconds)
    {
        auto *thread = client.findChild<QThread *>();
        if (!thread) return;
        auto *dispatcher = QAbstractEventDispatcher::instance(thread);
        if (!dispatcher) return;
        std::atomic<bool> busy{false};
        QMetaObject::invokeMethod(
            dispatcher, [&busy, milliseconds] { busy = true; QThread::msleep(milliseconds); },
            Qt::QueuedConnection);
        QTest::qWaitFor([&] { return busy.load(); }, 2000);
    }

    NetworkStats stats() const { return client.stats(); }
};

// Runs the recording host on its own event loop, so the GUI thread can stay
// blocked across a real UDP idle deadline while control responses and UDP
// registration keep working. The destructor always closes the sockets and joins
// the thread, also when an assertion returns early.
struct ThreadedRoom {
    explicit ThreadedRoom(bool rejectControl = false)
    {
        m_main = QThread::currentThread();
        m_thread.start();
        host = new RecordingHost(identity(), rejectControl);
        host->m_tcp.moveToThread(&m_thread);
        host->m_udp.moveToThread(&m_thread);
        host->moveToThread(&m_thread);
    }
    ~ThreadedRoom()
    {
        if (m_thread.isRunning()) {
            QMetaObject::invokeMethod(
                host,
                [this] {
                    host->m_tcp.close();
                    host->m_udp.close();
                    host->m_tcp.moveToThread(m_main);
                    host->m_udp.moveToThread(m_main);
                    host->moveToThread(m_main);
                },
                Qt::BlockingQueuedConnection);
            m_thread.quit();
            m_thread.wait();
        }
        delete host;
    }

    bool open(int bitrate = 2000000, const QString &codec = QStringLiteral("h264"))
    {
        client.start();
        client.connectToServer(QStringLiteral("127.0.0.1"), host->m_tcp.serverPort());
        if (!QTest::qWaitFor([&] { return connected.size() == 1; }, 3000)) return false;
        client.createRoom(QStringLiteral("720p30"));
        if (!QTest::qWaitFor([&] { return created.size() == 1; }, 3000)) return false;
        if (!QTest::qWaitFor(
                [&] { return registered.size() > 0 && registered.last().at(0).toBool(); }, 3000))
            return false;
        StreamFormat format;
        format.codec = codec;
        format.width = 1280;
        format.height = 720;
        format.fps = 30;
        format.bitrate = bitrate;
        client.configureStream(format);
        return QTest::qWaitFor([&] { return configured.size() == 1; }, 3000);
    }

    NetworkStats stats() const { return client.stats(); }

    RecordingHost *host = nullptr;
    QThread m_thread;
    QThread *m_main = nullptr;
    NetworkClient client;
    QSignalSpy connected{&client, &NetworkClient::connected};
    QSignalSpy created{&client, &NetworkClient::roomCreated};
    QSignalSpy configured{&client, &NetworkClient::streamConfigured};
    QSignalSpy registered{&client, &NetworkClient::udpReady};
};

} // namespace

class Stage5RemediationNetworkTests : public QObject {
    Q_OBJECT

private slots:
    void tileBaselineChunksRetainThePendingRegionWithinAgeBudget()
    {
        Fixture f;
        QVERIFY(f.open(2000000, QStringLiteral("bgra-tiles")));
        const int priming=950000;
        QVERIFY(f.client.enqueueVideo(packet(0,priming,true)));
        QTRY_VERIFY_WITH_TIMEOUT(f.host.completed.contains(0),4000);
        const auto available=f.stats().publishBudgetBytes;
        QVERIFY(available<200000);
        const int pending=int(available)+20000;
        QVERIFY(f.client.enqueueVideo(packet(1,pending,true)));
        QVERIFY(f.client.enqueueVideo(packet(2,1000,true)));
        QTRY_VERIFY_WITH_TIMEOUT(f.host.completed.contains(1),2000);
        QTRY_VERIFY_WITH_TIMEOUT(f.host.completed.contains(2),2000);
        QVERIFY(f.host.first.value(1)<f.host.first.value(2));
        QCOMPARE(f.stats().uploadPacketsDroppedAge,quint64(0));
        QVERIFY(f.client.shutdown());
    }
    // R4/P1: the rejected packet never reserved anything, so the following legal
    // large packet must still wait for the bucket to refill. Total wire bytes must
    // stay inside burst + rate * elapsed; the old defect overshot by ~850 KB.
    void consecutiveUnadmittedPacketsNeverMintPacingCredit()
    {
        Fixture f;
        QVERIFY(f.open());
        const int large = 900000;
        const int tooLarge = 1040000;
        const int fragments = fragmentsFor(large);
        QVERIFY(fragments > 0);
        const qint64 start = f.host.elapsedUs();

        QVERIFY(f.client.enqueueVideo(packet(0, large, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(0), fragments, 4000);
        // Two protocol-legal packets whose complete wire cost exceeds the 1 MiB
        // burst are rejected before admission, back to back.
        QVERIFY(f.client.enqueueVideo(packet(1, tooLarge, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.stats().uploadPacketsDroppedPacing, quint64(1), 3000);
        QVERIFY(f.client.enqueueVideo(packet(2, tooLarge, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.stats().uploadPacketsDroppedPacing, quint64(2), 3000);
        QVERIFY2(!f.host.count.contains(1) && !f.host.count.contains(2),
                 "a rejected packet still wrote fragments");
        QVERIFY(f.host.completed.contains(0));

        const qint64 enqueued3 = f.host.elapsedUs();
        QVERIFY(f.client.enqueueVideo(packet(3, large, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(3), fragments, 8000);
        const qint64 end = f.host.last.value(3);
        const qint64 bound = budgetBound(start, end, 2000000);
        const qint64 expected = 2 * qint64(fragments) * 1232;
        qInfo() << "R4 wireBytes" << f.host.wireBytes << "expected" << expected << "bound" << bound
                << "elapsedUs" << end - start << "thirdFirstUs" << f.host.first.value(3)
                << "thirdEnqueuedUs" << enqueued3
                << "waitedUs" << f.host.first.value(3) - enqueued3
                << "pacingDrops" << f.stats().uploadPacketsDroppedPacing;
        QCOMPARE(f.host.wireBytes, expected);
        QVERIFY2(f.host.wireBytes <= bound,
                 "dropping two unadmitted packets created pacing credit: transmitted bytes exceed "
                 "burst + profile rate x1.25");
        QVERIFY(f.client.shutdown());
    }

    // R1 residual: a deferred admission must not hold the send slot in front of a
    // fresher key frame. Runs both codecs' payload shapes through the same path.
    void deferredAdmissionYieldsToANewerKeyFrameForBothCodecs()
    {
        const QList<QPair<QString, int>> codecs{
            {QStringLiteral("h264"), 5000},
            {QStringLiteral("bgra-tiles"), 9000},
        };
        for (const auto &codec : codecs) {
            Fixture f;
            QVERIFY2(f.open(2000000,codec.first), qPrintable(codec.first));
            const int large = 900000;
            const int fragments = fragmentsFor(large);
            QVERIFY(f.client.enqueueVideo(packet(0, large, true)));
            QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(0), fragments, 4000);

            const qint64 enqueuedOld = f.host.elapsedUs();
            QVERIFY(f.client.enqueueVideo(packet(1, large, true)));
            // The bucket can only afford this packet after ~2.9 s, far beyond the
            // 200 ms publication age. A fresh independent key frame arrives first.
            QThread::msleep(250);
            const qint64 enqueuedFresh = f.host.elapsedUs();
            QVERIFY(f.client.enqueueVideo(packet(2, codec.second, true)));
            QTRY_VERIFY_WITH_TIMEOUT(f.host.count.value(2) > 0, 2000);
            const qint64 freshFirst = f.host.first.value(2);
            QTRY_VERIFY_WITH_TIMEOUT(f.host.completed.contains(2), 3000);
            QTest::qWait(100);

            const bool oldSent = f.host.count.contains(1);
            const qint64 oldAge = oldSent ? f.host.first.value(1) - enqueuedOld : -1;
            qInfo() << "R1-residual codec" << codec.first << "oldSent" << oldSent << "oldAgeUs"
                    << oldAge << "oldFragments" << f.host.count.value(1) << "freshWaitUs"
                    << freshFirst - enqueuedFresh << "freshFragments" << f.host.count.value(2)
                    << "ageDrops" << f.stats().uploadPacketsDroppedAge;
            QVERIFY2(!oldSent || oldAge <= MaxAgeUs,
                     "an unsent deferred packet bypassed the 200 ms publication age");
            QVERIFY2(f.host.completed.contains(2),
                     "the fresh key frame expired behind an old pending admission");

            // Normal picture resumes behind the recovered anchor.
            QVERIFY(f.client.enqueueVideo(packet(3, codec.second, false)));
            QTRY_VERIFY_WITH_TIMEOUT(f.host.count.value(3) > 0, 2000);
            QVERIFY(f.client.shutdown());
        }
    }

    // R4: the bounded admission wait ends in a timeout without refunding a
    // reservation it never made; the next legal large packet still has to wait.
    void stalledAdmissionTimeoutDoesNotMintCredit()
    {
        Fixture f;
        QVERIFY(f.open());
        const int large = 900000;
        const int fragments = fragmentsFor(large);
        const qint64 start = f.host.elapsedUs();
        QVERIFY(f.client.enqueueVideo(packet(0, large, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(0), fragments, 4000);

        QVERIFY(f.client.enqueueVideo(packet(1, large, true)));
        QTRY_VERIFY_WITH_TIMEOUT(f.stats().uploadPacketsDeferred >= 1, 1000);
        // The bound is bucket-refill time (~3.36 s) plus 250 ms slack; stalling
        // past it makes the admission deadline expire on resume.
        f.stallNetworkThread(3900);
        // stallNetworkThread() returns when the block starts, so the deadline is
        // observed when the blocked event loop resumes ~3.9 s later.
        QTRY_VERIFY_WITH_TIMEOUT(f.stats().uploadPacketsDroppedPacing >= 1, 7000);
        QCOMPARE(f.stats().uploadPacketsDroppedAge, quint64(0));
        QVERIFY2(!f.host.count.contains(1), "the timed-out packet wrote fragments");
        const qint64 droppedAt = f.host.elapsedUs();

        // The admission bound can only expire after the bucket has refilled to the
        // burst, so the observable invariants are: the whole packet was dropped,
        // the pipeline still transmits a complete legal packet afterwards, the
        // pacer never exceeds its burst, and total wire bytes stay inside
        // burst + rate * elapsed. The reserved/refunded accounting itself is
        // pinned by Stage5RemediationTests (SendReservation).
        const qint64 enqueued2 = f.host.elapsedUs();
        QVERIFY(f.client.enqueueVideo(packet(2, large, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(2), fragments, 8000);
        const qint64 waitedUs = f.host.first.value(2) - enqueued2;
        const qint64 end = f.host.last.value(2);
        const auto stats = f.stats();
        qInfo() << "R4-timeout droppedAtUs" << droppedAt - start << "followUpWaitedUs" << waitedUs
                << "wireBytes" << f.host.wireBytes << "bound" << budgetBound(start, end, 2000000)
                << "pacingDrops" << stats.uploadPacketsDroppedPacing << "budgetBytes"
                << stats.publishBudgetBytes;
        QVERIFY(f.host.completed.contains(0));
        QVERIFY(f.host.completed.contains(2));
        QVERIFY2(stats.publishBudgetBytes <= 1024 * 1024,
                 "the pacer held more than its burst after a timed-out admission");
        QVERIFY2(f.host.wireBytes <= budgetBound(start, end, 2000000),
                 "transmitted bytes exceed burst + profile rate x1.25");
        QVERIFY(f.client.shutdown());
    }

    // R4: clearing the queue while a packet waits for its budget has nothing to
    // refund; the next legal large packet must still wait for the bucket. A clear
    // that refunded the deferred packet's whole cost would instantly refill the
    // bucket and let it start immediately (the pre-fix behaviour).
    void clearWhileDeferredDoesNotMintCredit()
    {
        Fixture f;
        QVERIFY(f.open());
        const int large = 900000;
        const int fragments = fragmentsFor(large);
        const qint64 start = f.host.elapsedUs();
        QVERIFY(f.client.enqueueVideo(packet(0, large, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(0), fragments, 4000);

        QVERIFY(f.client.enqueueVideo(packet(1, large, true)));
        QTRY_VERIFY_WITH_TIMEOUT(f.stats().uploadPacketsDeferred >= 1, 1000);
        f.client.clearUpload();
        QTest::qWait(100);
        QVERIFY2(!f.host.count.contains(1), "the cleared deferred packet wrote fragments");
        const auto cleared = f.stats();
        QCOMPARE(cleared.outboundBytes, qint64(0));
        QCOMPARE(cleared.outboundQueued, 0);

        const qint64 enqueued2 = f.host.elapsedUs();
        QVERIFY(f.client.enqueueVideo(packet(2, large, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(2), fragments, 8000);
        const qint64 waitedUs = f.host.first.value(2) - enqueued2;
        const qint64 end = f.host.last.value(2);
        qInfo() << "R4-clear nextWaitedUs" << waitedUs << "wireBytes" << f.host.wireBytes
                << "bound" << budgetBound(start, end, 2000000);
        QVERIFY2(waitedUs >= 2400000,
                 "clearing an unadmitted packet refunded bytes it never reserved");
        QVERIFY2(f.host.wireBytes <= budgetBound(start, end, 2000000),
                 "clearing the queue created pacing credit beyond what reached the wire");
        QVERIFY(f.client.shutdown());
    }

    // R4: shutdown drops the queue and the reservation; no uploads are accepted
    // afterwards and the reported backlog is empty.
    void stopReleasesTheQueueAndReservation()
    {
        Fixture f;
        QVERIFY(f.open());
        const int large = 900000;
        QVERIFY(f.client.enqueueVideo(packet(0, large, true)));
        QTRY_VERIFY_WITH_TIMEOUT(f.host.count.value(0) >= 100, 4000);
        QVERIFY(f.client.shutdown());
        const auto stopped = f.stats();
        qInfo() << "R4-stop wireBytes" << f.host.wireBytes << "outboundBytes"
                << stopped.outboundBytes << "outboundQueued" << stopped.outboundQueued;
        QCOMPARE(stopped.outboundBytes, qint64(0));
        QCOMPARE(stopped.outboundQueued, 0);
        QVERIFY(!f.client.enqueueVideo(packet(1, 1000, true)));
    }

    // R6: while the GUI thread is blocked the notification board merges every
    // dropped packet into one pending task and one signal with the cumulative
    // count and the necessary reasons; a state notification posted in the same
    // window is not lost.
    void guiDropNoticesCoalesceWithCountsAndReasons()
    {
        Fixture f;
        QVERIFY(f.open());
        int accepted = 0;
        // No event-loop turn happens in this loop: the GUI stays blocked while the
        // network thread keeps rejecting the over-burst packets.
        for (int index = 0; index < 30; ++index) {
            if (f.client.enqueueVideo(packet(100 + quint64(index), 1040000, true))) ++accepted;
            QThread::msleep(10);
        }
        // A state notification is posted into the same blocked window. No
        // event-loop turn is allowed here, otherwise the board would drain.
        StreamFormat format;
        format.width = 1280;
        format.height = 720;
        format.fps = 30;
        format.bitrate = 2000000;
        f.client.configureStream(format);
        QThread::msleep(300);
        QCOMPARE(f.dropped.size(), 0);
        const int configuredBefore = f.configured.size();
        const auto pending = f.stats();
        qInfo() << "R6 accepted" << accepted << "pendingNotices" << pending.guiPendingNotices
                << "peakNotices" << pending.guiPeakNotices << "coalesced"
                << pending.guiNoticesCoalesced;
        QVERIFY2(pending.guiPeakNotices <= 4,
                 "the pending GUI task count grew with the number of dropped packets");

        QCoreApplication::sendPostedEvents(&f.client, QEvent::MetaCall);
        QTRY_VERIFY_WITH_TIMEOUT(f.dropped.size() >= 1, 1000);
        const auto delivered = f.stats();
        qInfo() << "R6 deliveredSignals" << f.dropped.size() << "configuredBefore"
                << configuredBefore << "configuredAfter" << f.configured.size() << "delivered"
                << delivered.guiNoticesDelivered << "coalesced" << delivered.guiNoticesCoalesced;
        QCOMPARE(f.dropped.size(), 1);
        QCOMPARE(f.dropped.last().at(0).toInt(), accepted);
        QVERIFY(f.dropped.last().at(1).toString().contains(QStringLiteral("超出发送突发上限")));
        QVERIFY2(delivered.guiNoticesCoalesced >= quint64(accepted - 1),
                 "drop notifications were not merged");
        QCOMPARE(delivered.guiPendingNotices, 0);
        // The stream reconfiguration that happened inside the blocked window is
        // still delivered, so coalescing never discards important state.
        QTRY_VERIFY_WITH_TIMEOUT(f.configured.size() >= configuredBefore + 1, 2000);

        // Recovery: after the drain the pipeline still transmits normally.
        f.client.configureStream(format);
        QTRY_VERIFY_WITH_TIMEOUT(f.configured.size() >= configuredBefore + 2, 3000);
        QVERIFY(f.client.enqueueVideo(packet(200, 5000, true)));
        QTRY_VERIFY_WITH_TIMEOUT(f.host.count.value(200) > 0, 3000);
        QVERIFY(f.client.shutdown());
    }

    // R6: distinct control failures are kept per operation inside the one pending
    // GUI task, so a later failure cannot hide an earlier one and the task count
    // still stays at one.
    void guiControlFailuresStayPerOperationInOnePendingTask()
    {
        NetworkClient client;
        QSignalSpy failed(&client, &NetworkClient::controlFailed);
        client.start();
        // Not connected: both commands fail locally on the network thread.
        client.createRoom(QStringLiteral("720p30"));
        client.joinRoom(QStringLiteral("12345678"));
        QThread::msleep(250);
        QCOMPARE(failed.size(), 0);
        const auto pending = client.stats();
        QCOMPARE(pending.guiPendingNotices, 1);
        QCOMPARE(pending.guiPeakNotices, 1);

        QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 2, 1000);
        QStringList operations;
        for (const auto &arguments : failed) {
            operations.append(arguments.at(0).toString());
            QCOMPARE(arguments.at(1).toInt(), int(ErrorCode::NotReady));
        }
        operations.sort();
        qInfo() << "R6-control operations" << operations << "pending"
                << client.stats().guiPendingNotices << "delivered"
                << client.stats().guiNoticesDelivered;
        QCOMPARE(operations, QStringList({QStringLiteral("create"), QStringLiteral("join")}));
        QCOMPARE(client.stats().guiPendingNotices, 0);
        QVERIFY(client.shutdown());
    }

    // P3: a consumed batch must not be history for the next drain. Also call
    // stats() from the signal callback: delivery must never hold the board lock.
    void deliveredControlFailureIsNotReplayedLater()
    {
        NetworkClient client;
        QSignalSpy failed(&client, &NetworkClient::controlFailed);
        QList<int> pendingDuringCallback;
        connect(&client, &NetworkClient::controlFailed, &client,
                [&](const QString &, int, const QString &) {
                    pendingDuringCallback.append(client.stats().guiPendingNotices);
                });
        client.start();
        client.createRoom(QStringLiteral("720p30"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 2000);
        QCOMPARE(failed.takeFirst().at(0).toString(), QStringLiteral("create"));
        client.joinRoom(QStringLiteral("12345678"));
        QThread::msleep(250);
        QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
        QStringList second;
        for (const auto &args : failed) second.append(args.at(0).toString());
        qInfo() << "P3 first create second" << second << "callbackPending" << pendingDuringCallback;
        QCOMPARE(second, QStringList({QStringLiteral("join")}));
        failed.clear();
        // The same operation failing again is a new failure, not deduplicated
        // against the already consumed create failure.
        client.createRoom(QStringLiteral("720p30"));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 2000);
        QCOMPARE(failed.first().at(0).toString(), QStringLiteral("create"));
        QCOMPARE(pendingDuringCallback, QList<int>({0, 0, 0}));
        QVERIFY(client.shutdown());
    }

    void blockedControlFailuresKeepOnlyLatestErrorPerOperation()
    {
        ThreadedRoom room(true);
        QSignalSpy failed(&room.client, &NetworkClient::controlFailed);
        room.client.start();
        room.client.connectToServer(QStringLiteral("127.0.0.1"), room.host->m_tcp.serverPort());
        QTRY_COMPARE_WITH_TIMEOUT(room.connected.size(), 1, 3000);
        const auto before = room.stats();
        room.client.createRoom(QStringLiteral("720p30"));
        room.client.joinRoom(QStringLiteral("12345678"));
        room.client.createRoom(QStringLiteral("720p30"));
        room.client.joinRoom(QStringLiteral("12345678"));
        // Both the TCP host and NetworkClient have their own event loops. Do
        // not pump GUI events until all four distinct wire responses arrived.
        QElapsedTimer wait;
        wait.start();
        while (room.stats().tcpResponsesReceived < before.tcpResponsesReceived + 4
               && wait.elapsed() < 3000) QThread::msleep(10);
        QCOMPARE(room.stats().tcpResponsesReceived, before.tcpResponsesReceived + 4);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(room.stats().guiPendingNotices, 1);
        QCOMPARE(room.stats().guiNoticesCoalesced - before.guiNoticesCoalesced, quint64(3));
        QCoreApplication::sendPostedEvents(&room.client, QEvent::MetaCall);
        QCOMPARE(failed.size(), 2);
        QStringList operations;
        for (const auto &args : failed) {
            const auto op = args.at(0).toString();
            operations.append(op);
            QCOMPARE(args.at(1).toInt(), int(ErrorCode::RoomFull));
            QCOMPARE(args.at(2).toString(), op + QStringLiteral("-2"));
        }
        operations.sort();
        qInfo() << "P3 latest-per-operation" << operations << "coalesced" << 3;
        QCOMPARE(operations, QStringList({QStringLiteral("create"), QStringLiteral("join")}));
        QCOMPARE(room.stats().guiPendingNotices, 0);
        QVERIFY(room.client.shutdown());
    }

    void blockedControlFailurePlateauDoesNotGrowWithAttempts()
    {
        NetworkClient client;
        QSignalSpy failed(&client, &NetworkClient::controlFailed);
        client.start();
        for (int attempts : {40, 400}) {
            const auto before = client.stats();
            for (int i = 0; i < attempts; ++i) {
                client.createRoom(QStringLiteral("720p30"));
                client.joinRoom(QStringLiteral("12345678"));
            }
            QElapsedTimer wait;
            wait.start();
            while (client.stats().guiNoticesCoalesced < before.guiNoticesCoalesced + 2 * attempts - 1
                   && wait.elapsed() < 3000) QThread::msleep(10);
            const auto pending = client.stats();
            QCOMPARE(pending.guiNoticesCoalesced - before.guiNoticesCoalesced, quint64(2 * attempts - 1));
            QCOMPARE(pending.guiPendingNotices, 1);
            QCOMPARE(pending.guiPeakNotices, 1);
            QCOMPARE(failed.size(), 0);
            QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
            QCOMPARE(failed.size(), 2);
            qInfo() << "P3 plateau attempts" << 2 * attempts << "peak" << pending.guiPeakNotices
                    << "coalesced" << pending.guiNoticesCoalesced << "signals" << failed.size();
            failed.clear();
            QCOMPARE(client.stats().guiPendingNotices, 0);
        }
        QVERIFY(client.shutdown());
    }

    // R6: repeated long blocked windows keep the same plateau; the pending task
    // count does not grow with the number of dropped packets.
    void guiNoticeBoardPlateauDoesNotGrowAcrossBlockedWindows()
    {
        Fixture f;
        QVERIFY(f.open());
        QList<int> delivered;
        QList<int> peaks;
        for (int attempts : {40, 80}) {
            QCoreApplication::sendPostedEvents(&f.client, QEvent::MetaCall);
            f.dropped.clear();
            for (int index = 0; index < attempts; ++index) {
                QVERIFY(f.client.enqueueVideo(packet(300 + quint64(index), 1040000, true)));
                QThread::msleep(10);
            }
            QThread::msleep(150);
            const auto pending = f.stats();
            peaks.append(pending.guiPeakNotices);
            QCoreApplication::sendPostedEvents(&f.client, QEvent::MetaCall);
            QTRY_VERIFY_WITH_TIMEOUT(f.dropped.size() >= 1, 1000);
            delivered.append(f.dropped.size());
            qInfo() << "R6-plateau attempts" << attempts << "peak" << pending.guiPeakNotices
                    << "delivered" << delivered.last() << "coalesced"
                    << f.stats().guiNoticesCoalesced;
        }
        QVERIFY2(delivered[1] <= delivered[0] + 4,
                 "pending GUI drop callbacks grew with the blocked time");
        QVERIFY2(peaks[1] <= 4, "the pending notification plateau grew with the input count");
        QVERIFY(f.client.shutdown());
    }

    // R7 round-4 residual: an earlier notice of the same GUI drain changes the
    // room lifetime. The UDP failure that the drain copied before that callback
    // must not be delivered as the state of the session that exists afterwards.
    void roomChangeDuringGuiDrainMustInvalidateCopiedOldUdpNotice()
    {
        ThreadedRoom room;
        QVERIFY(room.open());
        room.registered.clear();
        bool armed = true;
        bool transitioned = false;
        int failuresAfterTransition = 0;
        quint64 acksAfterTransition = 0;
        QObject::connect(&room.client, &NetworkClient::udpReady, &room.client,
                         [&](bool ok, const QString &) {
                             if (!ok && transitioned) ++failuresAfterTransition;
                         });
        QObject::connect(&room.client, &NetworkClient::streamConfigured, &room.client,
                         [&](quint64) {
                             if (!armed) return;
                             armed = false;
                             // This earlier notice is in the same drain snapshot as
                             // the old UDP failure. Let the real network thread leave
                             // and enter a new room before the drain reaches
                             // udpReady; no nested event pumping or private method.
                             room.client.stopRoom(QStringLiteral("remediation-reentrant-switch"));
                             QThread::msleep(400);
                             room.host->allowAck = true;
                             room.client.createRoom(QStringLiteral("720p30"));
                             QThread::msleep(1200);
                             acksAfterTransition = room.client.stats().udpAckReceived;
                             transitioned = true;
                         });
        room.host->allowAck = false;
        StreamFormat format;
        format.width = 1280;
        format.height = 720;
        format.fps = 30;
        format.bitrate = 2000000;
        room.client.configureStream(format);
        QThread::msleep(16000); // real 15 s UDP idle deadline while the GUI is blocked
        const auto before = room.client.stats();
        QCoreApplication::sendPostedEvents(&room.client, QEvent::MetaCall);
        QList<bool> order;
        for (const auto &row : room.registered) order.append(row.at(0).toBool());
        const auto after = room.client.stats();
        qInfo() << "R7-drain pendingBefore" << before.guiPendingNotices << "transitioned"
                << transitioned << "newRoomAcks" << acksAfterTransition
                << "oldFailuresAfterTransition" << failuresAfterTransition << "order" << order
                << "stale" << after.guiNoticesStale << "pendingAfterFirstDrain"
                << after.guiPendingNotices;
        // The new room's own notices are pushed while this drain is running; the
        // next drain must still deliver them and end with an empty board.
        QTRY_VERIFY_WITH_TIMEOUT(room.created.size() >= 2, 3000);
        QCoreApplication::sendPostedEvents(&room.client, QEvent::MetaCall);
        const auto settled = room.client.stats();
        qInfo() << "R7-drain-settled newRoomCreated" << room.created.size() << "stale"
                << settled.guiNoticesStale << "pendingAfter" << settled.guiPendingNotices;
        QVERIFY(room.client.shutdown());
        QVERIFY(before.guiPendingNotices >= 2);
        QVERIFY(transitioned);
        QVERIFY(acksAfterTransition >= 1);
        QVERIFY2(failuresAfterTransition == 0,
                 "the drain compared a copied UDP generation with its copied snapshot and emitted "
                 "an old-room failure after a new room registered");
        QVERIFY(settled.guiNoticesStale >= 1);
        QCOMPARE(settled.guiPendingNotices, 0);
    }

    // R7 round-4 residual: the earlier callback stops the client's room. The
    // remaining copied notices of the old lifetime may not be delivered, and the
    // invalidation may not depend on the network thread having processed the stop
    // (the control command is asynchronous and this callback does not wait).
    void stopInsideEarlierDrainCallbackMustDiscardRemainingOldNotices()
    {
        ThreadedRoom room;
        QVERIFY(room.open());
        room.registered.clear();
        bool stopped = false;
        int failuresAfterStop = 0;
        QObject::connect(&room.client, &NetworkClient::udpReady, &room.client,
                         [&](bool ok, const QString &) {
                             if (!ok && stopped) ++failuresAfterStop;
                         });
        QObject::connect(&room.client, &NetworkClient::streamConfigured, &room.client,
                         [&](quint64) {
                             if (stopped) return;
                             stopped = true;
                             room.client.stopRoom(QStringLiteral("remediation-stop-in-drain"));
                         });
        room.host->allowAck = false;
        StreamFormat format;
        format.width = 1280;
        format.height = 720;
        format.fps = 30;
        format.bitrate = 2000000;
        room.client.configureStream(format);
        QThread::msleep(16000);
        const auto before = room.client.stats();
        QCoreApplication::sendPostedEvents(&room.client, QEvent::MetaCall);
        QThread::msleep(800);
        QCoreApplication::sendPostedEvents(&room.client, QEvent::MetaCall);
        const auto after = room.client.stats();
        qInfo() << "R7-drain-stop pendingBefore" << before.guiPendingNotices << "peak"
                << before.guiPeakNotices << "stopped" << stopped << "failuresAfterStop"
                << failuresAfterStop << "stale" << after.guiNoticesStale << "pendingAfter"
                << after.guiPendingNotices;
        QVERIFY(room.client.shutdown());
        QVERIFY(before.guiPendingNotices >= 2);
        QVERIFY(before.guiPeakNotices <= 2);
        QVERIFY(stopped);
        QVERIFY2(failuresAfterStop == 0,
                 "a UDP failure copied before the callback-internal stop was delivered after it");
        QVERIFY(after.guiNoticesStale >= 1);
        QCOMPARE(after.guiPendingNotices, 0);
    }

    // R7 round-4 residual: the UDP failure handler itself closes the room. The
    // recovery that was merged into the same fixed slot before the drain belongs
    // to the closed room and must not be delivered after the failure.
    void udpFailureCallbackClosingRoomMustSuppressCopiedRecovery()
    {
        ThreadedRoom room;
        QVERIFY(room.open());
        const int deliveredBefore = room.registered.size();
        QObject::connect(&room.client, &NetworkClient::udpReady, &room.client,
                         [&](bool ok, const QString &) {
                             if (ok) return;
                             room.client.stopRoom(QStringLiteral("remediation-close-in-failure"));
                         });
        room.host->allowAck = false;
        QThread::msleep(16000);
        room.host->allowAck = true;
        QThread::msleep(1500); // recovery merged into the slot before the drain
        const auto before = room.client.stats();
        QCoreApplication::sendPostedEvents(&room.client, QEvent::MetaCall);
        QList<bool> order;
        for (int index = deliveredBefore; index < room.registered.size(); ++index)
            order.append(room.registered.at(index).at(0).toBool());
        const auto after = room.client.stats();
        qInfo() << "R7-drain-close pendingBefore" << before.guiPendingNotices << "order" << order
                << "stale" << after.guiNoticesStale << "pendingAfter" << after.guiPendingNotices;
        QVERIFY(room.client.shutdown());
        QVERIFY(before.guiPendingNotices >= 1);
        QVERIFY2(order == QList<bool>{false},
                 "the copied recovery of a room closed inside the failure handler was delivered");
        QVERIFY(after.guiNoticesStale >= 1);
        QCOMPARE(after.guiPendingNotices, 0);
    }
};

QTEST_GUILESS_MAIN(Stage5RemediationNetworkTests)
#include "Stage5RemediationNetworkTests.moc"
