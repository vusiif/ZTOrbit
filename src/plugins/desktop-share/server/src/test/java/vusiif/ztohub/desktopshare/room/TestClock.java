package vusiif.ztohub.desktopshare.room;

/** Deterministic monotonic clock used by the isolated room-control tests. */
final class TestClock implements MonotonicClock {
    private long nanos = 1_000_000_000L;

    @Override
    public long nanoTime() {
        return nanos;
    }

    void advanceMillis(long millis) {
        nanos += millis * 1_000_000L;
    }

    void advanceNanos(long delta) {
        nanos += delta;
    }
}
