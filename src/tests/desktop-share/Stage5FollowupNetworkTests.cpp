#include "NetworkClient.h"

#include <desktop-share/PublishQueue.h>
#include <ztprotocol/WireProtocol.h>

#include <QAbstractEventDispatcher>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

VideoPacket packet(quint64 sequence, int bytes, bool keyFrame, quint64 epoch = 3)
{
    VideoPacket value;
    value.token.fill(0x41);
    value.sessionId = 7;
    value.roomId = 11;
    value.sequence = sequence;
    value.streamEpoch = epoch;
    value.pts = qint64(sequence) * 33333;
    value.duration = 33333;
    if (keyFrame) value.flags |= quint16(MediaFlag::KeyFrame);
    value.data = QByteArray(bytes, char('a' + int(sequence % 26)));
    return value;
}

// Minimal host: admits the control handshake, answers UDP registration and records
// every video datagram without dropping any. It owns a real reassembler so a test
// can assert that a packet was actually usable by the receiver, not merely that
// datagrams were written.
class RecordingHost : public QObject {
public:
    explicit RecordingHost(const MediaHeader &id) : m_identity(id), m_assembler(id)
    {
        m_timer.start();
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
                    m_udp.writeDatagram(
                        wrapZts1(encodeRegisterAck(m_identity.token, m_identity.sessionId,
                                                   m_identity.roomId)),
                        datagram.senderAddress(), datagram.senderPort());
                    continue;
                }
                if (decoded.header.type != MediaType::Video) continue;
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
    quint64 expiredAssemblies() const { return m_assembler.expiredPackets(); }
    int fragmentsFor(quint64 sequence, int bytes) const
    {
        return fragmentVideo(packet(sequence, bytes, true)).size();
    }

    QTcpServer m_tcp;
    QUdpSocket m_udp;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QMap<quint64, int> count;
    QMap<quint64, qint64> first;
    QMap<quint64, qint64> last;
    QList<quint64> completed;

private:
    MediaHeader m_identity;
    QElapsedTimer m_timer;
    PacketReassembler m_assembler;
};

// Brings one real NetworkClient into a publishing room against the recording host.
struct Fixture {
    RecordingHost host{identity()};
    NetworkClient client;
    QSignalSpy connected{&client, &NetworkClient::connected};
    QSignalSpy created{&client, &NetworkClient::roomCreated};
    QSignalSpy registered{&client, &NetworkClient::udpReady};
    QSignalSpy configured{&client, &NetworkClient::streamConfigured};
    QSignalSpy dropped{&client, &NetworkClient::uploadDropped};

    bool open(int bitrate = 2000000)
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
        format.width = 1280;
        format.height = 720;
        format.fps = 30;
        format.bitrate = bitrate;
        client.configureStream(format);
        return QTest::qWaitFor([&] { return configured.size() == 1; }, 3000);
    }

    // Runs a blocking call on the network thread so its event loop stalls while a
    // packet is in flight, exactly like a long event-loop turn in production.
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

} // namespace

namespace {

// Records one measured timeline when the evidence directory is set. The tests stay
// silent and file-free in a normal run; the R1-R3 follow-up evidence run sets
// ZDS_FOLLOWUP_EVIDENCE_DIR so the same assertions also emit raw JSON.
void writeEvidence(const QString &name, const QJsonObject &value)
{
    const auto directory = qEnvironmentVariable("ZDS_FOLLOWUP_EVIDENCE_DIR");
    if (directory.isEmpty()) return;
    QDir().mkpath(directory);
    QFile file(QDir(directory).filePath(name));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(value).toJson(QJsonDocument::Indented));
    }
}

QJsonObject packetTimeline(const RecordingHost &host, quint64 sequence, qint64 enqueuedUs)
{
    QJsonObject value;
    value.insert(QStringLiteral("sequence"), QString::number(sequence));
    value.insert(QStringLiteral("enqueuedUs"), double(enqueuedUs));
    value.insert(QStringLiteral("fragmentsObserved"), host.count.value(sequence));
    value.insert(QStringLiteral("firstFragmentUs"), double(host.first.value(sequence)));
    value.insert(QStringLiteral("lastFragmentUs"), double(host.last.value(sequence)));
    value.insert(QStringLiteral("spanUs"),
                 double(host.last.value(sequence) - host.first.value(sequence)));
    value.insert(QStringLiteral("admissionWaitUs"),
                 double(host.first.value(sequence) - enqueuedUs));
    value.insert(QStringLiteral("reassembled"), host.completed.contains(sequence));
    return value;
}

} // namespace

// R1: a large-but-legal packet may only start when its complete wire cost fits the
// pacing burst, so every fragment still arrives inside the receiver's 200 ms
// reassembly window instead of dripping over seconds after the packet expired.
class Stage5FollowupNetworkTests : public QObject {
    Q_OBJECT

private slots:
    void largePacketWaitsForItsWholeBudgetAndStaysReassemblable()
    {
        Fixture f;
        QVERIFY(f.open());
        const int bytes = 900000;
        const int fragments = f.host.fragmentsFor(1, bytes);
        QVERIFY(fragments > 0);

        const qint64 enqueued0 = f.host.elapsedUs();
        // The first packet drains the 1 MiB burst in one turn-sized sweep.
        QVERIFY(f.client.enqueueVideo(packet(0, bytes, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(0), fragments, 4000);
        // The second legal key frame can only be sent after the bucket refills; the
        // sender must wait for the whole cost instead of dripping fragments.
        const qint64 enqueued1 = f.host.elapsedUs();
        QVERIFY(f.client.enqueueVideo(packet(1, bytes, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(1), fragments, 8000);
        const qint64 spanUs = f.host.last.value(1) - f.host.first.value(1);
        const auto stats = f.stats();
        {
            QJsonObject evidence;
            evidence.insert(QStringLiteral("payloadBytes"), bytes);
            evidence.insert(QStringLiteral("fragmentsPerPacket"), fragments);
            evidence.insert(QStringLiteral("reassemblyDeadlineUs"), 200000);
            evidence.insert(QStringLiteral("activeSendDeadlineUs"), 150000);
            evidence.insert(QStringLiteral("firstPacket"),
                            packetTimeline(f.host, 0, enqueued0));
            evidence.insert(QStringLiteral("secondPacket"),
                            packetTimeline(f.host, 1, enqueued1));
            evidence.insert(QStringLiteral("pacingWaits"), double(stats.pacingWaits));
            evidence.insert(QStringLiteral("packetsDeferred"),
                            double(stats.uploadPacketsDeferred));
            evidence.insert(QStringLiteral("pacingDrops"),
                            double(stats.uploadPacketsDroppedPacing));
            evidence.insert(QStringLiteral("expiredAssemblies"),
                            double(f.host.expiredAssemblies()));
            writeEvidence(QStringLiteral("r1-large-packet-timeline.json"), evidence);
        }
        qInfo() << "firstSpanUs" << f.host.last.value(0) - f.host.first.value(0)
                << "secondSpanUs" << spanUs << "fragments" << fragments
                << "waits" << stats.pacingWaits << "deferred" << stats.uploadPacketsDeferred
                << "pacingDrops" << stats.uploadPacketsDroppedPacing;
        QVERIFY(f.host.completed.contains(1));
        QVERIFY2(spanUs <= 200000,
                 qPrintable(QStringLiteral("last fragment arrived %1 us after the first")
                                .arg(spanUs)));
        QCOMPARE(f.host.expiredAssemblies(), quint64(0));
        QVERIFY(stats.pacingWaits >= 1);
        QVERIFY(stats.uploadPacketsDeferred >= 1);
        QCOMPARE(stats.uploadPacketsDroppedPacing, quint64(0));
        QVERIFY(f.client.shutdown());
    }

    void packetLargerThanTheBurstIsDroppedWholeAndAKeyFrameRecovers()
    {
        Fixture f;
        QVERIFY(f.open());
        // 1,040,000 payload bytes are legal for the protocol (<= 1 MiB) but their
        // complete wire cost exceeds the 1 MiB pacing burst, so this packet can
        // never finish inside the 200 ms reassembly window at this pace.
        const qint64 oversizedEnqueuedUs = f.host.elapsedUs();
        QVERIFY(f.client.enqueueVideo(packet(0, 1040000, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.stats().uploadPacketsDroppedPacing, quint64(1), 3000);
        const qint64 oversizedDroppedUs = f.host.elapsedUs();
        QVERIFY2(f.host.count.isEmpty(), "no fragment of an impossible packet may be sent");
        QVERIFY(!f.host.completed.contains(0));
        QVERIFY(f.dropped.size() >= 1);

        // Its dependent delta is refused by the bounded queue and never reaches the wire.
        QVERIFY2(!f.client.enqueueVideo(packet(1, 20000, false)),
                 "a delta that depends on a dropped key frame must be refused");
        QTest::qWait(80);
        QVERIFY2(!f.host.count.contains(1),
                 "the delta that referenced the dropped key frame reached the wire");

        // A fresh, small key frame reopens the stream and its deltas follow.
        const int keyBytes = 50000;
        const int keyFragments = f.host.fragmentsFor(2, keyBytes);
        const qint64 recoveryEnqueuedUs = f.host.elapsedUs();
        QVERIFY(f.client.enqueueVideo(packet(2, keyBytes, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(2), keyFragments, 4000);
        QVERIFY(f.client.enqueueVideo(packet(3, 20000, false)));
        QTRY_VERIFY_WITH_TIMEOUT(f.host.count.value(3) > 0, 3000);
        {
            QJsonObject evidence;
            evidence.insert(QStringLiteral("oversizedEnqueuedUs"),
                            double(oversizedEnqueuedUs));
            evidence.insert(QStringLiteral("oversizedDroppedUs"), double(oversizedDroppedUs));
            evidence.insert(QStringLiteral("oversizedFragmentsObserved"),
                            f.host.count.value(0));
            evidence.insert(QStringLiteral("dependentFragmentsObserved"),
                            f.host.count.value(1));
            evidence.insert(QStringLiteral("pacingDrops"),
                            double(f.stats().uploadPacketsDroppedPacing));
            evidence.insert(QStringLiteral("recoveryEnqueuedUs"), double(recoveryEnqueuedUs));
            evidence.insert(QStringLiteral("recoveryKeyFirstFragmentUs"),
                            double(f.host.first.value(2)));
            evidence.insert(QStringLiteral("recoveryDeltaFirstFragmentUs"),
                            double(f.host.first.value(3)));
            writeEvidence(QStringLiteral("r1-drop-recovery-timeline.json"), evidence);
        }
        QVERIFY(f.client.shutdown());
    }

    void expiredKeyFrameKeepsDependentsOffTheWireUntilANewKeyFrame()
    {
        Fixture f;
        QVERIFY(f.open());
        // Stall the network thread so the queued key frame ages out before the
        // delta behind it is acquired.
        f.stallNetworkThread(280);
        const qint64 keyEnqueuedUs = f.host.elapsedUs();
        QVERIFY(f.client.enqueueVideo(packet(10, 1000, true)));
        QTest::qWait(120);
        const qint64 deltaEnqueuedUs = f.host.elapsedUs();
        QVERIFY(f.client.enqueueVideo(packet(11, 1000, false)));
        QTest::qWait(400);
        const auto stats = f.stats();
        qInfo() << "sequences" << f.host.count.keys() << "ageDrops"
                << stats.uploadPacketsDroppedAge << "dependencyDrops"
                << stats.uploadPacketsDroppedDependency << "queueRejected"
                << stats.uploadPacketsDroppedQueue;
        QVERIFY2(!f.host.count.contains(11),
                 "the delta was transmitted after its key frame had expired");
        QVERIFY(stats.uploadPacketsDroppedAge >= 1);
        QVERIFY(stats.uploadPacketsDroppedDependency >= 1);

        // A dependent frame offered after the break is refused before it is queued.
        QVERIFY(!f.client.enqueueVideo(packet(12, 1000, false)));
        QTest::qWait(50);
        QVERIFY(!f.host.count.contains(12));

        // The next independent key frame restores normal sending.
        const int keyFragments = f.host.fragmentsFor(13, 4000);
        QVERIFY(f.client.enqueueVideo(packet(13, 4000, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(13), keyFragments, 3000);
        QVERIFY(f.client.enqueueVideo(packet(14, 2000, false)));
        QTRY_VERIFY_WITH_TIMEOUT(f.host.count.value(14) > 0, 3000);
        {
            QJsonObject evidence;
            evidence.insert(QStringLiteral("keyEnqueuedUs"), double(keyEnqueuedUs));
            evidence.insert(QStringLiteral("deltaEnqueuedUs"), double(deltaEnqueuedUs));
            evidence.insert(QStringLiteral("ageDropLimitUs"), 200000);
            evidence.insert(QStringLiteral("keyFragmentsObserved"), f.host.count.value(10));
            evidence.insert(QStringLiteral("deltaFragmentsObserved"), f.host.count.value(11));
            evidence.insert(QStringLiteral("refusedDependentFragmentsObserved"),
                            f.host.count.value(12));
            evidence.insert(QStringLiteral("ageDrops"),
                            double(stats.uploadPacketsDroppedAge));
            evidence.insert(QStringLiteral("dependencyDrops"),
                            double(stats.uploadPacketsDroppedDependency));
            evidence.insert(QStringLiteral("recoveryKeyFragments"), f.host.count.value(13));
            evidence.insert(QStringLiteral("recoveryDeltaFragments"), f.host.count.value(14));
            writeEvidence(QStringLiteral("r2-dependency-timeline.json"), evidence);
        }
        QVERIFY(f.client.shutdown());
    }

    void stalledActivePacketAbortsTheRestAndInvalidatesItsDependents()
    {
        Fixture f;
        QVERIFY(f.open());
        const int bytes = 900000;
        const int fragments = f.host.fragmentsFor(20, bytes);
        QVERIFY(f.client.enqueueVideo(packet(20, bytes, true)));
        // Wait until the burst is in flight, then stall the network thread beyond
        // the reassembly window: continuing would only produce expired fragments.
        QTRY_VERIFY_WITH_TIMEOUT(f.host.count.value(20) > 0, 3000);
        f.stallNetworkThread(350);
        QTRY_COMPARE_WITH_TIMEOUT(f.stats().uploadPacketsDroppedDeadline, quint64(1), 4000);
        const int sentFragments = f.host.count.value(20);
        const qint64 abortedUs = f.host.elapsedUs();
        qInfo() << "sentFragments" << sentFragments << "of" << fragments
                << "droppedDeadline" << f.stats().uploadPacketsDroppedDeadline;
        QVERIFY2(sentFragments < fragments, "the aborted packet must not be completed");
        QVERIFY(!f.host.completed.contains(20));
        QTest::qWait(80);
        // Dependents queued by the encoder are retracted, not sent late.
        QVERIFY2(!f.client.enqueueVideo(packet(21, 2000, false)),
                 "a delta was accepted after the active key frame was aborted");
        QTest::qWait(80);
        QVERIFY2(!f.host.count.contains(21),
                 "a delta was sent after the active key frame was aborted");

        // Recovery: a new key frame is admitted immediately (the unsent budget was
        // refunded) and its dependents follow.
        const int keyFragments = f.host.fragmentsFor(22, 4000);
        QVERIFY(f.client.enqueueVideo(packet(22, 4000, true)));
        QTRY_COMPARE_WITH_TIMEOUT(f.host.count.value(22), keyFragments, 4000);
        QVERIFY(f.client.enqueueVideo(packet(23, 2000, false)));
        QTRY_VERIFY_WITH_TIMEOUT(f.host.count.value(23) > 0, 3000);
        {
            QJsonObject evidence;
            evidence.insert(QStringLiteral("payloadBytes"), bytes);
            evidence.insert(QStringLiteral("fragmentsPerPacket"), fragments);
            evidence.insert(QStringLiteral("fragmentsBeforeStall"), sentFragments);
            evidence.insert(QStringLiteral("abortedAtUs"), double(abortedUs));
            evidence.insert(QStringLiteral("reassembled"), f.host.completed.contains(20));
            evidence.insert(QStringLiteral("droppedDeadline"),
                            double(f.stats().uploadPacketsDroppedDeadline));
            evidence.insert(QStringLiteral("dependentFragmentsObserved"),
                            f.host.count.value(21));
            evidence.insert(QStringLiteral("recoveryKeyFragments"), f.host.count.value(22));
            evidence.insert(QStringLiteral("recoveryDeltaFragments"), f.host.count.value(23));
            writeEvidence(QStringLiteral("r2-stall-abort-timeline.json"), evidence);
        }
        QVERIFY(f.client.shutdown());
    }

    void h264AndBgraTilesChainsFollowTheSameDependencyRules()
    {
        // The wire layer never inspects codec payloads; both formats express their
        // anchors with the key-frame flag and their dependents without it. This runs
        // the identical chain with H.264-shaped and tile-shaped payloads.
        const QList<QPair<QString, int>> formats{
            {QStringLiteral("h264"), 8000},
            {QStringLiteral("bgra-tiles"), 12000},
        };
        for (const auto &format : formats) {
            PublishQueue queue;
            const int bytes = format.second;
            // Anchor is acquired and completed.
            QVERIFY(queue.push(packet(40, bytes, true), 0));
            VideoPacket active;
            QVERIFY(queue.acquire(1000, active));
            QCOMPARE(active.sequence, quint64(40));
            QVERIFY(active.keyFrame());
            queue.releaseActive();
            // Two dependents are queued; the first is aborted after admission, which
            // must retract the second one as well.
            QVERIFY(queue.push(packet(41, bytes, false), 2000));
            QVERIFY(queue.push(packet(42, bytes, false), 3000));
            QVERIFY(queue.acquire(4000, active));
            QCOMPARE(active.sequence, quint64(41));
            queue.abortActive();
            QCOMPARE(queue.queuedPackets(), 0);
            QVERIFY(queue.waitingKeyFrame());
            QVERIFY(!queue.acquire(5000, active));
            QVERIFY2(!queue.push(packet(43, bytes, false), 6000),
                     qPrintable(format.first + QStringLiteral(" dependent was queued after a break")));
            // A new anchor reopens the chain for both formats.
            QVERIFY(queue.push(packet(44, bytes, true), 7000));
            QVERIFY(queue.acquire(8000, active));
            QCOMPARE(active.sequence, quint64(44));
            queue.releaseActive();
            QVERIFY(queue.push(packet(45, bytes, false), 9000));
            QVERIFY(queue.acquire(10000, active));
            QCOMPARE(active.sequence, quint64(45));
            queue.releaseActive();
        }
    }
};

QTEST_GUILESS_MAIN(Stage5FollowupNetworkTests)
#include "Stage5FollowupNetworkTests.moc"
