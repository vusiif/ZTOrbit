package vusiif.ztgui.server.api;

import java.net.InetSocketAddress;
import java.util.List;

/**
 * Entry point implemented by a ZTServer plugin JAR.
 *
 * <p>Threading and lifecycle contract:
 * <ul>
 *   <li>{@link #onStart}, {@link #onTcpPacket}, {@link #onUdpPacket},
 *       {@link #onConnectionClosed} and every query/send/network callback run on the single
 *       host event thread. Plugins must return quickly and must not block that thread.</li>
 *   <li>The optional {@link StartTask} returned by {@link #onStart} runs on a virtual/worker
 *       thread. Returning {@code null} means the plugin is immediately ready; a returned task
 *       makes it ready only after the task returns {@link StartResult#ok()}. A failed or throwing
 *       task keeps the plugin not ready.</li>
 *   <li>{@link #onStop} is called after pending tasks, queries, subscriptions and callbacks have
 *       been cancelled or awaited. Plugins must stop and join their own threads/timers.</li>
 *   <li>{@link HostServices#files()} and {@link HostServices#logs()} are thread-safe; the
 *       connection and network methods documented there follow their own notes.</li>
 * </ul>
 *
 * <p>The plugin jar is trusted code. The host does not sandbox plugins, isolate dependencies or
 * hot-reload jars; add or remove plugin jars and restart the server.
 */
public interface ServerPlugin {
    /** Identity of this plugin. Must match the plugin manifest exactly. */
    PluginInfo info();

    /**
     * Called once on the host event thread while the plugin is starting.
     *
     * @return a task to run in the background, or {@code null} when the plugin is immediately
     *         ready.
     */
    default StartTask onStart(HostServices host, CancellationToken cancellation) {
        return null;
    }

    /** Receives a ZTS1 payload routed to this plugin over TCP. */
    default void onTcpPacket(long connectionId, byte[] payload) {}

    /** Receives a ZTS1 payload routed to this plugin over UDP. */
    default void onUdpPacket(InetSocketAddress endpoint, byte[] payload) {}

    /** Called once for every TCP connection that previously sent this plugin a packet. */
    default void onConnectionClosed(long connectionId) {}

    /** Called once while the host is stopping, after all callbacks have been cancelled. */
    default void onStop() {}
}
