package vusiif.ztohub.desktopshare.room;

import java.net.InetAddress;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

/**
 * Failed room-join limiter keyed by TCP source address. It survives reconnect because
 * the key is the address, not the connection or session. Active entries are never
 * evicted to make room for another address: a full table rejects new sources instead
 * of letting them bypass the limit.
 */
final class JoinFailureLimiter {
    enum Decision {
        ALLOWED,
        BLOCKED,
        TABLE_FULL,
    }

    private static final class Entry {
        final ArrayDeque<Long> failures = new ArrayDeque<>();
        long lastTouchedNanos;

        boolean prune(long nowNanos, long windowNanos) {
            while (!failures.isEmpty() && nowNanos - failures.peekFirst() >= windowNanos) {
                failures.removeFirst();
            }
            return failures.isEmpty();
        }
    }

    private final long windowNanos;
    private final int limit;
    private final long idleNanos;
    private final int maxSources;
    private final Map<InetAddress, Entry> entries = new HashMap<>();

    JoinFailureLimiter(long windowNanos, int limit, long idleNanos, int maxSources) {
        this.windowNanos = windowNanos;
        this.limit = limit;
        this.idleNanos = idleNanos;
        this.maxSources = maxSources;
    }

    Decision check(InetAddress source, long nowNanos) {
        if (source == null) {
            return Decision.ALLOWED;
        }
        Entry entry = entries.get(source);
        if (entry != null) {
            entry.lastTouchedNanos = nowNanos;
            if (entry.prune(nowNanos, windowNanos)) {
                entries.remove(source);
                return Decision.ALLOWED;
            }
            return entry.failures.size() >= limit ? Decision.BLOCKED : Decision.ALLOWED;
        }
        if (entries.size() < maxSources) {
            return Decision.ALLOWED;
        }
        evictIdle(nowNanos);
        return entries.size() < maxSources ? Decision.ALLOWED : Decision.TABLE_FULL;
    }

    void recordFailure(InetAddress source, long nowNanos) {
        if (source == null) {
            return;
        }
        Entry entry = entries.get(source);
        if (entry == null) {
            if (entries.size() >= maxSources) {
                evictIdle(nowNanos);
            }
            if (entries.size() >= maxSources) {
                // The table is full of active or not-yet-idle sources. Dropping this
                // record is safer than evicting an active limit; the join still failed.
                return;
            }
            entry = new Entry();
            entries.put(source, entry);
        }
        entry.prune(nowNanos, windowNanos);
        entry.lastTouchedNanos = nowNanos;
        if (entry.failures.size() < limit) {
            entry.failures.addLast(nowNanos);
        }
    }

    void maintenance(long nowNanos) {
        evictIdle(nowNanos);
        for (Entry entry : new ArrayList<>(entries.values())) {
            entry.prune(nowNanos, windowNanos);
        }
    }

    int size() {
        return entries.size();
    }

    private void evictIdle(long nowNanos) {
        for (Map.Entry<InetAddress, Entry> item : new ArrayList<>(entries.entrySet())) {
            Entry entry = item.getValue();
            entry.prune(nowNanos, windowNanos);
            if (entry.failures.isEmpty() || nowNanos - entry.lastTouchedNanos >= idleNanos) {
                entries.remove(item.getKey());
            }
        }
    }
}
