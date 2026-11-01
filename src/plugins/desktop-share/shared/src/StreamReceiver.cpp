#include <desktop-share/StreamReceiver.h>

#include <algorithm>

namespace vusiif::zto::desktop_share {

StreamReceiver::StreamReceiver() = default;
StreamReceiver::~StreamReceiver() = default;

void StreamReceiver::clear()
{
    if (m_reassembler) m_stats.expiredFragments = m_reassembler->expiredPackets();
    m_identity = {};
    m_reassembler.reset();
    m_held.clear();
    m_heldBytes = 0;
    m_next = 0;
    m_highest = 0;
    m_gapDeadlineUs = 0;
    m_configured = false;
    m_started = false;
    m_keyFrameRequired = false;
    m_waitingKeyFrame = false;
    m_gapArmed = false;
    m_pendingDiscontinuity = false;
    // Statistics survive a leave/teardown so the viewer can report what it
    // actually received; a new epoch (configure) resets them.
}

void StreamReceiver::configure(const MediaHeader &identity, bool keyFrameRequired, qint64 nowUs)
{
    Q_UNUSED(nowUs);
    clear();
    m_stats = {};
    m_peakHeldPackets = 0;
    m_peakHeldBytes = 0;
    m_identity = identity;
    m_identity.type = MediaType::Video;
    m_reassembler = std::make_unique<PacketReassembler>(m_identity);
    m_keyFrameRequired = keyFrameRequired;
    // Both codecs start from a complete key frame: H.264 cannot decode a
    // mid-GOP packet, and the patch codec needs one full baseline before it
    // displays anything.
    m_waitingKeyFrame = true;
    m_configured = true;
}

void StreamReceiver::observe(quint64 sequence, qint64 nowUs)
{
    if (sequence > m_highest) m_highest = sequence;
    if (!m_started || m_gapArmed) return;
    if (sequence > m_next) {
        m_gapArmed = true;
        m_gapDeadlineUs = nowUs + GapTimeoutUs;
    }
}

AssemblyResult StreamReceiver::push(QByteArrayView payload, qint64 nowUs, QList<VideoPacket> &out)
{
    if (!m_configured) return AssemblyResult::Rejected;
    ++m_stats.datagrams;
    VideoPacket packet;
    const auto result = m_reassembler->push(payload, nowUs, packet);
    const auto &header = m_reassembler->lastHeader();
    switch (result) {
    case AssemblyResult::Rejected:
        ++m_stats.rejectedDatagrams;
        return result;
    case AssemblyResult::Ignored:
        ++m_stats.duplicateDatagrams;
        return result;
    case AssemblyResult::Pending:
        observe(header.sequence, nowUs);
        return result;
    case AssemblyResult::Complete:
        ++m_stats.completePackets;
        observe(packet.sequence, nowUs);
        complete(std::move(packet), nowUs, out);
        return result;
    }
    return result;
}

bool StreamReceiver::tick(qint64 nowUs, QList<VideoPacket> &out)
{
    if (!m_configured) return false;
    if (m_reassembler) m_reassembler->expire(nowUs);
    if (!m_started || !m_gapArmed || nowUs < m_gapDeadlineUs) return false;
    declareGap(nowUs, out);
    return true;
}

void StreamReceiver::resync()
{
    m_held.clear();
    m_heldBytes = 0;
    m_pendingDiscontinuity = true;
    if (m_started) m_waitingKeyFrame = true;
    disarmGap();
}

void StreamReceiver::complete(VideoPacket packet, qint64 nowUs, QList<VideoPacket> &out)
{
    Q_UNUSED(nowUs);
    const bool keyFrame = (packet.flags & quint16(MediaFlag::KeyFrame)) != 0;
    const auto sequence = packet.sequence;
    if (!m_started) {
        if (!keyFrame) {
            ++m_stats.droppedUntilKeyFrame;
            return;
        }
        commit(std::move(packet), false, out);
        m_started = true;
        m_next = sequence + 1;
        m_waitingKeyFrame = false;
        disarmGap();
        drainHeld(out);
        return;
    }
    // Once a start is established, even recovery must advance the committed
    // position. A delayed, previously incomplete key cannot rewind it.
    if (sequence < m_next) {
        ++m_stats.latePackets;
        return;
    }
    if (m_waitingKeyFrame) {
        if (!keyFrame) {
            ++m_stats.droppedUntilKeyFrame;
            return;
        }
        m_held.clear();
        m_heldBytes = 0;
        commit(std::move(packet), true, out);
        m_waitingKeyFrame = false;
        m_next = sequence + 1;
        disarmGap();
        drainHeld(out);
        return;
    }
    if (packet.sequence < m_next) {
        ++m_stats.latePackets;
        return;
    }
    if (packet.sequence == m_next) {
        commit(std::move(packet), m_pendingDiscontinuity, out);
        ++m_next;
        drainHeld(out);
        return;
    }
    if (m_keyFrameRequired && keyFrame) {
        // An independent key frame makes every earlier packet obsolete: close
        // the gap right away instead of holding the key frame until the
        // deadline. It is committed with the discontinuity marker.
        declareGap(nowUs, out);
        m_waitingKeyFrame = false;
        commit(std::move(packet), true, out);
        m_next = sequence + 1;
        disarmGap();
        drainHeld(out);
        return;
    }
    // A later packet completed before the missing one. Hold it in order until
    // the gap is resolved by the missing packet or by the gap deadline.
    if (m_held.size() >= MaxHeldPackets || m_heldBytes + packet.data.size() > MaxHeldBytes) {
        ++m_stats.heldOverflow;
        declareGap(nowUs, out);
        if (m_waitingKeyFrame) {
            if (!keyFrame) {
                ++m_stats.droppedUntilKeyFrame;
                return;
            }
            m_held.clear();
            m_heldBytes = 0;
            commit(std::move(packet), true, out);
            m_waitingKeyFrame = false;
            m_next = sequence + 1;
            disarmGap();
            drainHeld(out);
            return;
        }
        if (packet.sequence < m_next) {
            ++m_stats.latePackets;
            return;
        }
        if (packet.sequence == m_next) {
            commit(std::move(packet), m_pendingDiscontinuity, out);
            ++m_next;
            disarmGap();
            drainHeld(out);
            return;
        }
    }
    if (!hold(std::move(packet))) ++m_stats.rejectedDatagrams;
}

bool StreamReceiver::hold(VideoPacket packet)
{
    if (packet.data.size() > MaxHeldBytes) return false;
    m_heldBytes += packet.data.size();
    m_held.insert(packet.sequence, std::move(packet));
    m_peakHeldPackets = std::max(m_peakHeldPackets, int(m_held.size()));
    m_peakHeldBytes = std::max(m_peakHeldBytes, m_heldBytes);
    return true;
}

void StreamReceiver::drainHeld(QList<VideoPacket> &out)
{
    for (;;) {
        auto it = m_held.find(m_next);
        if (it == m_held.end()) break;
        auto packet = std::move(it.value());
        m_heldBytes -= packet.data.size();
        m_held.erase(it);
        commit(std::move(packet), m_pendingDiscontinuity, out);
        ++m_next;
    }
    // Keep the original observation deadline while any observed successor
    // remains uncommitted, including fragments of an incomplete packet.
    if (m_next > m_highest) disarmGap();
}

void StreamReceiver::declareGap(qint64 nowUs, QList<VideoPacket> &out)
{
    const auto observedDeadline = m_gapDeadlineUs;
    ++m_stats.gaps;
    ++m_stats.missingPackets;
    disarmGap();
    m_pendingDiscontinuity = true;
    if (m_keyFrameRequired) {
        // H.264: every buffered packet depends on the missing one.
        m_held.clear();
        m_heldBytes = 0;
        m_waitingKeyFrame = true;
        return;
    }
    if (m_held.isEmpty()) {
        // Nothing completed after the gap: jump to the highest observed
        // sequence (a fragment header proves the sender emitted it) so the
        // next completed packet can commit without another wait.
        m_next = std::max(m_highest, m_next + 1);
        return;
    }
    m_next = m_held.firstKey();
    drainHeld(out);
    if (m_next <= m_highest) {
        // Skipping one missing range does not resolve a second range already
        // evidenced by held packets or partial successors. Retain its deadline
        // so a quiet stream cannot strand those packets indefinitely.
        m_gapArmed = true;
        m_gapDeadlineUs = observedDeadline ? observedDeadline : nowUs + GapTimeoutUs;
    }
}

void StreamReceiver::commit(VideoPacket packet, bool discontinuity, QList<VideoPacket> &out)
{
    if (discontinuity) packet.flags |= quint16(MediaFlag::Discontinuity);
    m_pendingDiscontinuity = false;
    ++m_stats.submittedPackets;
    out.append(std::move(packet));
}

void StreamReceiver::disarmGap()
{
    m_gapArmed = false;
    m_gapDeadlineUs = 0;
}

StreamReceiver::Stats StreamReceiver::stats() const
{
    if (m_reassembler) m_stats.expiredFragments = m_reassembler->expiredPackets();
    m_stats.highestSequence = m_highest;
    m_stats.heldPackets = int(m_held.size());
    m_stats.heldBytes = m_heldBytes;
    m_stats.pendingPackets = m_reassembler ? m_reassembler->pendingPackets() : 0;
    m_stats.bufferedBytes = m_reassembler ? m_reassembler->bufferedBytes() : 0;
    m_stats.peakPendingPackets = m_reassembler ? m_reassembler->peakPendingPackets() : 0;
    m_stats.peakBufferedBytes = m_reassembler ? m_reassembler->peakBufferedBytes() : 0;
    m_stats.peakHeldPackets = m_peakHeldPackets;
    m_stats.peakHeldBytes = m_peakHeldBytes;
    return m_stats;
}

} // namespace vusiif::zto::desktop_share
