#pragma once

#include <QtGlobal>

namespace vusiif::zto::desktop_share {

// Lifecycle of one publisher packet's wire-budget reservation.
//
// The pacer grants a whole-packet reservation before the first fragment is
// written. From that moment the reservation is "held"; every successfully
// written datagram consumes part of it. When the packet ends, only the part that
// was granted and never reached the socket may be refunded:
//
//   none ──reserve(cost)──> held ──written(n)──> held (partially written)
//                              │                   │
//                              ├──release()────────┤ abort / timeout / encode
//                              │   refund = cost - written   or socket failure
//                              ├──complete()───────┤ all fragments written
//                              └──release()────────┘ clear / stop
//
// A packet that was never admitted (rejected for exceeding the burst, or timed
// out before `reserve`) reports zero refundable bytes, so dropping it can never
// mint pacing tokens. `release()` is idempotent: it clears the state, so a
// duplicate abort cannot refund twice, and `reserve()` refuses to start a second
// reservation on top of an active one.
class SendReservation {
public:
    // Records that the pacer granted `cost` wire bytes for the active packet.
    // Returns false when a reservation is already held; the caller must finish
    // the previous packet first.
    bool reserve(qint64 cost)
    {
        if (m_reserved > 0) return false;
        if (cost < 0) return false;
        m_reserved = cost;
        m_written = 0;
        return true;
    }

    // Records datagrams accepted by the socket. Never accumulates beyond the
    // reservation, so a repeated callback cannot refund negative bytes.
    void written(qint64 bytes)
    {
        if (bytes <= 0 || m_reserved <= 0) return;
        m_written = qMin(m_reserved, m_written + bytes);
    }

    // Ends the active packet and returns the reserved-but-unwritten bytes. Safe
    // on an idle reservation (returns 0) and idempotent.
    qint64 release()
    {
        const qint64 refund = refundable();
        m_reserved = 0;
        m_written = 0;
        return refund;
    }

    // Ends a packet whose every fragment was written; nothing is refunded.
    void complete()
    {
        m_reserved = 0;
        m_written = 0;
    }

    bool active() const { return m_reserved > 0; }
    qint64 reserved() const { return m_reserved; }
    qint64 writtenBytes() const { return m_written; }
    qint64 refundable() const
    {
        const qint64 remaining = m_reserved - m_written;
        return remaining > 0 ? remaining : 0;
    }

private:
    qint64 m_reserved = 0;
    qint64 m_written = 0;
};

} // namespace vusiif::zto::desktop_share
