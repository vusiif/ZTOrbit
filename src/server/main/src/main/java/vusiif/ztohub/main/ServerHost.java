package vusiif.ztohub.main;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import io.netty.buffer.Unpooled;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFuture;
import io.netty.channel.ChannelOutboundBuffer;
import io.netty.channel.EventLoop;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.nio.NioSocketChannel;
import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.concurrent.Callable;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import vusiif.ztohub.api.ServerPlugin;
import vusiif.ztohub.main.network.NetworkTransport;
import vusiif.ztohub.main.plugin.PluginContainer;
import vusiif.ztohub.main.plugin.PluginManager;
import vusiif.ztohub.main.server.BuiltinHandler;
import vusiif.ztohub.main.server.Connection;
import vusiif.ztohub.main.server.ConnectionRegistry;
import vusiif.ztohub.main.server.TcpServer;
import vusiif.ztohub.main.server.UdpSendBudget;
import vusiif.ztohub.main.server.UdpSender;
import vusiif.ztohub.main.server.UdpServer;
import vusiif.ztohub.main.server.Wire;
import vusiif.ztohub.main.zerotier.ZeroTierClient;

/**
 * Main server host: Netty TCP/UDP listeners, the single host event thread, plugin lifecycle,
 * ZeroTier queries and the network transport. All mutable server state is confined to the event
 * thread; public methods that plugin code may call from other threads marshal internally.
 */
public final class ServerHost implements AutoCloseable {
    private static final long IDLE_TIMEOUT_MS = 30_000;
    private static final long NETWORK_POLL_INTERVAL_MS = 2_000;

    private final Path workingDirectory;
    private final InetAddress listenAddress;
    private final int port;
    private final EventLoopGroup group;
    private final EventLoop eventLoop;
    private final HostLog log;
    private final ConnectionRegistry registry = new ConnectionRegistry();
    private final ZeroTierClient zeroTier;
    private final PluginManager plugins;
    private final NetworkTransport transport;
    private final BuiltinHandler builtin;
    private final UdpSendBudget udpBudget = new UdpSendBudget();
    private final UdpSender udpSender = new UdpSender(udpBudget);
    private final AtomicBoolean stopping = new AtomicBoolean();
    private Channel tcpChannel;
    private Channel udpChannel;
    private ScheduledFuture<?> maintenance;
    private ScheduledFuture<?> networkPoll;
    private long lastNetworkPoll;
    private boolean networkPollPending;
    private long lastUdpBudgetLog;
    private String nodeId = "";

    public ServerHost(Path workingDirectory, InetAddress listenAddress, int port, HostLog log) {
        this.workingDirectory = workingDirectory;
        this.listenAddress = listenAddress;
        this.port = port;
        this.log = log;
        this.group = new NioEventLoopGroup(1,
                (java.util.concurrent.ThreadFactory) runnable -> new Thread(runnable, "zt-host"));
        this.eventLoop = group.next();
        this.zeroTier = new ZeroTierClient(eventLoop, log);
        this.plugins = new PluginManager(this);
        this.transport = new NetworkTransport(true, eventLoop, log, workingDirectory);
        this.builtin = new BuiltinHandler(this);
    }

    public void start() throws IOException {
        loadRelayConfig();
        try {
            tcpChannel = TcpServer.bind(group, listenAddress, port, this);
        } catch (Throwable failure) {
            throw new IOException("TCP bind failed: " + describe(failure), failure);
        }
        try {
            udpChannel = UdpServer.bind(group, listenAddress, port, this);
        } catch (Throwable failure) {
            closeQuietly(tcpChannel);
            tcpChannel = null;
            throw new IOException("UDP bind failed: " + describe(failure), failure);
        }
        try {
            eventLoop.submit(() -> {
                plugins.scanAndStart();
                return null;
            }).sync();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new IOException("Interrupted while loading plugins", interrupted);
        } catch (Exception failure) {
            throw new IOException("Plugin scan failed: " + describe(failure), failure);
        }
        maintenance = eventLoop.scheduleAtFixedRate(
                this::maintenanceTick, 1, 1, TimeUnit.SECONDS);
        networkPoll = eventLoop.scheduleAtFixedRate(
                this::networkPollTick, 2, 2, TimeUnit.SECONDS);
        log.info("ztohub listening " + listenAddress.getHostAddress() + " TCP/UDP " + port);
    }

    private void loadRelayConfig() {
        Path file = workingDirectory.resolve("relays.json");
        if (!Files.isRegularFile(file)) {
            return;
        }
        try {
            byte[] bytes = Files.readAllBytes(file);
            if (bytes.length > 65536) {
                log.warn("relays.json exceeds 64 KiB; relay configuration ignored");
                return;
            }
            JsonNode config = JsonSupport.parse(bytes);
            if (config == null || !config.isObject()) {
                log.warn("Invalid relays.json; relay configuration ignored");
                return;
            }
            transport.setRelayConfig(config);
        } catch (IOException invalid) {
            log.warn("Invalid relays.json; relay configuration ignored");
        }
    }

    private void maintenanceTick() {
        long now = now();
        for (Connection connection : new ArrayList<>(registry.all())) {
            if (now - connection.lastSeen() > IDLE_TIMEOUT_MS) {
                disconnect(connection.id());
            }
        }
        if (now - lastUdpBudgetLog >= 10_000) {
            lastUdpBudgetLog = now;
            UdpSendBudget.Snapshot snapshot = udpBudget.snapshot();
            if (snapshot.accepted() > 0 || snapshot.outstanding() > 0) {
                log.info("UDP 在途预算 inflightBytes=" + snapshot.inflightBytes()
                        + " inflightPackets=" + snapshot.inflightPackets()
                        + " peakBytes=" + snapshot.peakBytes()
                        + " peakPackets=" + snapshot.peakPackets()
                        + " accepted=" + snapshot.accepted()
                        + " rejected=" + snapshot.rejected());
            }
        }
    }

    private void networkPollTick() {
        long now = now();
        if (stopping.get() || networkPollPending || now - lastNetworkPoll < NETWORK_POLL_INTERVAL_MS) {
            return;
        }
        lastNetworkPoll = now;
        networkPollPending = true;
        zeroTier.query("info", JsonSupport.object(), info -> {
            if (!info.ok()) {
                networkPollPending = false;
                return;
            }
            nodeId = info.document().path("address").asText("");
            zeroTier.query("networks", JsonSupport.object(), networks -> {
                networkPollPending = false;
                if (networks.ok() && networks.document().isArray()) {
                    transport.setNetworks(nodeId, networks.document());
                }
            });
        });
    }

    /** Handles one decoded TCP packet on the host event thread. */
    public void handlePacket(Connection connection, Wire.Packet packet) {
        if (stopping.get()) {
            return;
        }
        long now = now();
        if (now - connection.windowStart() >= 1000) {
            connection.resetWindow(now);
        }
        connection.incrementRequests();
        if (connection.requests() > 60) {
            disconnect(connection.id());
            return;
        }
        connection.touch(now);
        String route = packet.route();
        if (Wire.BUILTIN.equals(route)) {
            builtin.handle(connection, packet.payload());
            return;
        }
        ServerPlugin plugin = plugins.readyPlugin(route);
        if (plugin == null) {
            hostLogUnknownRoute(connection, route);
            return;
        }
        connection.routes().add(route);
        try {
            plugin.onTcpPacket(connection.id(), packet.payload());
        } catch (Throwable failure) {
            log.warn("Plugin packet callback failed " + route + ": " + describe(failure));
            disconnect(connection.id());
        }
    }

    private void hostLogUnknownRoute(Connection connection, String route) {
        ObjectNode response = JsonSupport.object();
        response.put("v", 1);
        response.put("ok", false);
        response.put("error", "unknown_or_unready_plugin");
        response.put("plugin", route);
        sendTcp(Wire.BUILTIN, connection.id(), JsonSupport.compact(response));
    }

    /** Handles one datagram on the host event thread. */
    public void handleDatagram(InetSocketAddress sender, byte[] bytes) {
        if (stopping.get() || bytes.length > Wire.MAX_UDP_BYTES) {
            return;
        }
        Wire.Packet packet = Wire.decode(bytes);
        if (packet == null) {
            return;
        }
        if (Wire.BUILTIN.equals(packet.route())) {
            transport.datagram(sender.getAddress(), sender.getPort(), packet.payload());
            return;
        }
        ServerPlugin plugin = plugins.readyPlugin(packet.route());
        if (plugin == null) {
            return;
        }
        try {
            plugin.onUdpPacket(sender, packet.payload());
        } catch (Throwable failure) {
            log.warn("Plugin UDP callback failed " + packet.route() + ": " + describe(failure));
        }
    }

    /** Called after a connection was removed from the registry. */
    public void handleClosed(Connection connection) {
        builtin.cancelConnection(connection.id());
        for (String route : new ArrayList<>(connection.routes())) {
            PluginContainer container = plugins.findById(route);
            if (container == null) {
                continue;
            }
            try {
                container.plugin().onConnectionClosed(connection.id());
            } catch (Throwable failure) {
                log.warn("Plugin connection callback failed " + route + ": " + describe(failure));
            }
        }
    }

    public InetAddress peerAddress(long connectionId) {
        Connection connection = registry.get(connectionId);
        if (connection == null) {
            return null;
        }
        if (connection.channel().remoteAddress() instanceof InetSocketAddress remote) {
            return remote.getAddress();
        }
        return null;
    }

    public boolean sendTcp(String route, long connectionId, byte[] payload) {
        if (stopping.get()) {
            return false;
        }
        return Boolean.TRUE.equals(onLoop(() -> sendTcpOnLoop(route, connectionId, payload), Boolean.FALSE));
    }

    private boolean sendTcpOnLoop(String route, long connectionId, byte[] payload) {
        Connection connection = registry.get(connectionId);
        if (connection == null) {
            return false;
        }
        byte[] framed = Wire.frame(route, payload);
        if (framed == null) {
            return false;
        }
        Channel channel = connection.channel();
        long queued = 0;
        if (channel instanceof NioSocketChannel socket) {
            ChannelOutboundBuffer buffer = socket.unsafe().outboundBuffer();
            if (buffer != null) {
                queued = buffer.totalPendingWriteBytes();
            }
        }
        if (queued + framed.length > 2L * Wire.MAX_TCP_BYTES) {
            eventLoop.execute(() -> disconnect(connectionId));
            return false;
        }
        ChannelFuture future = channel.writeAndFlush(Unpooled.wrappedBuffer(framed));
        return future.channel().isActive();
    }

    public boolean sendUdp(String route, InetSocketAddress endpoint, byte[] payload) {
        if (stopping.get() || endpoint == null || endpoint.getAddress() == null || endpoint.getPort() < 1) {
            return false;
        }
        byte[] packet = Wire.encode(route, payload);
        if (packet == null || packet.length > Wire.MAX_UDP_BYTES) {
            return false;
        }
        Channel channel = udpChannel;
        if (channel == null || !channel.isActive()) {
            return false;
        }
        // The budget is reserved here, before any cross-thread event-loop task exists.
        // A true result means the datagram was accepted for transmission, not sent.
        return udpSender.send(channel, endpoint, packet);
    }

    /** Global UDP in-flight counters. Contains no endpoint, payload or plugin identity. */
    public UdpSendBudget.Snapshot udpBudgetSnapshot() {
        return udpBudget.snapshot();
    }

    public void disconnect(long connectionId) {
        if (stopping.get()) {
            return;
        }
        onLoop(() -> {
            Connection connection = registry.get(connectionId);
            if (connection != null) {
                connection.channel().close();
            }
            return null;
        }, null);
    }

    public long queuedTcpBytes(long connectionId) {
        Long queued = onLoop(() -> {
            Connection connection = registry.get(connectionId);
            if (connection == null) {
                return -1L;
            }
            if (connection.channel() instanceof NioSocketChannel socket) {
                ChannelOutboundBuffer buffer = socket.unsafe().outboundBuffer();
                return buffer == null ? 0L : buffer.totalPendingWriteBytes();
            }
            return 0L;
        }, -1L);
        return queued == null ? -1 : queued;
    }

    private <T> T onLoop(Callable<T> task, T fallback) {
        if (eventLoop.inEventLoop()) {
            try {
                return task.call();
            } catch (Exception failure) {
                return fallback;
            }
        }
        if (eventLoop.isShutdown() || eventLoop.isShuttingDown()) {
            return fallback;
        }
        try {
            return eventLoop.submit(task).get(5, TimeUnit.SECONDS);
        } catch (Exception failure) {
            return fallback;
        }
    }

    public void stop() {
        if (eventLoop.inEventLoop()) {
            stopInternal();
            return;
        }
        if (eventLoop.isShutdown() || eventLoop.isShuttingDown()) {
            return;
        }
        try {
            eventLoop.submit(this::stopInternal).get(40, TimeUnit.SECONDS);
        } catch (Exception failure) {
            log.warn("Server stop did not finish cleanly: " + describe(failure));
        }
    }

    private void stopInternal() {
        if (!stopping.compareAndSet(false, true)) {
            return;
        }
        // Reject new reservations immediately; writes accepted earlier either complete
        // or have their budget force-released after the UDP channel is closed.
        udpBudget.beginStop();
        if (maintenance != null) {
            maintenance.cancel(false);
        }
        if (networkPoll != null) {
            networkPoll.cancel(false);
        }
        closeQuietly(tcpChannel);
        tcpChannel = null;
        closeQuietly(udpChannel);
        udpChannel = null;
        zeroTier.close();
        plugins.beginStop();
        for (Connection connection : new ArrayList<>(registry.all())) {
            if (registry.remove(connection.id()) != null) {
                handleClosed(connection);
            }
            closeQuietly(connection.channel());
        }
        plugins.finishStop();
        transport.close();
        int releasedAtStop = udpBudget.forceReleaseAll();
        UdpSendBudget.Snapshot snapshot = udpBudget.snapshot();
        log.info("ztohub stopped; UDP 在途预算 inflightBytes=" + snapshot.inflightBytes()
                + " inflightPackets=" + snapshot.inflightPackets()
                + " peakBytes=" + snapshot.peakBytes()
                + " peakPackets=" + snapshot.peakPackets()
                + " accepted=" + snapshot.accepted()
                + " rejected=" + snapshot.rejected()
                + " releasedAtStop=" + releasedAtStop);
    }

    @Override
    public void close() {
        stop();
        if (!group.isShuttingDown()) {
            group.shutdownGracefully(0, 5, TimeUnit.SECONDS).syncUninterruptibly();
        }
        // A task cancelled by the final event-loop shutdown must not leave budget behind.
        udpBudget.forceReleaseAll();
    }

    private static void closeQuietly(Channel channel) {
        if (channel != null) {
            try {
                channel.close();
            } catch (RuntimeException ignored) {
                // The channel is already gone.
            }
        }
    }

    private static String describe(Throwable failure) {
        String message = failure.getMessage();
        return message == null || message.isBlank() ? failure.getClass().getSimpleName() : message;
    }

    public EventLoop eventLoop() {
        return eventLoop;
    }

    public HostLog log() {
        return log;
    }

    public Path workingDirectory() {
        return workingDirectory;
    }

    public int port() {
        return port;
    }

    public InetAddress listenAddress() {
        return listenAddress;
    }

    public ConnectionRegistry registry() {
        return registry;
    }

    public PluginManager plugins() {
        return plugins;
    }

    public NetworkTransport transport() {
        return transport;
    }

    public vusiif.ztohub.api.ZeroTierQueries zeroTier() {
        return zeroTier;
    }

    public long now() {
        return System.currentTimeMillis();
    }
}
