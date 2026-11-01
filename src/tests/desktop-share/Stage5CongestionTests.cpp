#include <desktop-share/ByteRateLimiter.h>
#include <desktop-share/PacketReassembler.h>
#include <desktop-share/PublishQueue.h>
#include <desktop-share/StreamReceiver.h>

#include <QtTest>

using namespace vusiif::zto::desktop_share;

namespace {

MediaHeader identity()
{
    MediaHeader header;
    header.token.fill(0x51);
    header.sessionId = 9;
    header.roomId = 21;
    header.streamEpoch = 4;
    return header;
}

VideoPacket packetFor(const MediaHeader &id, quint64 sequence, int bytes, bool keyFrame,
                      bool discontinuity = false)
{
    VideoPacket packet;
    packet.token = id.token;
    packet.sessionId = id.sessionId;
    packet.roomId = id.roomId;
    packet.streamEpoch = id.streamEpoch;
    packet.sequence = sequence;
    packet.pts = qint64(sequence) * 33333;
    packet.duration = 33333;
    if (keyFrame) packet.flags |= quint16(MediaFlag::KeyFrame);
    if (discontinuity) packet.flags |= quint16(MediaFlag::Discontinuity);
    packet.data = QByteArray(bytes, char('a' + int(sequence % 26)));
    return packet;
}

void pushPacket(StreamReceiver &receiver, const MediaHeader &id, quint64 sequence, int bytes,
                bool keyFrame, qint64 now, QList<VideoPacket> &out)
{
    for (const auto &fragment : fragmentVideo(packetFor(id, sequence, bytes, keyFrame)))
        receiver.push(fragment, now, out);
}

} // namespace

class Stage5CongestionTests : public QObject {
    Q_OBJECT

private slots:
    void publishQueueCountsTheActivePacketInBothLimits()
    {
        PublishQueue queue;
        QVERIFY(queue.push(packetFor(identity(), 0, 1024 * 1024, true), 0));
        QVERIFY(queue.push(packetFor(identity(), 1, 1024 * 1024, false), 0));
        VideoPacket active;
        QVERIFY(queue.acquire(0, active));
        QCOMPARE(queue.activeBytes(), qint64(1024 * 1024));
        QCOMPARE(queue.count(), 2);
        QCOMPARE(queue.bytes(), qint64(2 * 1024 * 1024));
        QVERIFY(queue.push(packetFor(identity(), 2, 1024 * 1024, false), 0));
        QVERIFY(queue.push(packetFor(identity(), 3, 1024 * 1024, false), 0));
        // active + three queued = four packets and exactly 4 MiB: the last slot.
        QCOMPARE(queue.count(), 4);
        QCOMPARE(queue.bytes(), qint64(4 * 1024 * 1024));
        QVERIFY(!queue.push(packetFor(identity(), 4, 1024, false), 0));
        QCOMPARE(queue.count(), 4);
        QVERIFY(queue.bytes() <= PublishQueue::MaxBytes);
        QCOMPARE(queue.rejected(), quint64(1));

        queue.releaseActive();
        QCOMPARE(queue.count(), 3);
        QCOMPARE(queue.bytes(), qint64(3 * 1024 * 1024));
        QVERIFY(!queue.push(packetFor(identity(), 5, 2 * 1024 * 1024, false), 0));
        QVERIFY(queue.bytes() <= PublishQueue::MaxBytes);
    }

    void publishQueueDropsStaleEntriesAndKeepsTheActivePacketBounded()
    {
        PublishQueue queue;
        QVERIFY(queue.push(packetFor(identity(), 0, 1000, true), 0));
        VideoPacket stale;
        QVERIFY(!queue.acquire(200001, stale));
        QCOMPARE(queue.expired(), quint64(1));
        QCOMPARE(queue.count(), 0);

        // A fresh entry survives the stale prefix. It has to be an independent key
        // frame: a delta behind an expired packet is unusable and retracted by the
        // dependency rule (covered separately below).
        QVERIFY(queue.push(packetFor(identity(), 1, 1000, true), 0));
        QVERIFY(queue.push(packetFor(identity(), 2, 1000, true), 200001));
        QCOMPARE(queue.count(), 1);
        QCOMPARE(queue.expired(), quint64(2));

        // The active packet is counted in the budget but is not age-limited, so a
        // slow drain cannot grow the queue; it can only block new packets.
        QVERIFY(queue.acquire(300000, stale));
        QCOMPARE(queue.activeBytes(), qint64(1000));
        QVERIFY(queue.push(packetFor(identity(), 3, 1000, false), 900000));
        QCOMPARE(queue.count(), 2);
        QVERIFY(queue.bytes() <= PublishQueue::MaxBytes);
        queue.releaseActive();
        QCOMPARE(queue.count(), 1);
    }

    void publishQueueDropsDependentsOfAnExpiredKeyFrame()
    {
        PublishQueue queue;
        QVERIFY(queue.push(packetFor(identity(), 10, 1000, true), 0));
        QVERIFY(queue.push(packetFor(identity(), 11, 1000, false), 100000));
        VideoPacket acquired;
        // The key frame aged out while the delta was still queued: the delta must
        // not be handed to the sender, because the viewer can never decode it.
        QVERIFY(!queue.acquire(200001, acquired));
        QCOMPARE(queue.expired(), quint64(1));
        QCOMPARE(queue.dependencyDropped(), quint64(1));
        QCOMPARE(queue.invalidations(), quint64(1));
        QVERIFY(queue.waitingKeyFrame());
        QCOMPARE(queue.count(), 0);

        // Later dependents are refused before they occupy the bounded queue.
        QVERIFY(!queue.push(packetFor(identity(), 12, 1000, false), 200010));
        QCOMPARE(queue.dependencyDropped(), quint64(2));
        QCOMPARE(queue.count(), 0);

        // The next independent key frame reopens the chain.
        QVERIFY(queue.push(packetFor(identity(), 13, 1000, true), 200020));
        QVERIFY(!queue.waitingKeyFrame());
        QVERIFY(queue.acquire(200030, acquired));
        QCOMPARE(acquired.sequence, quint64(13));
        QVERIFY(acquired.keyFrame());
        queue.releaseActive();
        QVERIFY(queue.push(packetFor(identity(), 14, 1000, false), 200040));
        QVERIFY(queue.acquire(200050, acquired));
        QCOMPARE(acquired.sequence, quint64(14));
    }

    void publishQueueActiveAbortInvalidatesQueuedDependents()
    {
        PublishQueue queue;
        QVERIFY(queue.push(packetFor(identity(), 20, 1000, true), 0));
        QVERIFY(queue.push(packetFor(identity(), 21, 1000, false), 0));
        QVERIFY(queue.push(packetFor(identity(), 22, 1000, false), 0));
        QCOMPARE(queue.count(), 3);
        VideoPacket acquired;
        QVERIFY(queue.acquire(1000, acquired));
        QCOMPARE(acquired.sequence, quint64(20));
        QCOMPARE(queue.count(), 3);
        // The active key frame was dropped after admission (send deadline or a
        // failed datagram): its queued dependents become unusable immediately.
        queue.abortActive();
        QCOMPARE(queue.activeBytes(), qint64(0));
        QCOMPARE(queue.count(), 0);
        QCOMPARE(queue.dependencyDropped(), quint64(2));
        QVERIFY(queue.waitingKeyFrame());
        QVERIFY(!queue.acquire(2000, acquired));
        QVERIFY(!queue.push(packetFor(identity(), 23, 1000, false), 2100));

        // A fresh key frame is admitted and the next delta follows it.
        QVERIFY(queue.push(packetFor(identity(), 24, 1000, true), 3000));
        QVERIFY(queue.acquire(4000, acquired));
        QCOMPARE(acquired.sequence, quint64(24));
        QCOMPARE(queue.count(), 1);
        queue.releaseActive();
    }

    void publishQueueOverflowRefusesFollowingDependentsUntilAKeyFrame()
    {
        PublishQueue queue;
        // Fill the hard budget: one active packet plus three queued, 4 MiB total.
        QVERIFY(queue.push(packetFor(identity(), 30, 1024 * 1024, true), 0));
        VideoPacket acquired;
        QVERIFY(queue.acquire(0, acquired));
        QVERIFY(queue.push(packetFor(identity(), 31, 1024 * 1024, true), 0));
        QVERIFY(queue.push(packetFor(identity(), 32, 1024 * 1024, true), 0));
        QVERIFY(queue.push(packetFor(identity(), 33, 1024 * 1024, true), 0));
        QCOMPARE(queue.count(), 4);
        // The overflow drops a packet; everything encoded after it depends on it.
        QVERIFY(!queue.push(packetFor(identity(), 34, 1000, false), 0));
        QCOMPARE(queue.rejected(), quint64(1));
        QVERIFY(queue.waitingKeyFrame());
        QCOMPARE(queue.count(), 4);
        QVERIFY(!queue.push(packetFor(identity(), 35, 1000, false), 0));
        QCOMPARE(queue.dependencyDropped(), quint64(1));
        // Queued data before the break stays valid; a new key frame reopens the chain.
        queue.releaseActive();
        QCOMPARE(queue.count(), 3);
        QVERIFY(queue.push(packetFor(identity(), 36, 1024 * 1024, true), 0));
        QVERIFY(!queue.waitingKeyFrame());
        QCOMPARE(queue.count(), 4);
        QVERIFY(queue.bytes() <= PublishQueue::MaxBytes);
    }

    void publishQueueClearResetsTheDependencyChain()
    {
        PublishQueue queue;
        QVERIFY(queue.push(packetFor(identity(), 40, 1000, true), 0));
        QVERIFY(queue.push(packetFor(identity(), 41, 1000, false), 0));
        VideoPacket acquired;
        QVERIFY(queue.acquire(1000, acquired));
        queue.abortActive();
        QVERIFY(queue.waitingKeyFrame());
        // A stream switch or stop clears the queue together with its chain state.
        queue.clear();
        QVERIFY(!queue.waitingKeyFrame());
        QCOMPARE(queue.count(), 0);
        QVERIFY(queue.push(packetFor(identity(), 42, 1000, false), 2000));
        QVERIFY(queue.acquire(3000, acquired));
        QCOMPARE(acquired.sequence, quint64(42));
    }

    void byteRateLimiterRefundsAWholePacketReservation()
    {
        ByteRateLimiter limiter;
        limiter.reset(312500.0, 1024 * 1024, 0);
        QCOMPARE(limiter.capacity(), qint64(1024 * 1024));
        // A whole packet is reserved before it starts, so an aborted packet must
        // give its unsent bytes back or the next packet waits for nothing.
        QCOMPARE(limiter.take(900000, 0), qint64(0));
        QVERIFY(limiter.available(0) < 200000);
        limiter.refund(900000, 0);
        QCOMPARE(limiter.available(0), qint64(1024 * 1024));
        // A refund is capped by the burst (no credit can be accumulated).
        limiter.refund(900000, 0);
        QCOMPARE(limiter.available(0), qint64(1024 * 1024));
    }

    void byteRateLimiterPacesWithBurstAndRestsOnProfileRate()
    {
        constexpr double rate = 312500.0; // 2.5 Mbit/s in bytes per second
        ByteRateLimiter limiter;
        limiter.reset(rate, 1024 * 1024, 0);
        QCOMPARE(limiter.take(1024 * 1024, 0), qint64(0));
        const auto wait = limiter.take(1000, 0);
        QVERIFY2(wait >= 3100 && wait <= 3300, qPrintable(QString::number(wait)));
        // The rejected take consumed nothing: after exactly the wait it succeeds.
        QCOMPARE(limiter.take(1000, wait), qint64(0));
        QCOMPARE(limiter.available(60'000'000), qint64(1024 * 1024));

        ByteRateLimiter disabled;
        disabled.reset(0, 1024 * 1024, 0);
        QVERIFY(!disabled.enabled());
        QCOMPARE(disabled.take(1232, 0), qint64(0));
    }

    void reassemblerCapsEightPacketsAndEightMiBAndKeepsTheFirstFragmentDeadline()
    {
        const auto id = identity();
        PacketReassembler reassembler(id);
        QList<QList<QByteArray>> fragments;
        for (int index = 0; index < 9; ++index) {
            auto list = fragmentVideo(packetFor(id, quint64(index), 1024 * 1024, true));
            QVERIFY(!list.isEmpty());
            fragments.append(list);
            VideoPacket packet;
            const auto result = reassembler.push(list.first(), 0, packet);
            if (index < 8) {
                QCOMPARE(result, AssemblyResult::Pending);
            } else {
                QCOMPARE(result, AssemblyResult::Rejected);
            }
        }
        QCOMPARE(reassembler.pendingPackets(), 8);
        QCOMPARE(reassembler.bufferedBytes(), 8 * 1024 * 1024);
        QVERIFY(reassembler.peakBufferedBytes() <= PacketReassembler::MaxBytes);
        QVERIFY(reassembler.peakPendingPackets() <= PacketReassembler::MaxPackets);
        reassembler.expire(200001);
        QCOMPARE(reassembler.pendingPackets(), 0);
        QCOMPARE(reassembler.expiredPackets(), quint64(8));

        // A later fragment must not extend the 200 ms deadline of the first one.
        PacketReassembler deadline(id);
        VideoPacket packet;
        auto late = fragmentVideo(packetFor(id, 40, 4000, true));
        QCOMPARE(deadline.push(late.first(), 0, packet), AssemblyResult::Pending);
        QCOMPARE(deadline.push(late.at(1), 150000, packet), AssemblyResult::Pending);
        deadline.expire(200001);
        QCOMPARE(deadline.pendingPackets(), 0);
        QCOMPARE(deadline.expiredPackets(), quint64(1));
    }

    void orderedCommitHoldsAtMostEightCompletedPackets()
    {
        const auto id = identity();
        StreamReceiver receiver;
        receiver.configure(id, false, 0);
        QList<VideoPacket> out;
        pushPacket(receiver, id, 0, 900, true, 0, out);
        QCOMPARE(out.size(), 1);
        out.clear();
        // Packets 1..9 are lost; 10..18 complete out of order and are held.
        for (quint64 sequence = 10; sequence <= 18; ++sequence)
            pushPacket(receiver, id, sequence, 1000, false, 1000 + qint64(sequence), out);
        auto stats = receiver.stats();
        QVERIFY(stats.peakHeldPackets <= StreamReceiver::MaxHeldPackets);
        QVERIFY(stats.peakHeldBytes <= StreamReceiver::MaxHeldBytes);
        QCOMPARE(stats.peakHeldPackets, 8);
        QVERIFY(stats.heldOverflow >= 1);
        QVERIFY(stats.heldPackets <= StreamReceiver::MaxHeldPackets);
        QVERIFY(out.size() >= 8);
        // The oldest held packet starts the delivered range; no reordering.
        for (int index = 1; index < out.size(); ++index)
            QVERIFY(out.at(index).sequence > out.at(index - 1).sequence);
    }
};

QTEST_GUILESS_MAIN(Stage5CongestionTests)
#include "Stage5CongestionTests.moc"
