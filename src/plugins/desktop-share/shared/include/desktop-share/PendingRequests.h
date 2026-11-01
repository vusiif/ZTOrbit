#pragma once

#include <QHash>
#include <QtGlobal>

namespace vusiif::zto::desktop_share {

// Tracks at most 16 outstanding control requests. A response is accepted only
// while its request is still live; a response that arrives at or after the
// five-second deadline is treated as an old response and cannot change state.
class PendingRequests {
public:
    static constexpr int MaxOutstanding = 16;
    static constexpr qint64 TimeoutMs = 5000;

    void clear() { m_deadlines.clear(); }

    bool add(quint32 id, qint64 nowMs)
    {
        if (id == 0 || m_deadlines.size() >= MaxOutstanding) return false;
        if (m_deadlines.contains(id)) return false;
        m_deadlines.insert(id, nowMs + TimeoutMs);
        return true;
    }

    bool complete(quint32 id, qint64 nowMs)
    {
        const auto it = m_deadlines.find(id);
        if (it == m_deadlines.end()) return false;
        const bool live = nowMs < it.value();
        m_deadlines.erase(it);
        return live;
    }

    bool contains(quint32 id, qint64 nowMs) const
    {
        const auto it = m_deadlines.constFind(id);
        return it != m_deadlines.constEnd() && nowMs < it.value();
    }

    int expire(qint64 nowMs)
    {
        int removed = 0;
        for (auto it = m_deadlines.begin(); it != m_deadlines.end();) {
            if (nowMs >= it.value()) {
                it = m_deadlines.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    int size() const { return int(m_deadlines.size()); }

private:
    QHash<quint32, qint64> m_deadlines;
};

} // namespace vusiif::zto::desktop_share
