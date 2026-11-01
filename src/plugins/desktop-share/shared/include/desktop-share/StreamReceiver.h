#pragma once

#include <desktop-share/MediaProtocol.h>
#include <desktop-share/PacketReassembler.h>

#include <QList>
#include <QMap>

#include <memory>

namespace vusiif::zto::desktop_share {

// Ordered commit state for one negotiated media identity: media token, local
// session ID, room ID and stream epoch. It owns the fragment reassembler and
// adds the rules that cannot live in it:
//
// - completed encoded packets are committed in wire-sequence order;
// - a whole packet whose fragments were all lost is detected within
//   GapTimeoutUs of observing a larger sequence;
// - a codec that depends on earlier packets resumes only at a complete key
//   frame, which carries the discontinuity flag exactly once;
// - frames from another epoch, room, session or token are rejected by the
//   reassembler's identity check and never touch current state.
//
// The class has no sockets and no Qt event loop dependency; the network thread
// feeds it and a unit test can drive the same state machine deterministically.
class StreamReceiver {
public:
    static constexpr qint64 GapTimeoutUs = 200000;
    static constexpr int MaxHeldPackets = 8;
    static constexpr qint64 MaxHeldBytes = 8 * 1024 * 1024;

    StreamReceiver();
    ~StreamReceiver();

    // Starts a new identity and drops every buffered packet from the previous
    // epoch. keyFrameRequired is true for codecs that cannot start decoding
    // mid-stream (H.264); patch codecs still start at a complete baseline.
    void configure(const MediaHeader &identity, bool keyFrameRequired, qint64 nowUs);
    void clear();

    bool configured() const { return m_configured; }
    const MediaHeader &identity() const { return m_identity; }
    quint64 streamEpoch() const { return m_identity.streamEpoch; }
    bool started() const { return m_started; }
    bool waitingKeyFrame() const { return m_waitingKeyFrame; }

    // One decoded business payload: 88-byte header plus fragment.
    AssemblyResult push(QByteArrayView payload, qint64 nowUs, QList<VideoPacket> &out);

    // Advances the whole-packet-loss deadline and releases expired partial
    // packets. Returns true when a gap was declared during this call.
    bool tick(qint64 nowUs, QList<VideoPacket> &out);

    // Forces the next commit to be a key frame after a local submission was
    // rejected. Buffered dependents are dropped so the decoder is not fed
    // frames that reference the rejected one.
    void resync();

    struct Stats {
        quint64 datagrams = 0;
        quint64 completePackets = 0;
        quint64 submittedPackets = 0;
        quint64 rejectedDatagrams = 0;
        quint64 duplicateDatagrams = 0;
        quint64 latePackets = 0;
        quint64 missingPackets = 0;
        quint64 gaps = 0;
        quint64 droppedUntilKeyFrame = 0;
        quint64 heldOverflow = 0;
        quint64 expiredFragments = 0;
        quint64 highestSequence = 0;
        int heldPackets = 0;
        qint64 heldBytes = 0;
        int pendingPackets = 0;
        int bufferedBytes = 0;
        int peakHeldPackets = 0;
        qint64 peakHeldBytes = 0;
        int peakPendingPackets = 0;
        int peakBufferedBytes = 0;
    };
    Stats stats() const;

private:
    void observe(quint64 sequence, qint64 nowUs);
    void complete(VideoPacket packet, qint64 nowUs, QList<VideoPacket> &out);
    void commit(VideoPacket packet, bool discontinuity, QList<VideoPacket> &out);
    void drainHeld(QList<VideoPacket> &out);
    void declareGap(qint64 nowUs, QList<VideoPacket> &out);
    void disarmGap();
    bool hold(VideoPacket packet);

    MediaHeader m_identity;
    std::unique_ptr<PacketReassembler> m_reassembler;
    QMap<quint64, VideoPacket> m_held;
    qint64 m_heldBytes = 0;
    int m_peakHeldPackets = 0;
    qint64 m_peakHeldBytes = 0;
    quint64 m_next = 0;
    quint64 m_highest = 0;
    qint64 m_gapDeadlineUs = 0;
    bool m_configured = false;
    bool m_started = false;
    bool m_keyFrameRequired = false;
    bool m_waitingKeyFrame = false;
    bool m_gapArmed = false;
    bool m_pendingDiscontinuity = false;
    mutable Stats m_stats;
};

} // namespace vusiif::zto::desktop_share
