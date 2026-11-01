package vusiif.ztohub.desktopshare.room;

/**
 * Per-session control-request token bucket. It refills at a fixed rate and allows a
 * bounded burst; a rejected request must not extend session activity.
 */
final class TokenBucket {
    private final double refillPerNano;
    private final double capacity;
    private double tokens;
    private long lastNanos;

    TokenBucket(int perSecond, int burst, long nowNanos) {
        this.refillPerNano = (double) perSecond / 1_000_000_000.0;
        this.capacity = burst;
        this.tokens = burst;
        this.lastNanos = nowNanos;
    }

    boolean take(long nowNanos) {
        if (nowNanos > lastNanos) {
            tokens = Math.min(capacity, tokens + (nowNanos - lastNanos) * refillPerNano);
            lastNanos = nowNanos;
        }
        if (tokens >= 1.0) {
            tokens -= 1.0;
            return true;
        }
        return false;
    }
}
