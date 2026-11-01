package vusiif.ztohub.desktopshare.room;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import vusiif.ztohub.api.Cancellable;

/**
 * The narrow host surface used by {@link RoomController}. Everything runs on the host
 * event thread except scheduling, which the host documents as thread-safe.
 */
public interface HostGateway {
    int port();

    InetAddress peerAddress(long connectionId);

    boolean sendTcp(long connectionId, byte[] payload);

    boolean sendUdp(InetSocketAddress endpoint, byte[] payload);

    void disconnect(long connectionId);

    long queuedTcpBytes(long connectionId);

    /**
     * Schedules a periodic maintenance task on the host event thread. The returned
     * handle cancels it; {@link RoomController#stop()} always cancels exactly once.
     */
    Cancellable scheduleAtFixedRate(Runnable task, long periodMillis);

    void info(String message);

    void warn(String message);
}
