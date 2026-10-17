package vusiif.ztgui.server.api;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.List;
import java.util.concurrent.ScheduledExecutorService;

/**
 * Host services borrowed by a plugin.
 *
 * <p>Threading: {@code peerAddress}, {@code sendTcp}, {@code sendUdp}, {@code disconnect},
 * {@code queuedTcpBytes} and {@code scheduler()} are thread-safe. File and log services are
 * thread-safe as well. Every other method must be called from the host event thread, and
 * asynchronous callbacks always arrive on that thread. Calls made after {@code onStop} has
 * returned are ignored.
 */
public interface HostServices {
    /** Binary file service scoped to {@code plugins-data/<PluginInfo.name>/}. Thread-safe. */
    PluginFiles files();

    /** Log service writing to both the host log and {@code plugins-data/<name>/logs/}. Thread-safe. */
    PluginLogger logs();

    /** ZeroTier read-only queries. Event thread only. */
    ZeroTierQueries zeroTier();

    /**
     * Scheduler whose tasks run on the host event thread. Use it for plugin timers and cancel
     * every task in {@link ServerPlugin#onStop()}. Thread-safe registration.
     */
    ScheduledExecutorService scheduler();

    /** TCP/UDP server port the host is listening on. */
    int port();

    /**
     * Peer address of a TCP connection, or {@code null} when the connection no longer exists.
     * Thread-safe.
     */
    InetAddress peerAddress(long connectionId);

    /**
     * Sends one ZTS1 payload to a TCP connection using this plugin's route ID. Returns
     * {@code false} when the connection is gone, the payload exceeds the frame limit, the host is
     * stopping, or the send backlog would exceed 2 MiB. Thread-safe.
     */
    boolean sendTcp(long connectionId, byte[] payload);

    /**
     * Sends one ZTS1 UDP datagram using this plugin's route ID. Returns {@code false} when the
     * endpoint is invalid, the payload exceeds the UDP limit or the host is stopping.
     * Thread-safe.
     */
    boolean sendUdp(InetSocketAddress endpoint, byte[] payload);

    /** Closes a TCP connection. Thread-safe. */
    void disconnect(long connectionId);

    /**
     * Bytes still queued for a TCP connection on the event thread, or {@code -1} when the
     * connection does not exist. Thread-safe.
     */
    long queuedTcpBytes(long connectionId);

    /**
     * Sends data to one network member. The callback is invoked on the host event thread.
     * Cancelling the returned handle suppresses the callback. Event thread only.
     */
    Cancellable sendDataTo(String networkId, String memberId, byte[] data, SendCallback completed);

    /**
     * Sends data to several network members through one transport path. Event thread only.
     */
    Cancellable broadcastDataTo(
            String networkId, List<String> memberIds, byte[] data, SendCallback completed);

    /**
     * Sends data to several members, distributing them over the best available relays.
     * Event thread only.
     */
    Cancellable broadcastDataBalancedTo(
            String networkId, List<String> memberIds, byte[] data, SendCallback completed);

    /** Looks up network relays. Event thread only. */
    Cancellable getNetworkRelays(String networkId, RelayCallback completed);

    /**
     * Subscribes to authorized data received for a network. The callback runs on the host event
     * thread; cancel the subscription in {@link ServerPlugin#onStop()}. Event thread only.
     */
    NetworkSubscription subscribeNetworkData(String networkId, NetworkCallback received);
}
