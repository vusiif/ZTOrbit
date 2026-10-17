package vusiif.ztgui.server.main.server;

import io.netty.channel.Channel;
import java.util.LinkedHashSet;
import java.util.Set;

/** Per-connection state. All mutable fields are accessed on the host event thread. */
public final class Connection {
    private final long id;
    private final Channel channel;
    private final Set<String> routes = new LinkedHashSet<>();
    private int requests;
    private long windowStart;
    private long lastSeen;

    Connection(long id, Channel channel, long now) {
        this.id = id;
        this.channel = channel;
        this.windowStart = now;
        this.lastSeen = now;
    }

    public long id() {
        return id;
    }

    public Channel channel() {
        return channel;
    }

    public Set<String> routes() {
        return routes;
    }

    public int requests() {
        return requests;
    }

    public void incrementRequests() {
        requests++;
    }

    public void resetWindow(long now) {
        requests = 0;
        windowStart = now;
    }

    public long windowStart() {
        return windowStart;
    }

    public long lastSeen() {
        return lastSeen;
    }

    public void touch(long now) {
        lastSeen = now;
    }
}
