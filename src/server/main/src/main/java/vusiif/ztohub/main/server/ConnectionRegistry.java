package vusiif.ztohub.main.server;

import io.netty.channel.Channel;
import java.util.Collection;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/** Event-thread connection registry with a hard limit of 256 simultaneous TCP connections. */
public final class ConnectionRegistry {
    public static final int MAX_CONNECTIONS = 256;
    private final AtomicLong nextId = new AtomicLong(1);
    private final Map<Long, Connection> connections = new ConcurrentHashMap<>();

    public Connection add(Channel channel, long now) {
        if (connections.size() >= MAX_CONNECTIONS) {
            return null;
        }
        long id = nextId.getAndIncrement();
        if (id <= 0) {
            // Wraparound guard: connection IDs are always positive.
            nextId.set(2);
            id = 1;
        }
        Connection connection = new Connection(id, channel, now);
        connections.put(id, connection);
        return connection;
    }

    public Connection get(long id) {
        return connections.get(id);
    }

    public Connection remove(long id) {
        return connections.remove(id);
    }

    public Collection<Connection> all() {
        return connections.values();
    }

    public int size() {
        return connections.size();
    }

    public boolean isEmpty() {
        return connections.isEmpty();
    }
}
