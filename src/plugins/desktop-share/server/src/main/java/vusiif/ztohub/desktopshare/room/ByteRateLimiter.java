package vusiif.ztohub.desktopshare.room;

/**
 * Byte-denominated token bucket for one monotonic clock. It is used for the room's
 * publisher ingress limit and for the server-wide egress limit.
 *
 * <p>A rejection never queues or waits for tokens: the caller drops the datagram. The
 * bucket is confined to the host event thread, so it is not synchronised.
 */
final class ByteRateLimiter {
    private double refillPerNano;
    private double capacity;
    private double tokens;
    private long lastNanos;

    ByteRateLimiter(long bytesPerSecond, long burstBytes, long nowNanos) {
        if (bytesPerSecond <= 0 || burstBytes <= 0) {
            throw new IllegalArgumentException("rate and burst must be positive");
        }
        this.refillPerNano = (double) bytesPerSecond / 1_000_000_000.0;
        this.capacity = burstBytes;
        this.tokens = burstBytes;
        this.lastNanos = nowNanos;
    }

    /** Replaces the rate for a new profile and starts a fresh burst window. */
    void reset(long bytesPerSecond, long burstBytes, long nowNanos) {
        if (bytesPerSecond <= 0 || burstBytes <= 0) {
            throw new IllegalArgumentException("rate and burst must be positive");
        }
        this.refillPerNano = (double) bytesPerSecond / 1_000_000_000.0;
        this.capacity = burstBytes;
        this.tokens = burstBytes;
        this.lastNanos = nowNanos;
    }

    /** Returns true and consumes the tokens, or false without changing the bucket. */
    boolean take(long bytes, long nowNanos) {
        if (bytes <= 0 || bytes > capacity) {
            return false;
        }
        refill(nowNanos);
        if (tokens + 1e-9 < bytes) {
            return false;
        }
        tokens -= bytes;
        return true;
    }

    /** Returns tokens for a datagram that was admitted but not actually sent. */
    void refund(long bytes, long nowNanos) {
        if (bytes <= 0) {
            return;
        }
        refill(nowNanos);
        tokens = Math.min(capacity, tokens + bytes);
    }

    /** Tokens currently available, for diagnostics only. */
    long available(long nowNanos) {
        refill(nowNanos);
        return (long) tokens;
    }

    private void refill(long nowNanos) {
        if (nowNanos <= lastNanos) {
            return;
        }
        tokens = Math.min(capacity, tokens + (nowNanos - lastNanos) * refillPerNano);
        lastNanos = nowNanos;
    }
}
