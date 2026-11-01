#pragma once

#include <desktop-share/MediaProtocol.h>

#include <QQueue>

namespace vusiif::zto::desktop_share {

// One queued encoded packet plus the monotonic enqueue time used for the age
// limit. The timestamp is never sent on the wire; it only bounds local latency.
// `pushSequence` is the publisher queue's strictly increasing push order; the
// receive mailbox ignores it. The publisher uses it to tell a packet that was
// queued before a loss (a pre-gap anchor) from one encoded after the loss.
struct QueuedVideo {
    VideoPacket packet;
    qint64 enqueuedUs = 0;
    quint64 pushSequence = 0;
};

// Bounded single-producer/single-consumer mailbox for video. The owner adds a
// mutex and a coalesced wakeup around it; the class itself keeps the hard
// packet and byte limits and, for queues with an age limit, drops stale
// entries instead of letting them delay newer ones.
//
// A rejected push means the caller must drop the packet (and count it); the
// mailbox never queues an unbounded backlog to "fix" congestion.
class VideoMailbox {
public:
    VideoMailbox(int maxPackets, qint64 maxBytes, qint64 maxAgeUs = 0)
        : m_maxPackets(maxPackets), m_maxBytes(maxBytes), m_maxAgeUs(maxAgeUs)
    {
    }

    // Thread-unsafe (the owner serialises it). A packet larger than the whole
    // byte budget is rejected immediately. `pushSequence` is the publisher
    // queue's push order so the publisher can tell a pre-gap anchor from a
    // post-gap recovery key frame.
    bool push(VideoPacket packet, qint64 nowUs, quint64 pushSequence = 0)
    {
        if (m_closed) return false;
        const qint64 bytes = packet.data.size();
        if (bytes <= 0 || bytes > m_maxBytes) {
            ++m_rejected;
            return false;
        }
        if (m_queue.size() >= m_maxPackets || m_bytes + bytes > m_maxBytes) {
            ++m_rejected;
            return false;
        }
        m_bytes += bytes;
        m_queue.enqueue({std::move(packet), nowUs, pushSequence});
        return true;
    }

    // Moves every fresh packet to out in order. Entries older than the age
    // limit are discarded first so a stalled consumer cannot replay old video.
    int take(qint64 nowUs, QList<VideoPacket> &out)
    {
        int taken = 0;
        while (!m_queue.isEmpty()) {
            auto entry = m_queue.dequeue();
            m_bytes -= entry.packet.data.size();
            if (m_maxAgeUs > 0 && nowUs - entry.enqueuedUs > m_maxAgeUs) {
                ++m_expired;
                continue;
            }
            out.append(std::move(entry.packet));
            ++taken;
        }
        return taken;
    }

    bool takeOne(qint64 nowUs, VideoPacket &out, qint64 *enqueuedUs = nullptr,
                 quint64 *pushSequence = nullptr)
    {
        prune(nowUs);
        if (m_queue.isEmpty()) return false;
        auto entry = m_queue.dequeue();
        m_bytes -= entry.packet.data.size();
        out = std::move(entry.packet);
        if (enqueuedUs) *enqueuedUs = entry.enqueuedUs;
        if (pushSequence) *pushSequence = entry.pushSequence;
        return true;
    }

    // Counts entries that must be discarded because of age without taking the
    // fresh ones. Used by the owning drain right before it re-arms its wakeup.
    // `highestExpiredSequence` reports the newest discarded push order so the
    // publisher can place the loss point exactly there.
    void prune(qint64 nowUs, quint64 *highestExpiredSequence = nullptr)
    {
        if (m_maxAgeUs <= 0 || m_queue.isEmpty()) return;
        QQueue<QueuedVideo> fresh;
        while (!m_queue.isEmpty()) {
            auto entry = m_queue.dequeue();
            m_bytes -= entry.packet.data.size();
            if (nowUs - entry.enqueuedUs > m_maxAgeUs) {
                ++m_expired;
                if (highestExpiredSequence
                    && entry.pushSequence > *highestExpiredSequence) {
                    *highestExpiredSequence = entry.pushSequence;
                }
                continue;
            }
            m_bytes += entry.packet.data.size();
            fresh.enqueue(std::move(entry));
        }
        m_queue = std::move(fresh);
    }

    // Discards every queued packet that is not an independent key frame and
    // returns how many were removed. Used by the publisher queue to retract
    // dependents of a lost packet before they can reach the wire.
    int dropNonKeyFrames()
    {
        if (m_queue.isEmpty()) return 0;
        int dropped = 0;
        QQueue<QueuedVideo> fresh;
        while (!m_queue.isEmpty()) {
            auto entry = m_queue.dequeue();
            m_bytes -= entry.packet.data.size();
            if (!entry.packet.keyFrame()) {
                ++dropped;
                continue;
            }
            m_bytes += entry.packet.data.size();
            fresh.enqueue(std::move(entry));
        }
        m_queue = std::move(fresh);
        return dropped;
    }

    void close()
    {
        m_closed = true;
        clear();
    }

    void clear()
    {
        m_queue.clear();
        m_bytes = 0;
    }

    bool closed() const { return m_closed; }
    int count() const { return int(m_queue.size()); }
    qint64 bytes() const { return m_bytes; }
    quint64 rejected() const { return m_rejected; }
    quint64 expired() const { return m_expired; }
    int maxPackets() const { return m_maxPackets; }
    qint64 maxBytes() const { return m_maxBytes; }
    // True when a queued entry is an independent key frame. The publisher uses
    // it to decide whether a newer anchor already supersedes the packet that is
    // waiting for send budget.
    bool containsKeyFrame() const
    {
        for (const auto &entry : m_queue)
            if (entry.packet.keyFrame()) return true;
        return false;
    }

private:
    int m_maxPackets = 4;
    qint64 m_maxBytes = 4 * 1024 * 1024;
    qint64 m_maxAgeUs = 0;
    QQueue<QueuedVideo> m_queue;
    qint64 m_bytes = 0;
    quint64 m_rejected = 0;
    quint64 m_expired = 0;
    bool m_closed = false;
};

} // namespace vusiif::zto::desktop_share
