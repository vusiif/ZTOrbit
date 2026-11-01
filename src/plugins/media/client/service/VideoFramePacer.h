#pragma once

#include <QtGlobal>
#include <algorithm>

namespace vusiif::zto::media {
// Capture timestamps are monotonic microseconds. The initial and paused bucket
// bank at most two frames. Repeated/backwards timestamps neither refill
// nor spend credit, and never move the high-water mark backwards. A new capture
// lifetime must reset this object (including when its timestamp epoch changes).
class VideoFramePacer {
  public:
    void reset() { *this = VideoFramePacer{}; }

    bool admit(qint64 pts, int frameRate) {
        if (pts < 0 || (initialized && pts <= refillPts)) return false;
        if (!initialized) {
            initialized = true;
            credit = Capacity;
        } else {
            const auto rate = std::max(1, frameRate);
            const auto delta = pts - refillPts;
            const auto missing = Capacity - credit;
            // Saturate before multiplying: even a very long pause cannot
            // overflow, nor can it finance an unbounded catch-up burst.
            credit = delta >= (missing + rate - 1) / rate
                         ? Capacity : credit + delta * rate;
        }
        // Advance on rejected frames too: every elapsed microsecond is paid
        // into the bucket exactly once. Keep encoding time separate.
        refillPts = pts;
        if (credit < Unit) return false;
        credit -= Unit;
        encodedPts = pts;
        return true;
    }

    qint64 lastEncodedPts() const { return encodedPts; }

  private:
    static constexpr qint64 Unit = 1000000, Capacity = 2 * Unit;
    qint64 credit = 0, refillPts = 0, encodedPts = -1;
    bool initialized = false;
};
} // namespace vusiif::zto::media
