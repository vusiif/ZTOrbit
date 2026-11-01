#pragma once
#include <QtGlobal>
#include <algorithm>

namespace vusiif::zto::desktop_share {
// Observe traffic at every poll, not only the poll that crosses the idle limit.
// Last activity and counter-consumption cursors are independent state.
class MediaEndpointActivity {
  public:
    void reset(quint64 sent = 0, quint64 received = 0) {
        sentCursor = sent; receivedCursor = received; lastProofUs = 0; known = false;
    }
    void acknowledge(qint64 now) { lastProofUs = known ? std::max(lastProofUs, now) : now; known = true; }
    void observe(qint64 now, quint64 sent, quint64 received) {
        if (sent > sentCursor || received > receivedCursor) acknowledge(now);
        sentCursor = sent; receivedCursor = received;
    }
    bool expired(qint64 now, qint64 idleUs) const {
        return !known || (now >= lastProofUs && now - lastProofUs >= idleUs);
    }
    qint64 lastActivityUs() const { return lastProofUs; }
  private:
    quint64 sentCursor = 0, receivedCursor = 0;
    qint64 lastProofUs = 0;
    bool known = false;
};
}
