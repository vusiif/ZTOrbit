// Stage-5 remediation unit coverage (2026-10-24 round R4/R1-residual/R5/R6).
//
// These cases pin the contracts that the independent re-review found missing:
//
// - SendReservation: a packet that was never admitted has nothing to refund, an
//   aborted packet refunds exactly the reserved-but-unwritten bytes once, and
//   complete/clear are idempotent. This is the accounting the real publisher now
//   uses for admission failure, timeout, abort, encode/socket failure, clear and
//   stop.
// - PublishQueue: the loss gate is generation based, so a key frame that was
//   queued before the gap is a usable pre-gap anchor but can never reopen the
//   gate for packets encoded after the gap; expiry, overflow, active abort,
//   repeated recovery-key loss, clear and close keep that rule.
// - The send slot can ask whether a queued key frame already supersedes the
//   packet that is waiting for budget.

#include <desktop-share/ByteRateLimiter.h>
#include <desktop-share/PublishQueue.h>
#include <desktop-share/SendReservation.h>

#include <QtTest>

using namespace vusiif::zto::desktop_share;

namespace {

VideoPacket packet(quint64 sequence, int bytes, bool keyFrame)
{
    VideoPacket value;
    value.token.fill(0x51);
    value.sessionId = 9;
    value.roomId = 21;
    value.streamEpoch = 4;
    value.sequence = sequence;
    value.duration = 33333;
    if (keyFrame) value.flags |= quint16(MediaFlag::KeyFrame);
    value.data = QByteArray(bytes, char('a' + int(sequence % 26)));
    return value;
}

} // namespace

class Stage5RemediationTests : public QObject {
    Q_OBJECT

private slots:
    // --- R4: reserved / written / refunded lifecycle -----------------------
    void reservationNeverRefundsAPacketThatWasNotAdmitted()
    {
        SendReservation reservation;
        QVERIFY(!reservation.active());
        // The rejected packet was never admitted, so dropping it must not create
        // pacing credit. This is the exact P1 defect in isolation.
        QCOMPARE(reservation.release(), qint64(0));
        QCOMPARE(reservation.release(), qint64(0));
        QVERIFY(!reservation.active());
    }

    void reservationRefundsOnlyReservedButUnwrittenBytes()
    {
        SendReservation reservation;
        const qint64 cost = 800 * 1232; // 900,000 payload bytes as wire cost
        QVERIFY(reservation.reserve(cost));
        QCOMPARE(reservation.reserved(), cost);
        reservation.written(1232 * 10);
        QCOMPARE(reservation.writtenBytes(), qint64(1232 * 10));
        QCOMPARE(reservation.refundable(), cost - 1232 * 10);
        QCOMPARE(reservation.release(), cost - 1232 * 10);
        QVERIFY(!reservation.active());
        // Idempotent: a duplicated abort must not refund a second time.
        QCOMPARE(reservation.release(), qint64(0));
        QCOMPARE(reservation.refundable(), qint64(0));
    }

    void reservationCompleteAndClearPathsAreIdempotent()
    {
        SendReservation reservation;
        QVERIFY(reservation.reserve(5000));
        reservation.written(5000);
        reservation.complete();
        QVERIFY(!reservation.active());
        QCOMPARE(reservation.release(), qint64(0));

        // Written bytes can never exceed the reservation, so the refund stays 0.
        QVERIFY(reservation.reserve(2000));
        reservation.written(9000);
        QCOMPARE(reservation.writtenBytes(), qint64(2000));
        QCOMPARE(reservation.release(), qint64(0));

        // A second reservation must not start before the first one is ended.
        QVERIFY(reservation.reserve(1000));
        QVERIFY(!reservation.reserve(2000));
        QCOMPARE(reservation.reserved(), qint64(1000));
        QCOMPARE(reservation.release(), qint64(1000));
    }

    // The refund amount is exactly what keeps the token bucket honest: a packet
    // dropped after admission must end with the pacer holding the bytes it never
    // wrote, never more than the burst.
    void refundAfterAdmissionCannotPushTheBucketBeyondItsBurst()
    {
        ByteRateLimiter limiter;
        limiter.reset(312500.0, 1024 * 1024, 0);
        SendReservation reservation;
        QCOMPARE(limiter.take(900000, 0), qint64(0));
        QVERIFY(reservation.reserve(900000));
        reservation.written(1232 * 3);
        limiter.refund(reservation.release(), 0);
        QCOMPARE(limiter.available(0), qint64(1024 * 1024 - 1232 * 3));
        // A rejected packet (no reservation) contributes no refund at all.
        limiter.refund(reservation.release(), 0);
        QCOMPARE(limiter.available(0), qint64(1024 * 1024 - 1232 * 3));
        QVERIFY(limiter.available(0) <= 1024 * 1024);
    }

    // --- R5: generation based loss gate -----------------------------------
    void preGapAnchorCannotReopenTheGateForPostGapDependents()
    {
        PublishQueue queue;
        // The exact re-review sequence: two keys, two deltas, then the overflow
        // that loses packet 14 while four packets are already queued.
        QVERIFY(queue.push(packet(10, 1000, true), 0));
        QVERIFY(queue.push(packet(11, 1000, true), 0));
        QVERIFY(queue.push(packet(12, 1000, false), 0));
        QVERIFY(queue.push(packet(13, 1000, false), 0));
        QVERIFY(!queue.push(packet(14, 1000, false), 0));
        QCOMPARE(queue.invalidations(), quint64(1));
        QVERIFY(queue.waitingKeyFrame());

        // Acquiring the old queued key (a pre-gap anchor) transmits it but must
        // not certify packets encoded after the loss.
        VideoPacket active;
        QVERIFY(queue.acquire(1000, active));
        QCOMPARE(active.sequence, quint64(10));
        QVERIFY(active.keyFrame());
        QVERIFY(queue.waitingKeyFrame());
        queue.releaseActive();
        QVERIFY2(!queue.push(packet(15, 1000, false), 2000),
                 "a key queued before the lost packet reopened the future dependency gate");

        // The pre-gap chain itself stays deliverable: 11, 12 and 13 were encoded
        // before the loss and follow packet 10 on the wire.
        QVERIFY(queue.acquire(2100, active));
        QCOMPARE(active.sequence, quint64(11));
        queue.releaseActive();
        QVERIFY(queue.acquire(2200, active));
        QCOMPARE(active.sequence, quint64(12));
        queue.releaseActive();
        QVERIFY(queue.acquire(2300, active));
        QCOMPARE(active.sequence, quint64(13));
        queue.releaseActive();
        QVERIFY(queue.waitingKeyFrame());

        // Only a key frame pushed after the loss reopens the gate, and its
        // dependent is then accepted.
        QVERIFY(queue.push(packet(16, 1000, true), 2400));
        QVERIFY(!queue.waitingKeyFrame());
        QVERIFY(queue.push(packet(17, 1000, false), 2500));
        QVERIFY(queue.acquire(2600, active));
        QCOMPARE(active.sequence, quint64(16));
        queue.releaseActive();
        QVERIFY(queue.acquire(2700, active));
        QCOMPARE(active.sequence, quint64(17));
        queue.releaseActive();
    }

    void expiredAnchorClosesTheGateUntilAPostLossKeyArrives()
    {
        PublishQueue queue;
        QVERIFY(queue.push(packet(20, 1000, true), 0));
        QVERIFY(queue.push(packet(21, 1000, false), 100000));
        VideoPacket active;
        QVERIFY(!queue.acquire(200001, active));
        QCOMPARE(queue.expired(), quint64(1));
        QVERIFY(queue.waitingKeyFrame());
        QVERIFY(!queue.push(packet(22, 1000, false), 200002));
        QVERIFY(queue.push(packet(23, 1000, true), 200003));
        QVERIFY(!queue.waitingKeyFrame());
        QVERIFY(queue.acquire(200004, active));
        QCOMPARE(active.sequence, quint64(23));
        queue.releaseActive();
        QVERIFY(queue.push(packet(24, 1000, false), 200005));
        QVERIFY(queue.acquire(200006, active));
        QCOMPARE(active.sequence, quint64(24));
        queue.releaseActive();
    }

    void abortGateIsOnlyReopenedByAKeyNewerThanTheAbortedPacket()
    {
        PublishQueue queue;
        QVERIFY(queue.push(packet(30, 1000, false), 0));
        QVERIFY(queue.push(packet(31, 1000, false), 0));
        // A key frame queued behind the active packet is newer than it: it is a
        // valid recovery anchor once it is transmitted, while the queued deltas
        // that reference the aborted packet are retracted immediately.
        QVERIFY(queue.push(packet(32, 1000, true), 0));
        VideoPacket active;
        QVERIFY(queue.acquire(1000, active));
        QCOMPARE(active.sequence, quint64(30));
        queue.abortActive();
        QCOMPARE(queue.count(), 1);
        QVERIFY(queue.waitingKeyFrame());
        QVERIFY(queue.dependencyDropped() >= 1);
        QVERIFY(!queue.push(packet(33, 1000, false), 2000));
        // The newer key frame reopens the chain and its dependent follows.
        QVERIFY(queue.acquire(3000, active));
        QCOMPARE(active.sequence, quint64(32));
        QVERIFY(active.keyFrame());
        QVERIFY(!queue.waitingKeyFrame());
        queue.releaseActive();
        QVERIFY(queue.push(packet(34, 1000, false), 3100));
        QVERIFY(queue.acquire(3200, active));
        QCOMPARE(active.sequence, quint64(34));
        queue.releaseActive();
    }

    void losingTheRecoveryKeyAgainKeepsTheGateClosed()
    {
        PublishQueue queue;
        VideoPacket active;
        // First loss by overflow: four packets are held, so the fifth is lost.
        QVERIFY(queue.push(packet(40, 1024 * 1024, true), 0));
        QVERIFY(queue.acquire(0, active));
        QVERIFY(queue.push(packet(41, 1024 * 1024, true), 0));
        QVERIFY(queue.push(packet(42, 1024 * 1024, true), 0));
        QVERIFY(queue.push(packet(43, 1024 * 1024, true), 0));
        QVERIFY(!queue.push(packet(44, 1000, false), 0));
        QVERIFY(queue.waitingKeyFrame());
        queue.releaseActive();
        // The recovery key is queued but never acquired because it ages out.
        QVERIFY(queue.push(packet(45, 1000, true), 1000));
        QVERIFY(!queue.waitingKeyFrame());
        queue.tick(300000);
        QCOMPARE(queue.count(), 0);
        QCOMPARE(queue.expired(), quint64(4));
        QVERIFY(queue.waitingKeyFrame());
        QVERIFY(!queue.push(packet(46, 1000, false), 300001));
        QVERIFY(queue.push(packet(47, 1000, true), 300002));
        QVERIFY(!queue.waitingKeyFrame());
        QVERIFY(queue.acquire(300003, active));
        QCOMPARE(active.sequence, quint64(47));
        // The active packet counts as one held packet.
        QCOMPARE(queue.count(), 1);
        queue.releaseActive();
        QCOMPARE(queue.count(), 0);
    }

    void clearAndCloseResetTheGateWithoutRelaxingTheLimits()
    {
        PublishQueue queue;
        VideoPacket active;
        QVERIFY(queue.push(packet(50, 1000, true), 0));
        QVERIFY(queue.push(packet(51, 1000, false), 0));
        QVERIFY(queue.acquire(1000, active));
        queue.abortActive();
        QVERIFY(queue.waitingKeyFrame());
        // A stream epoch switch or stop clears the chain with the queue.
        queue.clear();
        QVERIFY(!queue.waitingKeyFrame());
        QCOMPARE(queue.count(), 0);
        QVERIFY(queue.push(packet(52, 1000, false), 2000));
        QVERIFY(queue.acquire(3000, active));
        QCOMPARE(active.sequence, quint64(52));
        queue.releaseActive();

        // Bounds are unchanged: four packets, 4 MiB, and a closed queue refuses
        // everything.
        queue.clear();
        for (int index = 0; index < PublishQueue::MaxPackets; ++index)
            QVERIFY(queue.push(packet(60 + quint64(index), 1024 * 1024, true), 4000));
        QVERIFY(!queue.push(packet(70, 1000, false), 4000));
        QCOMPARE(queue.count(), PublishQueue::MaxPackets);
        queue.close();
        QVERIFY(queue.closed());
        QCOMPARE(queue.count(), 0);
        QVERIFY(!queue.push(packet(71, 1000, true), 5000));
    }

    void queuedKeyFramePeekDrivesSendSlotPreemption()
    {
        PublishQueue queue;
        QVERIFY(!queue.queuedKeyFrame());
        QVERIFY(queue.push(packet(80, 1000, false), 0));
        QVERIFY(!queue.queuedKeyFrame());
        QVERIFY(queue.push(packet(81, 1000, true), 0));
        QVERIFY(queue.queuedKeyFrame());
        VideoPacket active;
        // Acquiring the delta leaves the key queued, so the send slot can see the
        // newer anchor while a packet of its own is waiting for budget.
        QVERIFY(queue.acquire(1000, active));
        QCOMPARE(active.sequence, quint64(80));
        QVERIFY(queue.queuedKeyFrame());
        queue.releaseActive();
        QVERIFY(queue.acquire(2000, active));
        QCOMPARE(active.sequence, quint64(81));
        QVERIFY(!queue.queuedKeyFrame());
        queue.releaseActive();
    }

    void acquireReportsTheQueueTimestampForThePublicationAge()
    {
        PublishQueue queue;
        VideoPacket active;
        QVERIFY(queue.push(packet(90, 1000, true), 123456));
        qint64 enqueuedUs = 0;
        QVERIFY(queue.acquire(200000, active, &enqueuedUs));
        QCOMPARE(enqueuedUs, qint64(123456));
        queue.releaseActive();
    }
};

QTEST_GUILESS_MAIN(Stage5RemediationTests)
#include "Stage5RemediationTests.moc"
