#include <desktop-share/PacketReassembler.h>
#include <desktop-share/StreamReceiver.h>
#include <desktop-share/VideoMailbox.h>
#include "../../plugins/desktop-share/client/MediaEndpointActivity.h"

#include <QtTest>

using namespace vusiif::zto::desktop_share;

namespace {

MediaHeader identity()
{
    MediaHeader header;
    header.token.fill(0x41);
    header.sessionId = 7;
    header.roomId = 11;
    header.streamEpoch = 3;
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

QList<QByteArray> fragmentsFor(const MediaHeader &id, quint64 sequence, int bytes, bool keyFrame,
                               bool discontinuity = false)
{
    return fragmentVideo(packetFor(id, sequence, bytes, keyFrame, discontinuity));
}

// Pushes every fragment of one packet and appends the completed packets.
void pushPacket(StreamReceiver &receiver, const QList<QByteArray> &fragments, qint64 now,
                QList<VideoPacket> &out)
{
    for (const auto &fragment : fragments) receiver.push(fragment, now, out);
}

} // namespace

class StreamLogicTests : public QObject {
    Q_OBJECT

private slots:
    void continuousMediaSurvivesAnEmptyTimeoutBoundaryPoll() {
        for (bool publisher : {false, true}) {
            MediaEndpointActivity activity;
            activity.acknowledge(0);
            quint64 count = 0;
            qint64 nextMedia = 1000;
            for (qint64 now = 0; now <= 40000000; now += 5000) {
                if (now >= nextMedia) { ++count; nextMedia += 33333; }
                activity.observe(now, publisher ? count : 0, publisher ? 0 : count);
                QVERIFY2(!activity.expired(now, 15000000), "active endpoint expired on a no-traffic poll");
            }
            auto last = activity.lastActivityUs();
            activity.observe(last + 14999999, publisher ? count : 0, publisher ? 0 : count);
            QVERIFY(!activity.expired(last + 14999999, 15000000));
            QVERIFY(activity.expired(last + 15000000, 15000000));
        }
    }
    void endpointResetCannotReuseOldCountersAsTraffic() {
        MediaEndpointActivity activity;
        activity.reset(100, 200);
        activity.acknowledge(100000000);
        activity.observe(114999999, 100, 200);
        QVERIFY(activity.expired(115000000, 15000000));
        activity.acknowledge(115000000);
        QVERIFY(!activity.expired(115000000, 15000000));
        activity.reset(100, 200);
        QVERIFY(activity.expired(115000000, 15000000));
    }
    void orderedCommitHoldsOutOfOrderPacketsAndIgnoresDuplicates()
    {
        const auto id = identity();
        StreamReceiver receiver;
        receiver.configure(id, true, 0);
        QList<VideoPacket> out;
        const auto first = fragmentsFor(id, 10, 3000, true);
        pushPacket(receiver, first, 0, out);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.first().sequence, quint64(10));
        QVERIFY(!out.first().discontinuity());
        QCOMPARE(receiver.stats().submittedPackets, quint64(1));
        out.clear();

        // Packet 12 arrives before 11: it is held, not submitted.
        const auto third = fragmentsFor(id, 12, 1500, false);
        pushPacket(receiver, third, 1000, out);
        QCOMPARE(out.size(), 0);
        QCOMPARE(receiver.stats().heldPackets, 1);
        // A duplicate fragment of a partial packet is ignored.
        receiver.push(third.first(), 1100, out);
        QCOMPARE(out.size(), 0);

        // Packet 11 completes; 11 and the held 12 are committed in order.
        pushPacket(receiver, fragmentsFor(id, 11, 800, false), 2000, out);
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0).sequence, quint64(11));
        QCOMPARE(out.at(1).sequence, quint64(12));
        QCOMPARE(receiver.stats().heldPackets, 0);
        out.clear();

        // A fully retired duplicate cannot re-enter.
        pushPacket(receiver, first, 3000, out);
        QCOMPARE(out.size(), 0);
        QVERIFY(receiver.stats().duplicateDatagrams >= 1);
    }

    void missingFragmentsDropTheWholePacketAndDoNotRebuildIt()
    {
        const auto id = identity();
        StreamReceiver receiver;
        receiver.configure(id, true, 0);
        QList<VideoPacket> out;
        pushPacket(receiver, fragmentsFor(id, 0, 3000, true), 0, out);
        QCOMPARE(out.size(), 1);
        out.clear();

        auto missingLast = fragmentsFor(id, 1, 3000, false);
        missingLast.removeLast();
        pushPacket(receiver, missingLast, 1000, out);
        QCOMPARE(out.size(), 0);
        receiver.tick(1000 + 199000, out);
        QCOMPARE(out.size(), 0);
        // The first fragment arrived 200ms ago: the packet is expired and cannot
        // be completed afterwards.
        receiver.tick(1000 + 200001, out);
        QVERIFY(receiver.stats().expiredFragments >= 1);
        QCOMPARE(receiver.stats().missingPackets, quint64(0));
        // The expired partial packet is retired: replayed fragments cannot
        // rebuild it, so only a later key frame starts a new range.
        pushPacket(receiver, fragmentsFor(id, 1, 3000, false), 1000 + 220000, out);
        QCOMPARE(out.size(), 0);
        pushPacket(receiver, fragmentsFor(id, 2, 900, true), 1000 + 250000, out);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).sequence, quint64(2));
        QVERIFY(out.at(0).discontinuity());
        QCOMPARE(receiver.stats().missingPackets, quint64(1));
    }

    void wholePacketLossIsDetectedFromTheNextSequenceAndResumesOnAKeyFrame()
    {
        const auto id = identity();
        StreamReceiver receiver;
        receiver.configure(id, true, 0);
        QList<VideoPacket> out;
        pushPacket(receiver, fragmentsFor(id, 0, 2500, true), 0, out);
        QCOMPARE(out.size(), 1);
        out.clear();

        // Packet 1 is entirely lost. The first fragment of packet 2 proves it.
        auto second = fragmentsFor(id, 2, 1200, false);
        receiver.push(second.first(), 100000, out);
        QCOMPARE(out.size(), 0);
        QCOMPARE(receiver.stats().missingPackets, quint64(0));
        // Fewer than 200ms: still waiting for the gap.
        receiver.tick(100000 + 199000, out);
        QCOMPARE(out.size(), 0);
        receiver.tick(100000 + 200001, out);
        QCOMPARE(out.size(), 0);
        QCOMPARE(receiver.stats().gaps, quint64(1));
        QCOMPARE(receiver.stats().missingPackets, quint64(1));
        QVERIFY(receiver.waitingKeyFrame());

        // Late non-key packets are dropped, not submitted.
        pushPacket(receiver, fragmentsFor(id, 3, 800, false), 100000 + 210000, out);
        QCOMPARE(out.size(), 0);
        QVERIFY(receiver.stats().droppedUntilKeyFrame >= 1);

        // The first complete key frame after the loss recovers the stream and
        // carries the discontinuity marker exactly once.
        pushPacket(receiver, fragmentsFor(id, 4, 4000, true), 100000 + 300000, out);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).sequence, quint64(4));
        QVERIFY(out.at(0).discontinuity());
        out.clear();
        pushPacket(receiver, fragmentsFor(id, 5, 700, false), 100000 + 310000, out);
        QCOMPARE(out.size(), 1);
        QVERIFY(!out.at(0).discontinuity());
    }

    void patchCodecCommitsBufferedPacketsAfterGapWithDiscontinuity()
    {
        const auto id = identity();
        StreamReceiver receiver;
        receiver.configure(id, false, 0);
        QList<VideoPacket> out;
        pushPacket(receiver, fragmentsFor(id, 0, 900, true), 0, out);
        QCOMPARE(out.size(), 1);
        out.clear();

        // Packet 1 lost; 2 and 3 complete while the gap is unresolved.
        pushPacket(receiver, fragmentsFor(id, 2, 500, false), 10000, out);
        pushPacket(receiver, fragmentsFor(id, 3, 500, false), 20000, out);
        QCOMPARE(out.size(), 0);
        receiver.tick(10000 + 200001, out);
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0).sequence, quint64(2));
        QCOMPARE(out.at(1).sequence, quint64(3));
        QVERIFY(out.at(0).discontinuity());
        QVERIFY(!out.at(1).discontinuity());
        QCOMPARE(receiver.stats().missingPackets, quint64(1));
    }

    void lateJoinStartsAtTheFirstKeyFrameNotSequenceZero()
    {
        const auto id = identity();
        StreamReceiver receiver;
        receiver.configure(id, true, 0);
        QList<VideoPacket> out;
        pushPacket(receiver, fragmentsFor(id, 40, 900, false), 0, out);
        QCOMPARE(out.size(), 0);
        pushPacket(receiver, fragmentsFor(id, 41, 900, true), 1000, out);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).sequence, quint64(41));
        QVERIFY(!out.at(0).discontinuity());
        out.clear();
        pushPacket(receiver, fragmentsFor(id, 42, 900, false), 2000, out);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).sequence, quint64(42));
    }

    void otherEpochsRoomsSessionsAndTokensAreRejected()
    {
        const auto id = identity();
        StreamReceiver receiver;
        receiver.configure(id, true, 0);
        QList<VideoPacket> out;
        auto foreign = id;
        ++foreign.streamEpoch;
        pushPacket(receiver, fragmentsFor(foreign, 0, 900, true), 0, out);
        QCOMPARE(out.size(), 0);
        foreign = id;
        ++foreign.roomId;
        pushPacket(receiver, fragmentsFor(foreign, 0, 900, true), 0, out);
        foreign = id;
        ++foreign.sessionId;
        pushPacket(receiver, fragmentsFor(foreign, 0, 900, true), 0, out);
        foreign = id;
        foreign.token.fill(0x99);
        pushPacket(receiver, fragmentsFor(foreign, 0, 900, true), 0, out);
        QCOMPARE(out.size(), 0);
        QVERIFY(receiver.stats().rejectedDatagrams >= 4);
        // The real identity still works.
        pushPacket(receiver, fragmentsFor(id, 0, 900, true), 0, out);
        QCOMPARE(out.size(), 1);
    }

    void conflictingFragmentMetadataDropsTheWholePacket()
    {
        const auto id = identity();
        StreamReceiver receiver;
        receiver.configure(id, true, 0);
        QList<VideoPacket> out;
        pushPacket(receiver, fragmentsFor(id, 0, 900, true), 0, out);
        out.clear();
        auto fragments = fragmentsFor(id, 1, 3000, false);
        receiver.push(fragments.first(), 1000, out);
        // Same fragment index and sequence but different encoded bytes.
        auto conflict = fragments.first();
        conflict[conflict.size() - 1] = 'z';
        receiver.push(conflict, 1100, out);
        QCOMPARE(out.size(), 0);
        QVERIFY(receiver.stats().rejectedDatagrams >= 1);
        // The remaining fragments of the rejected packet cannot complete it.
        for (int index = 1; index < fragments.size(); ++index) receiver.push(fragments.at(index), 1200, out);
        QCOMPARE(out.size(), 0);
        // A later key frame starts a new ordered range.
        pushPacket(receiver, fragmentsFor(id, 2, 900, true), 2000, out);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).sequence, quint64(2));
    }

    void reassemblyIsBoundedAndOldSequencesAreNotRebuilt()
    {
        const auto id = identity();
        StreamReceiver receiver;
        receiver.configure(id, true, 0);
        QList<VideoPacket> out;
        pushPacket(receiver, fragmentsFor(id, 0, 900, true), 0, out);
        out.clear();
        // Prime the window with many complete packets.
        for (quint64 sequence = 1; sequence <= 1100; ++sequence)
            pushPacket(receiver, fragmentsFor(id, sequence, 64, false), 1000 + qint64(sequence), out);
        const auto submitted = receiver.stats().submittedPackets;
        // A fragment far behind the newest sequence is ignored without allocating.
        pushPacket(receiver, fragmentsFor(id, 1, 900, false), 5000, out);
        QCOMPARE(receiver.stats().submittedPackets, submitted);
        QVERIFY(receiver.stats().duplicateDatagrams >= 1);
    }

    void skippingOneTileGapKeepsTheRemainingGapDeadline()
    {
        const auto id = identity();
        StreamReceiver receiver;
        receiver.configure(id, false, 0);
        QList<VideoPacket> out;
        pushPacket(receiver, fragmentsFor(id, 0, 100, true), 0, out);
        out.clear();
        pushPacket(receiver, fragmentsFor(id, 3, 100, false), 1000, out);
        pushPacket(receiver, fragmentsFor(id, 5, 100, false), 2000, out);
        QVERIFY(receiver.tick(201001, out));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.first().sequence, quint64(3));
        out.clear();
        QVERIFY(receiver.tick(201002, out));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.first().sequence, quint64(5));
        QCOMPARE(receiver.stats().heldPackets, 0);
    }

    void mailboxEnforcesPacketByteAndAgeLimits()
    {
        VideoMailbox mailbox(4, 8 * 1024 * 1024, 200000);
        for (int index = 0; index < 4; ++index) {
            VideoPacket packet = packetFor(identity(), quint64(index), 1000, false);
            QVERIFY(mailbox.push(packet, 0));
        }
        VideoPacket extra = packetFor(identity(), 9, 1000, false);
        QVERIFY(!mailbox.push(extra, 0));
        QCOMPARE(mailbox.rejected(), quint64(1));
        QCOMPARE(mailbox.count(), 4);
        QList<VideoPacket> out;
        mailbox.take(100000, out);
        QCOMPARE(out.size(), 4);
        out.clear();
        QVERIFY(mailbox.push(packetFor(identity(), 10, 1000, false), 200000));
        mailbox.take(400001, out);
        QCOMPARE(out.size(), 0);
        QCOMPARE(mailbox.expired(), quint64(1));
        QVERIFY(!mailbox.push(packetFor(identity(), 11, 9 * 1024 * 1024, false), 500000));
    }

    void reassemblerExposesHeaderOfIncompletePackets()
    {
        PacketReassembler reassembler(identity());
        auto fragments = fragmentsFor(identity(), 77, 3000, false);
        VideoPacket packet;
        QCOMPARE(reassembler.push(fragments.first(), 0, packet), AssemblyResult::Pending);
        QCOMPARE(reassembler.lastHeader().sequence, quint64(77));
    }
};

QTEST_GUILESS_MAIN(StreamLogicTests)
#include "StreamLogicTests.moc"
