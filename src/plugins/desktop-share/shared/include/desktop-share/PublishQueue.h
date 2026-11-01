#pragma once

#include <desktop-share/VideoMailbox.h>

namespace vusiif::zto::desktop_share {

// Publisher-side bounded queue. It combines the queued encoded packets with the
// one packet currently being admitted/fragmented/sent, so the hard limits cover
// the complete held set:
//
// - at most four encoded packets (the active packet counts as one),
// - at most 4 MiB including the active packet,
// - queued entries older than 200 ms are discarded before admission.
//
// It also owns the dependency rule for the producer's packet chain. A codec
// packet that is not a key frame depends on every earlier packet, so once a
// packet is lost locally (age, budget, or an aborted active packet) the queued
// dependents are dropped and further non-key pushes are refused until a fresh
// key frame is accepted. This is what keeps an expired key frame from being
// followed on the wire by the delta packets that referenced it; a GUI-only
// notification cannot retract data that is already queued.
//
// The gate is positioned at the lost packet, not at the moment the loss is
// noticed. Every accepted push gets a strictly increasing push sequence, and a
// loss records the sequence of the packet that was lost:
//
// - a packet whose sequence is at or below the loss point was encoded before the
//   gap. It belongs to the chain that preceded the gap, so it may still be
//   transmitted ("pre-gap anchor"), but it can never certify packets encoded
//   after the gap;
// - a key frame pushed after the loss point is the recovery anchor and clears
//   the gate; every dependent pushed after the loss point is refused until it
//   has been accepted.
//
// This is what separates "queued before the gap" from "encoded after the gap": a
// key frame that was already queued when an overflow lost a newer packet stays a
// usable pre-gap anchor without reopening the gate, while a key frame that is
// newer than an aborted active packet is still a valid recovery anchor. The
// network thread owns this state; the GUI notification is only a supplement.
//
// The active packet itself is bounded by the 4 MiB total and by the wire packet
// limit; the age limit applies to queued entries and is enforced by the network
// thread for a packet that waits for its whole-packet send budget.
class PublishQueue {
public:
    static constexpr int MaxPackets = 4;
    static constexpr qint64 MaxBytes = 4 * 1024 * 1024;
    static constexpr qint64 MaxAgeUs = 200000;

    bool push(VideoPacket packet, qint64 nowUs)
    {
        if (m_closed) return false;
        const quint64 sequence = ++m_pushSequence;
        const qint64 bytes = packet.data.size();
        if (bytes <= 0 || bytes > MaxBytes) {
            ++m_rejected;
            raiseGate(sequence);
            return false;
        }
        // Stale entries must not keep occupying the budget while a newer packet
        // waits behind them; the expiry also breaks the dependency chain.
        pruneLocked(nowUs);
        if (!packet.keyFrame() && m_waitingKeyFrame) {
            ++m_dependencyRejected;
            return false;
        }
        const int held = m_queue.count() + (m_activeBytes > 0 ? 1 : 0);
        const qint64 heldBytes = m_queue.bytes() + m_activeBytes;
        if (held + 1 > MaxPackets || heldBytes + bytes > MaxBytes) {
            ++m_rejected;
            // The rejected packet is newer than everything queued, so the queued
            // entries stay valid pre-gap anchors; only packets encoded after this
            // point become unusable. The loss point is the rejected packet.
            raiseGate(sequence);
            return false;
        }
        // A key frame pushed after the loss is the recovery anchor for every
        // dependent that follows it; one that was already queued before the loss
        // never reaches this branch, so it cannot reopen the gate.
        if (packet.keyFrame() && sequence > m_lossSequence) m_waitingKeyFrame = false;
        return m_queue.push(std::move(packet), nowUs, sequence);
    }

    // Moves the next valid packet out and marks it active. Only one packet can
    // be active at a time because the network thread admits and sends it before
    // taking the next one. `enqueuedUs` reports the queue timestamp so the caller
    // can keep enforcing the publication age while it waits for budget.
    bool acquire(qint64 nowUs, VideoPacket &out, qint64 *enqueuedUs = nullptr)
    {
        if (m_closed || m_activeBytes > 0) return false;
        pruneLocked(nowUs);
        qint64 enqueued = 0;
        quint64 sequence = 0;
        if (!m_queue.takeOne(nowUs, out, &enqueued, &sequence)) return false;
        if (m_waitingKeyFrame && sequence > m_lossSequence) {
            if (!out.keyFrame()) {
                // Encoded after the gap: only a recovery key frame may pass.
                ++m_dependencyRejected;
                return false;
            }
            m_waitingKeyFrame = false;
        }
        // A packet at or below the loss point is a pre-gap anchor: it may be
        // transmitted but never reopens the gate.
        m_activeBytes = out.data.size();
        m_activeSequence = sequence;
        if (enqueuedUs) *enqueuedUs = enqueued;
        return true;
    }

    // The active packet was transmitted completely.
    void releaseActive()
    {
        m_activeBytes = 0;
        m_activeSequence = 0;
    }

    // The active packet is dropped after admission (send deadline, socket
    // failure or a packet that can never fit the burst). Every queued packet
    // follows it in the chain, so its dependents are discarded and new non-key
    // pushes are refused until a fresh key frame arrives. The loss point is the
    // dropped packet itself, so a key frame that was already queued behind it (a
    // newer intra frame) is still a valid recovery anchor.
    void abortActive()
    {
        const quint64 lost = m_activeSequence;
        m_activeBytes = 0;
        m_activeSequence = 0;
        const int dropped = m_queue.dropNonKeyFrames();
        m_dependencyDropped += quint64(dropped);
        raiseGate(lost);
    }

    // Discards queued entries that exceeded the age limit. The network thread
    // calls this on its periodic tick so a stalled drain cannot leave invalid
    // packets in the queue without a push or acquire.
    void tick(qint64 nowUs) { pruneLocked(nowUs); }

    void close()
    {
        m_closed = true;
        clear();
    }

    void clear()
    {
        m_queue.clear();
        m_activeBytes = 0;
        m_activeSequence = 0;
        m_waitingKeyFrame = false;
    }

    int count() const { return m_queue.count() + (m_activeBytes > 0 ? 1 : 0); }
    qint64 bytes() const { return m_queue.bytes() + m_activeBytes; }
    int queuedPackets() const { return m_queue.count(); }
    // True while a queued packet is an independent key frame; the network thread
    // uses it to let a newer anchor supersede a packet that is waiting for send
    // budget instead of blocking the fresher picture behind it.
    bool queuedKeyFrame() const { return m_queue.containsKeyFrame(); }
    qint64 activeBytes() const { return m_activeBytes; }
    quint64 rejected() const { return m_rejected + m_queue.rejected(); }
    quint64 expired() const { return m_queue.expired(); }
    // Packets refused or discarded because their dependency was already lost.
    quint64 dependencyDropped() const { return m_dependencyDropped + m_dependencyRejected; }
    // Number of times the packet chain broke (age, budget or aborted active).
    quint64 invalidations() const { return m_invalidations; }
    bool waitingKeyFrame() const { return m_waitingKeyFrame; }
    bool closed() const { return m_closed; }

private:
    // Places the loss point at `sequence` and closes the gate. The loss point
    // never moves backwards: an older loss cannot make newer packets look like
    // pre-gap anchors.
    void raiseGate(quint64 sequence)
    {
        ++m_invalidations;
        m_waitingKeyFrame = true;
        if (sequence > m_lossSequence) m_lossSequence = sequence;
    }

    void pruneLocked(qint64 nowUs)
    {
        if (m_queue.count() == 0) return;
        quint64 highestExpired = 0;
        m_queue.prune(nowUs, &highestExpired);
        if (highestExpired == 0) return;
        // Everything still queued is newer than the expired packet, so its
        // dependents are unusable. Independent key frames survive as anchors.
        const int dependents = m_queue.dropNonKeyFrames();
        m_dependencyDropped += quint64(dependents);
        raiseGate(highestExpired);
    }

    VideoMailbox m_queue{MaxPackets, MaxBytes, MaxAgeUs};
    qint64 m_activeBytes = 0;
    quint64 m_activeSequence = 0;
    quint64 m_pushSequence = 0;
    quint64 m_lossSequence = 0;
    quint64 m_rejected = 0;
    quint64 m_dependencyRejected = 0;
    quint64 m_dependencyDropped = 0;
    quint64 m_invalidations = 0;
    bool m_waitingKeyFrame = false;
    bool m_closed = false;
};

} // namespace vusiif::zto::desktop_share
