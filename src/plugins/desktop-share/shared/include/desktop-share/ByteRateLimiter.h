#pragma once

#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace vusiif::zto::desktop_share {

// Byte-denominated token bucket used for the publisher's wire pacing. The
// bucket refills at a fixed rate and allows one bounded burst; take() never
// sleeps or queues, it only reports how long the caller must wait before the
// datagram fits again.
class ByteRateLimiter {
public:
    void reset(double bytesPerSecond, qint64 burstBytes, qint64 nowUs)
    {
        m_ratePerUs = bytesPerSecond > 0 ? bytesPerSecond / 1'000'000.0 : 0;
        m_capacity = burstBytes > 0 ? burstBytes : 0;
        m_tokens = double(m_capacity);
        m_lastUs = nowUs;
    }

    bool enabled() const { return m_ratePerUs > 0 && m_capacity > 0; }

    // Burst size: no single admission can reserve more than this, and it is the
    // upper bound for how long an admission can have to wait for tokens.
    qint64 capacity() const { return qint64(m_capacity); }

    // Returns tokens for bytes that were reserved but never reached the wire
    // (an aborted packet or a failed datagram). Never exceeds the burst.
    void refund(qint64 bytes, qint64 nowUs)
    {
        if (bytes <= 0 || !enabled()) return;
        refill(nowUs);
        m_tokens = std::min(double(m_capacity), m_tokens + double(bytes));
    }

    // Returns 0 after consuming the tokens, or the microseconds the caller must
    // wait before this datagram fits. The bucket is unchanged on a wait.
    qint64 take(qint64 bytes, qint64 nowUs)
    {
        if (bytes <= 0 || !enabled()) return 0;
        if (bytes > m_capacity) {
            // Never admit a datagram larger than the whole burst; callers use
            // wire-sized datagrams, so this is a defensive bound.
            return 1'000'000;
        }
        refill(nowUs);
        if (m_tokens + 1e-9 >= double(bytes)) {
            m_tokens -= double(bytes);
            return 0;
        }
        const double missing = double(bytes) - m_tokens;
        const auto waitUs = qint64(std::ceil(missing / m_ratePerUs));
        return std::max<qint64>(1, waitUs);
    }

    qint64 available(qint64 nowUs)
    {
        refill(nowUs);
        return qint64(m_tokens);
    }

private:
    void refill(qint64 nowUs)
    {
        if (!enabled() || nowUs <= m_lastUs) {
            if (nowUs > m_lastUs) m_lastUs = nowUs;
            return;
        }
        m_tokens = std::min(double(m_capacity), m_tokens + (nowUs - m_lastUs) * m_ratePerUs);
        m_lastUs = nowUs;
    }

    double m_ratePerUs = 0;
    double m_capacity = 0;
    double m_tokens = 0;
    qint64 m_lastUs = 0;
};

} // namespace vusiif::zto::desktop_share
