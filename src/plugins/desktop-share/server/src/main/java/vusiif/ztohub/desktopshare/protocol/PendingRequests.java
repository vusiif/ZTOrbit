package vusiif.ztohub.desktopshare.protocol;

import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

/**
 * Tracks at most 16 outstanding control requests. A response is accepted only
 * while its request is still live; a response that arrives at or after the
 * five-second deadline is treated as an old response and cannot change state.
 */
public final class PendingRequests {
    public static final int MAX_OUTSTANDING = 16;
    public static final long TIMEOUT_MS = 5000;

    private final Map<Long, Long> deadlines = new HashMap<>();

    public void clear() {
        deadlines.clear();
    }

    public boolean add(long id, long nowMs) {
        if (id <= 0 || id > ControlProtocol.MAX_REQUEST_ID) return false;
        if (deadlines.size() >= MAX_OUTSTANDING) return false;
        if (deadlines.containsKey(id)) return false;
        deadlines.put(id, nowMs + TIMEOUT_MS);
        return true;
    }

    public boolean complete(long id, long nowMs) {
        Long deadline = deadlines.remove(id);
        if (deadline == null) return false;
        return nowMs < deadline;
    }

    public boolean contains(long id, long nowMs) {
        Long deadline = deadlines.get(id);
        return deadline != null && nowMs < deadline;
    }

    public int expire(long nowMs) {
        int removed = 0;
        for (Iterator<Map.Entry<Long, Long>> iterator = deadlines.entrySet().iterator();
                iterator.hasNext(); ) {
            if (nowMs >= iterator.next().getValue()) {
                iterator.remove();
                ++removed;
            }
        }
        return removed;
    }

    public int size() {
        return deadlines.size();
    }
}
