package vusiif.ztgui.server.main.network;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import io.netty.bootstrap.Bootstrap;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFuture;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.ChannelOption;
import io.netty.channel.EventLoop;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.SimpleChannelInboundHandler;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.DatagramPacket;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioDatagramChannel;
import io.netty.channel.socket.nio.NioSocketChannel;
import java.io.IOException;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Base64;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.regex.Pattern;
import vusiif.ztgui.server.api.Cancellable;
import vusiif.ztgui.server.api.NetworkCallback;
import vusiif.ztgui.server.api.NetworkMessage;
import vusiif.ztgui.server.api.NetworkSubscription;
import vusiif.ztgui.server.api.RelayInfo;
import vusiif.ztgui.server.api.RelayResult;
import vusiif.ztgui.server.api.SendResult;
import vusiif.ztgui.server.main.HostLog;
import vusiif.ztgui.server.main.JsonSupport;
import vusiif.ztgui.server.main.server.Wire;
import vusiif.ztgui.server.main.server.ZtsFrameDecoder;

/**
 * Java port of {@code src/shared/network/NetworkTransport.cpp}.
 *
 * <p>Runs on its own single-threaded Netty event loop. Each network owns a UDP socket bound to the
 * node's ZeroTier IPv4 address. RPC between servers uses the regular ZTS1 {@code builtin} TCP
 * protocol. The host event loop receives only the final plugin-facing callbacks.
 */
public final class NetworkTransport implements AutoCloseable {
    private static final int NETWORK_PORT = 18100;
    private static final int CLIENT_NETWORK_PORT = 18101;
    private static final int MAX_UDP_BYTES = 1232;
    private static final int MAX_TCP_BYTES = 1024 * 1024;
    private static final int MAX_PENDING_RPC = 128;
    private static final int MAX_SEEN = 8192;
    private static final int MAX_TARGETS = 4096;
    private static final long SEEN_TTL_MS = 30_000;
    private static final long MEMBER_TTL_MS = 30_000;
    private static final long ENDPOINT_TTL_MS = 10_000;
    private static final long BINDING_TTL_MS = 30_000;
    private static final long PROBE_FRESH_MS = 10_000;
    private static final long PROBE_TIMEOUT_MS = 1_000;
    private static final int DEFAULT_RPC_DEADLINE_MS = 1500;
    private static final int REFRESH_DEADLINE_MS = 20_000;
    private static final Pattern NETWORK_ID = Pattern.compile("^[0-9a-fA-F]{16}$");
    private static final Pattern MEMBER_ID = Pattern.compile("^[0-9a-fA-F]{10}$");

    private final boolean server;
    private final EventLoop hostLoop;
    private final HostLog log;
    private final Path workingDirectory;
    private final EventLoopGroup group;
    private final EventLoop loop;
    private final Map<String, NetworkState> networks = new LinkedHashMap<>();
    private final Map<String, Long> seen = new HashMap<>();
    private final Map<String, Probe> probes = new HashMap<>();
    private final Set<Subscription> subscriptions = ConcurrentHashMap.newKeySet();
    private final AtomicBoolean closing = new AtomicBoolean();
    private JsonNode relayConfig = JsonSupport.object();
    private long forwardedBytes;
    private long rateStart;
    private int pendingRpc;
    private ScheduledFuture<?> maintenance;

    public NetworkTransport(boolean server, EventLoop hostLoop, HostLog log, Path workingDirectory) {
        this.server = server;
        this.hostLoop = hostLoop;
        this.log = log;
        this.workingDirectory = workingDirectory;
        this.group = new NioEventLoopGroup(1,
                (java.util.concurrent.ThreadFactory) runnable -> new Thread(runnable, "zt-transport"));
        this.loop = group.next();
        this.rateStart = System.currentTimeMillis();
        try {
            loop.execute(this::startOnLoop);
        } catch (RejectedExecutionException ignored) {
            // Construction never races shutdown in practice.
        }
    }

    private void startOnLoop() {
        maintenance = loop.scheduleAtFixedRate(this::tick, 2, 2, TimeUnit.SECONDS);
    }

    // ------------------------------------------------------------------ public API

    public void setRelayConfig(JsonNode config) {
        if (closing.get()) {
            return;
        }
        JsonNode copy = config == null ? JsonSupport.object() : config.deepCopy();
        execute(() -> relayConfig = copy);
    }

    public void setNetworks(String nodeId, JsonNode descriptors) {
        if (closing.get()) {
            return;
        }
        JsonNode copy = descriptors == null ? JsonSupport.mapper().createArrayNode() : descriptors.deepCopy();
        execute(() -> configure(nodeId == null ? "" : nodeId, copy));
    }

    public void datagram(InetAddress source, int port, byte[] payload) {
        if (closing.get()) {
            return;
        }
        execute(() -> datagramOnLoop(source, port, payload));
    }

    public Cancellable builtin(InetAddress source, JsonNode request, Consumer<ObjectNode> completed) {
        BuiltinJob job = new BuiltinJob(completed);
        if (closing.get()) {
            return job;
        }
        JsonNode copy = request == null ? JsonSupport.object() : request.deepCopy();
        execute(() -> builtinOnLoop(source, copy, job));
        return job;
    }

    public Cancellable send(
            String plugin,
            String network,
            List<String> members,
            byte[] data,
            boolean balanced,
            Consumer<SendResult> completed) {
        SendJob job = new SendJob(completed);
        if (closing.get()) {
            job.complete(SendResult.failure("network_unavailable", List.of()));
            return job;
        }
        List<String> copy = members == null ? List.of() : List.copyOf(members);
        byte[] payload = data == null ? new byte[0] : data.clone();
        String route = plugin == null ? "" : plugin;
        String id = network == null ? "" : network;
        execute(() -> sendOnLoop(job, route, id, copy, payload, balanced));
        return job;
    }

    public Cancellable relays(String network, Consumer<RelayResult> completed) {
        RelayJob job = new RelayJob(completed);
        if (closing.get()) {
            job.complete(RelayResult.failure("network_unavailable"));
            return job;
        }
        String id = network == null ? "" : network;
        execute(() -> relaysOnLoop(job, id));
        return job;
    }

    public NetworkSubscription subscribe(String plugin, String network, NetworkCallback callback) {
        Subscription subscription = new Subscription(plugin, network, callback);
        if (callback == null || !validNetworkId(network) || closing.get()) {
            subscription.cancel();
            return subscription;
        }
        subscriptions.add(subscription);
        return subscription;
    }

    @Override
    public void close() {
        if (!closing.compareAndSet(false, true)) {
            return;
        }
        try {
            loop.execute(() -> {
                if (maintenance != null) {
                    maintenance.cancel(false);
                }
                for (NetworkState network : networks.values()) {
                    closeQuietly(network.socket);
                }
                networks.clear();
                probes.clear();
                subscriptions.clear();
            });
        } catch (RejectedExecutionException ignored) {
            // Loop already stopped.
        }
        group.shutdownGracefully(0, 5, TimeUnit.SECONDS).syncUninterruptibly();
    }

    // ------------------------------------------------------------------ scheduling

    private void execute(Runnable task) {
        if (closing.get()) {
            return;
        }
        try {
            loop.execute(task);
        } catch (RejectedExecutionException ignored) {
            // Transport is stopping.
        }
    }

    private void deliver(Runnable task) {
        if (closing.get()) {
            return;
        }
        try {
            hostLoop.execute(task);
        } catch (RejectedExecutionException ignored) {
            // Host is stopping.
        }
    }

    private static long now() {
        return System.currentTimeMillis();
    }

    // ------------------------------------------------------------------ tick

    private void tick() {
        try {
            long current = now();
            seen.entrySet().removeIf(entry -> entry.getValue() < current);
            for (NetworkState network : new ArrayList<>(networks.values())) {
                network.bindings.entrySet().removeIf(entry -> current - entry.getValue().used > BINDING_TTL_MS);
                for (RelayState relay : network.relays.values()) {
                    relay.reserved = 0;
                }
                refresh(network.id, null);
                for (RelayState relay : new ArrayList<>(network.relays.values())) {
                    if (relay.pending) {
                        continue;
                    }
                    relay.pending = true;
                    String id = network.id;
                    String key = relay.key();
                    InetAddress address = parseIpv4(relay.virtualIp);
                    if (address == null) {
                        relay.pending = false;
                        continue;
                    }
                    probe(id, address, relay.port, (milliseconds, stats) -> probeCompleted(
                            id, key, milliseconds, stats));
                }
            }
            if (current - rateStart > 10_000) {
                rateStart = current;
                forwardedBytes = 0;
            }
        } catch (RuntimeException failure) {
            log.warn("Network transport tick failed: " + failure.getMessage());
        }
    }

    private void probeCompleted(String networkId, String key, double milliseconds, JsonNode stats) {
        NetworkState network = networks.get(networkId);
        if (network == null) {
            return;
        }
        RelayState relay = network.relays.get(key);
        if (relay == null) {
            return;
        }
        relay.pending = false;
        if (milliseconds < 0) {
            relay.failures++;
            if (relay.failures >= 3) {
                relay.available = false;
            }
        } else {
            relay.failures = 0;
            relay.samples.addLast(milliseconds);
            if (relay.samples.size() > 10) {
                relay.samples.removeFirst();
            }
            double total = 0;
            for (double sample : relay.samples) {
                total += sample;
            }
            relay.averageLatency = total / relay.samples.size();
            relay.lastLatency = milliseconds;
            relay.available = true;
            relay.sampledAt = now();
            relay.bytesPerSecond = stats.path("bytesPerSecond").asLong(0);
            relay.queuedBytes = stats.path("queuedBytes").asLong(0);
        }
        Set<String> eligible = new HashSet<>(candidates(network));
        for (Binding binding : network.bindings.values()) {
            if (binding.relay.equals(key)) {
                binding.outside = eligible.contains(key) ? 0 : binding.outside + 1;
            }
        }
    }

    // ------------------------------------------------------------------ configuration

    private void configure(String node, JsonNode descriptors) {
        if (closing.get()) {
            return;
        }
        Set<String> retained = new LinkedHashSet<>();
        if (descriptors != null && descriptors.isArray()) {
            for (JsonNode descriptor : descriptors) {
                if (!descriptor.isObject()) {
                    continue;
                }
                String id = text(descriptor, "nwid", text(descriptor, "id", "")).toLowerCase();
                if (!validNetworkId(id) || !validMemberId(node)) {
                    continue;
                }
                InetAddress local = firstIpv4(descriptor.path("assignedAddresses"));
                if (local == null) {
                    continue;
                }
                retained.add(id);
                NetworkState existing = networks.get(id);
                if (existing != null && !existing.local.equals(local)) {
                    closeQuietly(existing.socket);
                    networks.remove(id);
                }
                NetworkState network = networks.computeIfAbsent(id, ignored -> new NetworkState(id));
                network.node = node.toLowerCase();
                network.local = local;
                network.controller = networkServer(descriptor);
                if (descriptor.has("transportController") && descriptor.get("transportController").isTextual()) {
                    InetAddress override = parseIpv4(descriptor.get("transportController").asText());
                    if (override != null) {
                        network.controller = override;
                    }
                }
                network.controllerPort = descriptor.path("transportControllerPort").asInt(NETWORK_PORT);
                if (network.controller == null) {
                    network.controller = local;
                }
                if (network.socket == null) {
                    bindNetworkSocket(network);
                }
                refresh(id, null);
            }
        }
        for (String id : new ArrayList<>(networks.keySet())) {
            if (!retained.contains(id)) {
                NetworkState network = networks.remove(id);
                if (network != null) {
                    closeQuietly(network.socket);
                }
            }
        }
    }

    private void bindNetworkSocket(NetworkState network) {
        Bootstrap bootstrap = new Bootstrap();
        InetAddress local = network.local;
        bootstrap.group(loop)
                .channel(NioDatagramChannel.class)
                .option(ChannelOption.SO_REUSEADDR, true)
                .localAddress(new InetSocketAddress(local, server ? 0 : CLIENT_NETWORK_PORT))
                .handler(new ChannelInboundHandlerAdapter() {
                    @Override
                    public void channelRead(ChannelHandlerContext context, Object message) {
                        DatagramPacket packet = (DatagramPacket) message;
                        ByteBuf content = packet.content();
                        try {
                            int size = content.readableBytes();
                            if (size > MAX_UDP_BYTES) {
                                return;
                            }
                            byte[] datagram = new byte[size];
                            content.readBytes(datagram);
                            InetSocketAddress sender = packet.sender();
                            Wire.Packet decoded = Wire.decode(datagram);
                            if (decoded != null && Wire.BUILTIN.equals(decoded.route())) {
                                datagramOnLoop(sender.getAddress(), sender.getPort(),
                                        decoded.payload());
                            }
                        } finally {
                            content.release();
                        }
                    }

                    @Override
                    public void exceptionCaught(ChannelHandlerContext context, Throwable failure) {
                        log.warn("Network socket error: " + failure.getMessage());
                    }
                });
        ChannelFuture future = bootstrap.bind();
        future.addListener(result -> {
            if (!result.isSuccess()) {
                log.warn("Network socket bind failed for " + network.id + ": "
                        + describe(result.cause()));
                networks.remove(network.id);
                return;
            }
            network.socket = future.channel();
        });
    }

    // ------------------------------------------------------------------ refresh

    private void refresh(String id, Consumer<JsonNode> done) {
        NetworkState network = networks.get(id);
        if (network == null) {
            if (done != null) {
                done.accept(failure("network_unavailable"));
            }
            return;
        }
        if (done != null) {
            network.waiting.add(done);
        }
        if (network.refreshing) {
            return;
        }
        network.refreshing = true;
        InetAddress controller = network.controller;
        rpc(network.local, controller, network.controllerPort, "controller.members",
                networkRequest(id), REFRESH_DEADLINE_MS, members -> {
                    if (!networks.containsKey(id)) {
                        return;
                    }
                    NetworkState current = networks.get(id);
                    if (members.path("ok").asBoolean(false) && members.path("data").isArray()) {
                        current.members = members.path("data");
                        current.membersById.clear();
                        for (JsonNode member : current.members) {
                            String memberId = member.path("nodeId").asText("");
                            if (validMemberId(memberId)) {
                                current.membersById.put(memberId.toLowerCase(), member);
                            }
                        }
                        current.membersAt = now();
                    }
                    rpc(current.local, controller, network.controllerPort, "network.relays",
                            networkRequest(id), REFRESH_DEADLINE_MS, relays -> {
                                if (!networks.containsKey(id)) {
                                    return;
                                }
                                NetworkState latest = networks.get(id);
                                JsonNode data = relays.path("data");
                                JsonNode list = data.path("relays");
                                if (!list.isArray() || list.isEmpty()) {
                                    ArrayNode fallback = JsonSupport.mapper().createArrayNode();
                                    ObjectNode primary = fallback.addObject();
                                    primary.put("nodeId", id.substring(0, 10));
                                    primary.put("moonId", "000000" + id.substring(0, 10));
                                    primary.put("virtualIp", latest.controller.getHostAddress());
                                    primary.put("port", latest.controllerPort);
                                    list = fallback;
                                }
                                Set<String> kept = new HashSet<>();
                                for (JsonNode entry : list) {
                                    String nodeId = entry.path("nodeId").asText("");
                                    InetAddress ip = parseIpv4(entry.path("virtualIp").asText(""));
                                    int port = entry.path("port").asInt(NETWORK_PORT);
                                    if (!validMemberId(nodeId)
                                            || !ip.equals(memberAddress(member(latest, nodeId)))
                                            || port < 1 || port > 65535) {
                                        continue;
                                    }
                                    String key = ip.getHostAddress() + ":" + port;
                                    kept.add(key);
                                    RelayState relay = latest.relays.computeIfAbsent(key, k -> new RelayState());
                                    relay.nodeId = nodeId.toLowerCase();
                                    relay.moonId = entry.path("moonId").asText("");
                                    relay.virtualIp = ip.getHostAddress();
                                    relay.port = port;
                                }
                                latest.relays.keySet().retainAll(kept);
                                JsonNode endpoints = data.path("endpoints");
                                if (endpoints.isObject()) {
                                    endpoints.fields().forEachRemaining(entry -> {
                                        int port = entry.getValue().asInt(0);
                                        if (port == CLIENT_NETWORK_PORT || port == NETWORK_PORT) {
                                            latest.endpoints.put(entry.getKey(), port);
                                        }
                                    });
                                }
                                for (RelayState relay : latest.relays.values()) {
                                    ObjectNode register = JsonSupport.object();
                                    register.put("networkId", id);
                                    register.put("senderId", latest.node);
                                    register.put("port", server ? NETWORK_PORT : CLIENT_NETWORK_PORT);
                                    InetAddress address = parseIpv4(relay.virtualIp);
                                    if (address != null) {
                                        rpc(latest.local, address, relay.port, "network.register",
                                                register, DEFAULT_RPC_DEADLINE_MS, ignored -> {});
                                    }
                                }
                                latest.refreshing = false;
                                List<Consumer<JsonNode>> waiting = new ArrayList<>(latest.waiting);
                                latest.waiting.clear();
                                boolean ready = latest.membersAt > 0
                                        && now() - latest.membersAt <= MEMBER_TTL_MS;
                                for (Consumer<JsonNode> callback : waiting) {
                                    callback.accept(ready ? success(JsonSupport.object())
                                            : failure("member_addresses_unavailable"));
                                }
                            });
                });
    }

    private ObjectNode networkRequest(String id) {
        ObjectNode request = JsonSupport.object();
        request.put("networkId", id);
        return request;
    }

    // ------------------------------------------------------------------ members

    private JsonNode member(NetworkState network, String id) {
        if (id == null || now() - network.membersAt > MEMBER_TTL_MS) {
            return null;
        }
        return network.membersById.get(id.toLowerCase());
    }

    private boolean authorized(NetworkState network, String node, InetAddress source) {
        return source != null && source.equals(memberAddress(member(network, node)));
    }

    private static InetAddress memberAddress(JsonNode member) {
        if (member == null || !member.path("authorized").asBoolean(false)) {
            return null;
        }
        JsonNode assignments = member.path("ipAssignments");
        if (!assignments.isArray()) {
            return null;
        }
        for (JsonNode value : assignments) {
            String text = value.asText("").split("/", 2)[0];
            InetAddress address = parseIpv4(text);
            if (address == null) {
                continue;
            }
            long numeric = ipv4ToLong(address);
            if ((numeric & 0xff) != 0 && (numeric & 0xff) != 255) {
                return address;
            }
        }
        return null;
    }

    private static InetAddress firstIpv4(JsonNode values) {
        if (values == null || !values.isArray()) {
            return null;
        }
        for (JsonNode value : values) {
            InetAddress address = parseIpv4(value.asText("").split("/", 2)[0]);
            if (address != null) {
                return address;
            }
        }
        return null;
    }

    private static InetAddress networkServer(JsonNode descriptor) {
        InetAddress assigned = firstIpv4(descriptor.path("assignedAddresses"));
        if (assigned == null) {
            return null;
        }
        long numeric = ipv4ToLong(assigned) & 0xffffff00L;
        return fromIpv4(numeric | 1);
    }

    private static InetAddress parseIpv4(String text) {
        if (text == null || !text.matches("\\d{1,3}(?:\\.\\d{1,3}){3}")) {
            return null;
        }
        try {
            InetAddress address = InetAddress.getByName(text);
            if (address instanceof Inet4Address && !address.isMulticastAddress()) {
                return address;
            }
            return null;
        } catch (UnknownHostException invalid) {
            return null;
        }
    }

    private static long ipv4ToLong(InetAddress address) {
        byte[] bytes = address.getAddress();
        return ((long) (bytes[0] & 0xff) << 24)
                | ((long) (bytes[1] & 0xff) << 16)
                | ((long) (bytes[2] & 0xff) << 8)
                | (bytes[3] & 0xff);
    }

    private static InetAddress fromIpv4(long value) {
        byte[] bytes = new byte[4];
        bytes[0] = (byte) (value >>> 24);
        bytes[1] = (byte) (value >>> 16);
        bytes[2] = (byte) (value >>> 8);
        bytes[3] = (byte) value;
        try {
            return InetAddress.getByAddress(bytes);
        } catch (UnknownHostException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private List<String> candidates(NetworkState network) {
        double minimum = Double.MAX_VALUE;
        for (RelayState relay : network.relays.values()) {
            if (relay.available && now() - relay.sampledAt <= PROBE_FRESH_MS) {
                minimum = Math.min(minimum, relay.averageLatency);
            }
        }
        double ceiling = minimum + Math.max(5.0, minimum * 0.2);
        List<String> result = new ArrayList<>();
        for (Map.Entry<String, RelayState> entry : network.relays.entrySet()) {
            RelayState relay = entry.getValue();
            if (relay.available && now() - relay.sampledAt <= PROBE_FRESH_MS
                    && relay.averageLatency >= 0 && relay.averageLatency <= ceiling) {
                result.add(entry.getKey());
            }
        }
        return result;
    }

    // ------------------------------------------------------------------ datagrams

    private void datagramOnLoop(InetAddress source, int sourcePort, byte[] payload) {
        try {
            Delivery delivery = decodeDelivery(payload);
            if (delivery != null) {
                NetworkState network = networks.get(delivery.network);
                if (network == null || !delivery.target.equals(network.node)
                        || memberAddress(member(network, delivery.sender)) == null) {
                    return;
                }
                boolean trusted = authorized(network, delivery.sender, source);
                for (RelayState relay : network.relays.values()) {
                    InetAddress relayAddress = parseIpv4(relay.virtualIp);
                    if (relayAddress != null && relayAddress.equals(source)) {
                        trusted = true;
                        break;
                    }
                }
                if (!trusted) {
                    return;
                }
                String key = delivery.network + "/" + delivery.sender + "/" + delivery.plugin + "/"
                        + hex(delivery.messageId);
                Long until = seen.get(key);
                if (until != null && until > now()) {
                    return;
                }
                if (seen.size() >= MAX_SEEN) {
                    return;
                }
                seen.put(key, now() + SEEN_TTL_MS);
                dispatchReceived(delivery);
                return;
            }
            JsonNode object = JsonSupport.parse(payload);
            if (object == null || !"network.probe".equals(object.path("op").asText(""))) {
                return;
            }
            String nonce = object.path("nonce").asText("");
            if (object.path("reply").asBoolean(false)) {
                Probe probe = probes.get(nonce);
                if (probe == null || !probe.address.equals(source)
                        || !probe.network.equals(object.path("networkId").asText(""))) {
                    return;
                }
                probes.remove(nonce);
                long elapsed = System.nanoTime() - probe.started;
                probe.callback.accept(elapsed / 1_000_000.0, object);
                return;
            }
            String networkId = object.path("networkId").asText("");
            NetworkState network = networks.get(networkId);
            if (network == null || !authorized(network, object.path("senderId").asText(""), source)) {
                return;
            }
            ObjectNode reply = ((ObjectNode) object).deepCopy();
            reply.put("reply", true);
            long elapsed = Math.max(1000, now() - rateStart);
            reply.put("bytesPerSecond", forwardedBytes * 1000.0 / elapsed);
            reply.put("queuedBytes", 0);
            byte[] bytes = Wire.encode(Wire.BUILTIN, JsonSupport.compact(reply));
            if (bytes != null) {
                sendDatagram(network.socket, bytes, source, sourcePort);
            }
        } catch (IOException | RuntimeException failure) {
            log.debug("Network datagram ignored: " + describe(failure));
        }
    }

    private void dispatchReceived(Delivery delivery) {
        byte[] data = delivery.data;
        String networkId = delivery.network;
        String sender = delivery.sender;
        deliver(() -> {
            for (Subscription subscription : subscriptions) {
                if (subscription.cancelled || !subscription.plugin.equalsIgnoreCase(delivery.plugin)
                        || !subscription.network.equals(networkId)) {
                    continue;
                }
                try {
                    subscription.callback.completed(new NetworkMessage(networkId, sender, data));
                } catch (RuntimeException failure) {
                    log.warn("Network subscription callback failed: " + failure.getMessage());
                }
            }
        });
    }

    private void probe(
            String networkId,
            InetAddress address,
            int port,
            BiConsumer<Double, JsonNode> callback) {
        NetworkState network = networks.get(networkId);
        if (network == null) {
            callback.accept(-1.0, JsonSupport.object());
            return;
        }
        String nonce = UUID.randomUUID().toString().replace("-", "");
        Probe probe = new Probe(networkId, address, System.nanoTime(), callback);
        probes.put(nonce, probe);
        ObjectNode request = JsonSupport.object();
        request.put("op", "network.probe");
        request.put("networkId", networkId);
        request.put("senderId", network.node);
        request.put("nonce", nonce);
        byte[] bytes = Wire.encode(Wire.BUILTIN, JsonSupport.compact(request));
        if (bytes == null || !sendDatagram(network.socket, bytes, address, port)) {
            probes.remove(nonce);
            callback.accept(-1.0, JsonSupport.object());
            return;
        }
        loop.schedule(() -> {
            Probe expired = probes.remove(nonce);
            if (expired != null) {
                expired.callback.accept(-1.0, JsonSupport.object());
            }
        }, PROBE_TIMEOUT_MS, TimeUnit.MILLISECONDS);
    }

    // ------------------------------------------------------------------ builtin operations

    private void builtinOnLoop(InetAddress source, JsonNode request, BuiltinJob job) {
        try {
            if (job.cancelled.get()) {
                return;
            }
            JsonNode data = request.path("data");
            if (!data.isObject()) {
                data = JsonSupport.object();
            }
            JsonNode parameters = data;
            String id = parameters.path("networkId").asText("").toLowerCase();
            String operation = request.path("op").asText("");
            if (!networks.containsKey(id)) {
                job.complete(failure("network_unavailable"));
                return;
            }
            Consumer<JsonNode> execute = refreshed -> {
                if (job.cancelled.get() || closing.get()) {
                    return;
                }
                if (!refreshed.path("ok").asBoolean(false) || !networks.containsKey(id)) {
                    job.complete(failure("members_unavailable"));
                    return;
                }
                operateNetwork(source, parameters, id, operation, job);
            };
            NetworkState network = networks.get(id);
            if (network.membersAt > 0 && now() - network.membersAt <= MEMBER_TTL_MS) {
                execute.accept(success(JsonSupport.object()));
            } else if ("network.relays".equals(operation)) {
                job.complete(failure("members_unavailable"));
            } else {
                refresh(id, execute);
            }
        } catch (RuntimeException failure) {
            log.warn("Network builtin request failed: " + failure.getMessage());
            job.complete(failure("network_unavailable"));
        }
    }

    private void operateNetwork(
            InetAddress source, JsonNode data, String id, String operation, BuiltinJob job) {
        NetworkState network = networks.get(id);
        if (network == null) {
            job.complete(failure("network_unavailable"));
            return;
        }
        String sender = data.path("senderId").asText("").toLowerCase();
        if ("network.relays".equals(operation)) {
            boolean allowed = false;
            for (JsonNode member : network.members) {
                InetAddress address = memberAddress(member);
                if (address != null && address.equals(source)) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                job.complete(failure("unauthorized"));
                return;
            }
            ObjectNode response = JsonSupport.object();
            response.put("ok", true);
            ArrayNode relays = response.putObject("data").putArray("relays");
            JsonNode configured = relayConfig.path(id);
            if (configured.isArray()) {
                relays.addAll((ArrayNode) configured);
            }
            ObjectNode primary = relays.insertObject(0);
            primary.put("nodeId", id.substring(0, 10));
            primary.put("moonId", "000000" + id.substring(0, 10));
            primary.put("virtualIp", networkServerAddress(network));
            primary.put("port", NETWORK_PORT);
            ObjectNode endpoints = (ObjectNode) response.path("data");
            ObjectNode values = endpoints.putObject("endpoints");
            long current = now();
            network.endpoints.forEach((node, port) -> {
                Long at = network.endpointAt.get(node);
                if (at != null && current - at <= ENDPOINT_TTL_MS) {
                    values.put(node, port);
                }
            });
            job.complete(response);
            return;
        }
        if (!authorized(network, sender, source)) {
            job.complete(failure("unauthorized_sender"));
            return;
        }
        if ("network.register".equals(operation)) {
            int port = data.path("port").asInt(0);
            if (port != CLIENT_NETWORK_PORT && port != NETWORK_PORT) {
                job.complete(failure("invalid_port"));
                return;
            }
            long current = now();
            Long at = network.endpointAt.get(sender);
            if (port == CLIENT_NETWORK_PORT || !network.endpoints.containsKey(sender)
                    || at == null || current - at > ENDPOINT_TTL_MS) {
                network.endpoints.put(sender, port);
                network.endpointAt.put(sender, current);
            } else if (network.endpoints.get(sender) == port) {
                network.endpointAt.put(sender, current);
            }
            job.complete(success(JsonSupport.object()));
            return;
        }
        if (!"network.forward".equals(operation)) {
            job.complete(failure("unknown_operation"));
            return;
        }
        forward(network, sender, data, job);
    }

    private String networkServerAddress(NetworkState network) {
        InetAddress serverAddress = fromIpv4((ipv4ToLong(network.local) & 0xffffff00L) | 1);
        return serverAddress.getHostAddress();
    }

    private void forward(NetworkState network, String sender, JsonNode data, BuiltinJob job) {
        String plugin = data.path("pluginId").asText("");
        byte[] message = decodeHex(data.path("messageId").asText(""));
        byte[] payload = decodeBase64(data.path("payload").asText(""));
        JsonNode targets = data.path("members");
        if (!validPluginId(plugin) || message == null || message.length != 16 || payload == null
                || payload.length > maxNetworkDataBytes(plugin)
                || !targets.isArray() || targets.size() > MAX_TARGETS) {
            job.complete(failure("invalid_forward"));
            return;
        }
        int sent = 0;
        ArrayNode failed = JsonSupport.mapper().createArrayNode();
        Set<String> unique = new HashSet<>();
        for (JsonNode targetNode : targets) {
            String target = targetNode.asText("").toLowerCase();
            if (!unique.add(target)) {
                continue;
            }
            InetAddress address = memberAddress(member(network, target));
            if (!validMemberId(target) || address == null) {
                failed.add(target);
                continue;
            }
            String key = network.id + "/" + sender + "/" + plugin + "/" + hex(message) + "/" + target;
            Long until = seen.get(key);
            if (until != null && until > now()) {
                sent++;
                continue;
            }
            byte[] bytes = encodeDelivery(network.id, sender, target, plugin, message, payload);
            int port = network.endpoints.getOrDefault(target, CLIENT_NETWORK_PORT);
            if (bytes == null || seen.size() >= MAX_SEEN
                    || !sendDatagram(network.socket, bytes, address, port)) {
                failed.add(target);
            } else {
                seen.put(key, now() + SEEN_TTL_MS);
                forwardedBytes += bytes.length;
                sent++;
            }
        }
        ObjectNode resultData = JsonSupport.object();
        resultData.put("recipients", sent);
        resultData.set("failedMembers", failed);
        job.complete(success(resultData));
    }

    // ------------------------------------------------------------------ plugin send API

    private void sendOnLoop(
            SendJob job, String plugin, String networkId, List<String> targets, byte[] data, boolean balanced) {
        try {
            String id = networkId.toLowerCase();
            Set<String> unique = new LinkedHashSet<>();
            List<String> normalized = new ArrayList<>();
            for (String target : targets) {
                String lower = target == null ? "" : target.toLowerCase();
                if (!validMemberId(lower)) {
                    job.complete(SendResult.failure("invalid_member_id", List.of()));
                    return;
                }
                if (unique.add(lower)) {
                    normalized.add(lower);
                }
            }
            if (!validPluginId(plugin) || !validNetworkId(id)
                    || data.length > maxNetworkDataBytes(plugin) || normalized.size() > MAX_TARGETS) {
                job.complete(SendResult.failure("invalid_network_or_payload", List.of()));
                return;
            }
            if (normalized.isEmpty()) {
                job.complete(SendResult.nothing());
                return;
            }
            Consumer<JsonNode> perform = response -> performSend(
                    job, plugin, id, normalized, data, balanced, response);
            NetworkState network = networks.get(id);
            if (network != null && network.membersAt > 0 && now() - network.membersAt <= 2000) {
                perform.accept(success(JsonSupport.object()));
            } else {
                refresh(id, perform);
            }
        } catch (RuntimeException failure) {
            log.warn("Network send failed: " + failure.getMessage());
            job.complete(SendResult.failure("network_unavailable", List.of()));
        }
    }

    private void performSend(
            SendJob job,
            String plugin,
            String id,
            List<String> targets,
            byte[] data,
            boolean balanced,
            JsonNode response) {
        if (job.cancelled.get() || closing.get()) {
            return;
        }
        if (!response.path("ok").asBoolean(false) || !networks.containsKey(id)) {
            job.complete(SendResult.failure(
                    response.path("error").asText("network_unavailable"), targets));
            return;
        }
        NetworkState network = networks.get(id);
        SenderProgress progress = new SenderProgress();
        byte[] message = randomMessageId();
        Map<String, List<String>> groups = new LinkedHashMap<>();
        for (String target : targets) {
            InetAddress address = memberAddress(member(network, target));
            if (address == null) {
                progress.failedMembers.add(target);
                continue;
            }
            String relay = "";
            if (balanced) {
                List<String> eligible = candidates(network);
                String bindingKey = plugin + "/" + network.node + "/" + target;
                Binding binding = network.bindings.get(bindingKey);
                boolean valid = binding != null && !binding.relay.isEmpty()
                        && network.relays.containsKey(binding.relay)
                        && network.relays.get(binding.relay).available
                        && now() - network.relays.get(binding.relay).sampledAt <= PROBE_FRESH_MS
                        && binding.outside < 3;
                if (valid) {
                    relay = binding.relay;
                } else if (!eligible.isEmpty()) {
                    relay = eligible.get(0);
                    for (String candidate : eligible) {
                        RelayState current = network.relays.get(candidate);
                        RelayState chosen = network.relays.get(relay);
                        long currentLoad = current.bytesPerSecond + current.queuedBytes + current.reserved;
                        long chosenLoad = chosen.bytesPerSecond + chosen.queuedBytes + chosen.reserved;
                        if (currentLoad < chosenLoad) {
                            relay = candidate;
                        }
                    }
                    if (binding == null) {
                        binding = new Binding();
                        network.bindings.put(bindingKey, binding);
                    }
                    binding.relay = relay;
                    binding.outside = 0;
                }
                if (binding != null) {
                    binding.used = now();
                }
                if (!relay.isEmpty()) {
                    network.relays.get(relay).reserved += data.length + 52L + plugin.length();
                }
            } else {
                String key = network.controller.getHostAddress() + ":" + network.controllerPort;
                RelayState relayState = network.relays.get(key);
                if (relayState != null
                        && ((relayState.available && now() - relayState.sampledAt <= PROBE_FRESH_MS)
                        || (relayState.sampledAt == 0 && relayState.failures < 3))) {
                    relay = key;
                }
            }
            groups.computeIfAbsent(relay, ignored -> new ArrayList<>()).add(target);
        }
        if (groups.isEmpty()) {
            job.complete(new SendResult(0, "no_authorized_targets", progress.failedMembers));
            return;
        }
        progress.outstanding = groups.size();
        for (Map.Entry<String, List<String>> group : groups.entrySet()) {
            List<String> members = group.getValue();
            if (group.getKey().isEmpty()) {
                direct(network, plugin, id, members, data, message, progress, job);
                finishSend(progress, job);
                continue;
            }
            relayForward(job, network, plugin, id, group.getKey(), members, data, message,
                    balanced, progress, new HashSet<>());
        }
    }

    private void relayForward(
            SendJob job,
            NetworkState network,
            String plugin,
            String networkId,
            String key,
            List<String> members,
            byte[] data,
            byte[] message,
            boolean balanced,
            SenderProgress progress,
            Set<String> attempted) {
        if (job.cancelled.get() || closing.get()) {
            return;
        }
        RelayState relay = network.relays.get(key);
        if (relay == null) {
            direct(network, plugin, networkId, members, data, message, progress, job);
            finishSend(progress, job);
            return;
        }
        attempted.add(key);
        ObjectNode request = JsonSupport.object();
        request.put("networkId", networkId);
        request.put("senderId", network.node);
        request.put("pluginId", plugin);
        ArrayNode memberArray = request.putArray("members");
        members.forEach(memberArray::add);
        request.put("messageId", hex(message));
        request.put("payload", Base64.getEncoder().encodeToString(data));
        InetAddress address = parseIpv4(relay.virtualIp);
        if (address == null) {
            markUnavailable(network, key);
            retryOrDirect(job, network, plugin, networkId, key, members, data, message,
                    balanced, progress, attempted);
            return;
        }
        rpc(network.local, address, relay.port, "network.forward", request,
                DEFAULT_RPC_DEADLINE_MS, ack -> {
                    if (job.cancelled.get() || closing.get()) {
                        return;
                    }
                    JsonNode response = ack.path("data");
                    List<String> failed = new ArrayList<>();
                    for (JsonNode value : response.path("failedMembers")) {
                        String member = value.asText("");
                        if (members.contains(member)) {
                            failed.add(member);
                        }
                    }
                    int count = response.path("recipients").asInt(-1);
                    if (ack.path("ok").asBoolean(false) && count >= 0
                            && count + failed.size() == members.size()) {
                        progress.recipients += count;
                        direct(network, plugin, networkId, failed, data, message, progress, job);
                        finishSend(progress, job);
                        return;
                    }
                    markUnavailable(network, key);
                    retryOrDirect(job, network, plugin, networkId, key, members, data, message,
                            balanced, progress, attempted);
                });
    }

    private void retryOrDirect(
            SendJob job,
            NetworkState network,
            String plugin,
            String networkId,
            String failedKey,
            List<String> members,
            byte[] data,
            byte[] message,
            boolean balanced,
            SenderProgress progress,
            Set<String> attempted) {
        String next = "";
        if (balanced) {
            for (String candidate : candidates(network)) {
                if (attempted.contains(candidate)) {
                    continue;
                }
                if (next.isEmpty()) {
                    next = candidate;
                    continue;
                }
                RelayState current = network.relays.get(candidate);
                RelayState chosen = network.relays.get(next);
                long currentLoad = current.bytesPerSecond + current.queuedBytes + current.reserved;
                long chosenLoad = chosen.bytesPerSecond + chosen.queuedBytes + chosen.reserved;
                if (currentLoad < chosenLoad) {
                    next = candidate;
                }
            }
            if (!next.isEmpty()) {
                RelayState relay = network.relays.get(next);
                relay.reserved += (long) (data.length + 52 + plugin.length()) * members.size();
                for (String target : members) {
                    Binding binding = new Binding();
                    binding.relay = next;
                    binding.used = now();
                    network.bindings.put(plugin + "/" + network.node + "/" + target, binding);
                }
            }
        }
        if (!next.isEmpty()) {
            relayForward(job, network, plugin, networkId, next, members, data, message,
                    balanced, progress, attempted);
        } else {
            direct(network, plugin, networkId, members, data, message, progress, job);
            finishSend(progress, job);
        }
    }

    private void markUnavailable(NetworkState network, String key) {
        RelayState relay = network.relays.get(key);
        if (relay != null) {
            relay.available = false;
        }
    }

    private void direct(
            NetworkState network,
            String plugin,
            String networkId,
            List<String> members,
            byte[] data,
            byte[] message,
            SenderProgress progress,
            SendJob job) {
        for (String target : members) {
            InetAddress address = memberAddress(member(network, target));
            byte[] bytes = encodeDelivery(networkId, network.node, target, plugin, message, data);
            int port = network.endpoints.getOrDefault(target, CLIENT_NETWORK_PORT);
            if (address == null || bytes == null || !sendDatagram(network.socket, bytes, address, port)) {
                progress.failedMembers.add(target);
            } else {
                progress.recipients++;
            }
        }
    }

    private void finishSend(SenderProgress progress, SendJob job) {
        if (--progress.outstanding > 0) {
            return;
        }
        String error = progress.failedMembers.isEmpty() ? "" : "some_members_not_queued";
        job.complete(new SendResult(progress.recipients, error, progress.failedMembers));
    }

    private void relaysOnLoop(RelayJob job, String networkId) {
        String id = networkId.toLowerCase();
        refresh(id, response -> {
            if (job.cancelled.get() || closing.get()) {
                return;
            }
            RelayResult result;
            if (!response.path("ok").asBoolean(false)) {
                result = RelayResult.failure(response.path("error").asText("network_unavailable"));
            } else {
                List<RelayInfo> relays = new ArrayList<>();
                NetworkState network = networks.get(id);
                if (network != null) {
                    for (RelayState relay : network.relays.values()) {
                        boolean available = relay.available
                                && now() - relay.sampledAt <= PROBE_FRESH_MS;
                        relays.add(new RelayInfo(relay.nodeId, relay.moonId, relay.virtualIp,
                                relay.port, relay.averageLatency, relay.lastLatency, available,
                                relay.sampledAt, relay.bytesPerSecond, relay.queuedBytes));
                    }
                }
                result = RelayResult.success(relays);
            }
            RelayResult value = result;
            job.complete(value);
        });
    }

    // ------------------------------------------------------------------ RPC

    private void rpc(
            InetAddress source,
            InetAddress destination,
            int port,
            String operation,
            ObjectNode data,
            int deadlineMs,
            Consumer<JsonNode> callback) {
        if (pendingRpc >= MAX_PENDING_RPC) {
            callback.accept(failure("too_many_pending_requests"));
            return;
        }
        pendingRpc++;
        ObjectNode request = JsonSupport.object();
        request.put("v", 1);
        request.put("id", 1);
        request.put("op", operation);
        request.set("data", data);
        byte[] framed = Wire.frame(Wire.BUILTIN, JsonSupport.compact(request));
        if (framed == null) {
            pendingRpc--;
            callback.accept(failure("invalid_request"));
            return;
        }
        RpcCall call = new RpcCall(callback);
        call.timeout = loop.schedule(
                () -> call.finish(failure("request_timeout")), deadlineMs, TimeUnit.MILLISECONDS);
        Bootstrap bootstrap = new Bootstrap();
        bootstrap.group(loop)
                .channel(NioSocketChannel.class)
                .option(ChannelOption.CONNECT_TIMEOUT_MILLIS, deadlineMs)
                .option(ChannelOption.TCP_NODELAY, true)
                .handler(new ChannelInitializer<SocketChannel>() {
                    @Override
                    protected void initChannel(SocketChannel channel) {
                        channel.pipeline().addLast(new ZtsFrameDecoder());
                        channel.pipeline().addLast(new SimpleChannelInboundHandler<Wire.Packet>() {
                            @Override
                            protected void channelRead0(ChannelHandlerContext context, Wire.Packet packet) {
                                if (call.finished.get()) {
                                    return;
                                }
                                if (!Wire.BUILTIN.equals(packet.route())) {
                                    call.finish(failure("invalid_response"));
                                    return;
                                }
                                try {
                                    JsonNode document = JsonSupport.parse(packet.payload());
                                    if (document == null || !document.isObject()
                                            || document.path("v").asInt(0) != 1
                                            || document.path("id").asInt(0) != 1) {
                                        call.finish(failure("invalid_response"));
                                    } else {
                                        call.finish(document);
                                    }
                                } catch (IOException invalid) {
                                    call.finish(failure("invalid_response"));
                                }
                            }

                            @Override
                            public void channelInactive(ChannelHandlerContext context) {
                                call.finish(failure("connection_closed"));
                            }

                            @Override
                            public void exceptionCaught(ChannelHandlerContext context, Throwable failure) {
                                call.finish(failure("invalid_response"));
                                context.close();
                            }
                        });
                    }
                });
        if (source != null) {
            bootstrap.localAddress(new InetSocketAddress(source, 0));
        }
        ChannelFuture connect = bootstrap.connect(destination, port);
        call.channel = connect.channel();
        connect.addListener(result -> {
            if (!result.isSuccess()) {
                String message = describe(result.cause());
                call.finish(failure(message.toLowerCase().contains("bind")
                        ? "source_bind_failed" : "connection_failed"));
                return;
            }
            if (call.finished.get()) {
                return;
            }
            call.channel.writeAndFlush(Unpooled.wrappedBuffer(framed));
        });
    }

    private final class RpcCall {
        private final Consumer<JsonNode> callback;
        private final AtomicBoolean finished = new AtomicBoolean();
        private Channel channel;
        private ScheduledFuture<?> timeout;

        private RpcCall(Consumer<JsonNode> callback) {
            this.callback = callback;
        }

        private void finish(JsonNode result) {
            if (!finished.compareAndSet(false, true)) {
                return;
            }
            if (timeout != null) {
                timeout.cancel(false);
            }
            if (channel != null) {
                channel.close();
            }
            pendingRpc--;
            try {
                callback.accept(result);
            } catch (RuntimeException failure) {
                log.warn("Network transport callback failed: " + failure.getMessage());
            }
        }
    }

    // ------------------------------------------------------------------ helpers

    private boolean sendDatagram(Channel socket, byte[] bytes, InetAddress address, int port) {
        if (socket == null || !socket.isActive() || address == null || port < 1 || port > 65535) {
            return false;
        }
        socket.writeAndFlush(new DatagramPacket(
                Unpooled.wrappedBuffer(bytes), new InetSocketAddress(address, port)));
        return true;
    }

    private static String text(JsonNode node, String field, String fallback) {
        JsonNode value = node.get(field);
        return value != null && value.isTextual() ? value.asText() : fallback;
    }

    private static String describe(Throwable failure) {
        if (failure == null) {
            return "unknown";
        }
        String message = failure.getMessage();
        return message == null || message.isBlank() ? failure.getClass().getSimpleName() : message;
    }

    private static void closeQuietly(Channel channel) {
        if (channel != null) {
            channel.close();
        }
    }

    private static ObjectNode success(JsonNode data) {
        ObjectNode node = JsonSupport.object();
        node.put("ok", true);
        node.set("data", data == null ? JsonSupport.object() : data);
        return node;
    }

    private static ObjectNode failure(String error) {
        ObjectNode node = JsonSupport.object();
        node.put("ok", false);
        node.put("error", error);
        return node;
    }

    private static boolean validNetworkId(String id) {
        return id != null && NETWORK_ID.matcher(id).matches();
    }

    private static boolean validMemberId(String id) {
        return id != null && MEMBER_ID.matcher(id).matches();
    }

    private static boolean validPluginId(String id) {
        return Wire.validId(id) && !Wire.BUILTIN.equalsIgnoreCase(id);
    }

    private static int maxNetworkDataBytes(String plugin) {
        return MAX_UDP_BYTES - 52 - plugin.getBytes(StandardCharsets.US_ASCII).length;
    }

    private static String hex(byte[] bytes) {
        StringBuilder builder = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            builder.append(Character.forDigit((value >>> 4) & 0xf, 16));
            builder.append(Character.forDigit(value & 0xf, 16));
        }
        return builder.toString();
    }

    private static byte[] decodeHex(String text) {
        if (text == null || (text.length() & 1) != 0) {
            return null;
        }
        byte[] result = new byte[text.length() / 2];
        for (int index = 0; index < result.length; index++) {
            int high = Character.digit(text.charAt(index * 2), 16);
            int low = Character.digit(text.charAt(index * 2 + 1), 16);
            if (high < 0 || low < 0) {
                return null;
            }
            result[index] = (byte) ((high << 4) | low);
        }
        return result;
    }

    private static byte[] decodeBase64(String text) {
        try {
            return Base64.getDecoder().decode(text == null ? "" : text);
        } catch (IllegalArgumentException invalid) {
            return null;
        }
    }

    private static byte[] randomMessageId() {
        UUID uuid = UUID.randomUUID();
        long high = uuid.getMostSignificantBits();
        long low = uuid.getLeastSignificantBits();
        byte[] bytes = new byte[16];
        for (int index = 0; index < 8; index++) {
            bytes[index] = (byte) (high >>> (56 - index * 8));
            bytes[8 + index] = (byte) (low >>> (56 - index * 8));
        }
        return bytes;
    }

    private static byte[] encodeDelivery(
            String network, String sender, String target, String plugin, byte[] messageId, byte[] data) {
        if (!validNetworkId(network) || !validMemberId(sender) || !validMemberId(target)
                || !validPluginId(plugin) || messageId == null || messageId.length != 16
                || data.length > maxNetworkDataBytes(plugin)) {
            return null;
        }
        byte[] pluginBytes = plugin.getBytes(StandardCharsets.US_ASCII);
        int size = 4 + 8 + 5 + 5 + 16 + 1 + pluginBytes.length + data.length;
        byte[] payload = new byte[size];
        int offset = 0;
        payload[offset++] = 'Z';
        payload[offset++] = 'T';
        payload[offset++] = 'D';
        payload[offset++] = '1';
        System.arraycopy(decodeHex(network), 0, payload, offset, 8);
        offset += 8;
        System.arraycopy(decodeHex(sender), 0, payload, offset, 5);
        offset += 5;
        System.arraycopy(decodeHex(target), 0, payload, offset, 5);
        offset += 5;
        System.arraycopy(messageId, 0, payload, offset, 16);
        offset += 16;
        payload[offset++] = (byte) pluginBytes.length;
        System.arraycopy(pluginBytes, 0, payload, offset, pluginBytes.length);
        offset += pluginBytes.length;
        System.arraycopy(data, 0, payload, offset, data.length);
        return Wire.encode(Wire.BUILTIN, payload);
    }

    private static Delivery decodeDelivery(byte[] payload) {
        if (payload == null || payload.length < 40
                || payload[0] != 'Z' || payload[1] != 'T' || payload[2] != 'D' || payload[3] != '1') {
            return null;
        }
        int pluginLength = payload[38] & 0xff;
        if (pluginLength < 1 || pluginLength > 128 || payload.length < 39 + pluginLength) {
            return null;
        }
        String plugin = new String(payload, 39, pluginLength, StandardCharsets.US_ASCII);
        if (!validPluginId(plugin) || payload.length + 13 > MAX_UDP_BYTES) {
            return null;
        }
        return new Delivery(
                hex(java.util.Arrays.copyOfRange(payload, 4, 12)),
                hex(java.util.Arrays.copyOfRange(payload, 12, 17)),
                hex(java.util.Arrays.copyOfRange(payload, 17, 22)),
                plugin,
                java.util.Arrays.copyOfRange(payload, 22, 38),
                java.util.Arrays.copyOfRange(payload, 39 + pluginLength, payload.length));
    }

    private record Delivery(String network, String sender, String target, String plugin,
            byte[] messageId, byte[] data) {}

    private static final class NetworkState {
        private final String id;
        private String node = "";
        private InetAddress local;
        private InetAddress controller;
        private int controllerPort = NETWORK_PORT;
        private JsonNode members = JsonSupport.mapper().createArrayNode();
        private final Map<String, JsonNode> membersById = new HashMap<>();
        private long membersAt;
        private final Map<String, RelayState> relays = new TreeMap<>();
        private final Map<String, Integer> endpoints = new HashMap<>();
        private final Map<String, Long> endpointAt = new HashMap<>();
        private final Map<String, Binding> bindings = new HashMap<>();
        private Channel socket;
        private boolean refreshing;
        private final List<Consumer<JsonNode>> waiting = new ArrayList<>();

        private NetworkState(String id) {
            this.id = id;
        }
    }

    private static final class RelayState {
        private String nodeId = "";
        private String moonId = "";
        private String virtualIp = "";
        private int port = NETWORK_PORT;
        private double averageLatency = -1;
        private double lastLatency = -1;
        private boolean available;
        private long sampledAt;
        private long bytesPerSecond;
        private long queuedBytes;
        private final ArrayDeque<Double> samples = new ArrayDeque<>();
        private int failures;
        private boolean pending;
        private long reserved;

        private String key() {
            return virtualIp + ":" + port;
        }
    }

    private static final class Binding {
        private String relay = "";
        private long used;
        private int outside;
    }

    private static final class Probe {
        private final String network;
        private final InetAddress address;
        private final long started;
        private final BiConsumer<Double, JsonNode> callback;

        private Probe(String network, InetAddress address, long started,
                BiConsumer<Double, JsonNode> callback) {
            this.network = network;
            this.address = address;
            this.started = started;
            this.callback = callback;
        }
    }

    private static final class SenderProgress {
        private int recipients;
        private int outstanding;
        private final List<String> failedMembers = new ArrayList<>();
    }

    private final class SendJob implements Cancellable {
        private final Consumer<SendResult> callback;
        private final AtomicBoolean cancelled = new AtomicBoolean();
        private final AtomicBoolean finished = new AtomicBoolean();

        private SendJob(Consumer<SendResult> callback) {
            this.callback = callback;
        }

        private void complete(SendResult result) {
            if (!finished.compareAndSet(false, true) || cancelled.get() || callback == null) {
                return;
            }
            deliver(() -> {
                try {
                    callback.accept(result);
                } catch (RuntimeException failure) {
                    log.warn("Network send callback failed: " + failure.getMessage());
                }
            });
        }

        @Override
        public void cancel() {
            cancelled.set(true);
        }
    }

    private final class RelayJob implements Cancellable {
        private final Consumer<RelayResult> callback;
        private final AtomicBoolean cancelled = new AtomicBoolean();
        private final AtomicBoolean finished = new AtomicBoolean();

        private RelayJob(Consumer<RelayResult> callback) {
            this.callback = callback;
        }

        private void complete(RelayResult result) {
            if (!finished.compareAndSet(false, true) || cancelled.get() || callback == null) {
                return;
            }
            deliver(() -> {
                try {
                    callback.accept(result);
                } catch (RuntimeException failure) {
                    log.warn("Network relay callback failed: " + failure.getMessage());
                }
            });
        }

        @Override
        public void cancel() {
            cancelled.set(true);
        }
    }

    private final class BuiltinJob implements Cancellable {
        private final Consumer<ObjectNode> callback;
        private final AtomicBoolean cancelled = new AtomicBoolean();
        private final AtomicBoolean finished = new AtomicBoolean();

        private BuiltinJob(Consumer<ObjectNode> callback) {
            this.callback = callback;
        }

        private void complete(ObjectNode result) {
            if (!finished.compareAndSet(false, true) || cancelled.get() || callback == null) {
                return;
            }
            deliver(() -> {
                try {
                    callback.accept(result);
                } catch (RuntimeException failure) {
                    log.warn("Network builtin callback failed: " + failure.getMessage());
                }
            });
        }

        @Override
        public void cancel() {
            cancelled.set(true);
        }
    }

    private final class Subscription implements NetworkSubscription {
        private final String plugin;
        private final String network;
        private final NetworkCallback callback;
        private volatile boolean cancelled;

        private Subscription(String plugin, String network, NetworkCallback callback) {
            this.plugin = plugin == null ? "" : plugin;
            this.network = network == null ? "" : network.toLowerCase();
            this.callback = callback;
        }

        @Override
        public void cancel() {
            cancelled = true;
            subscriptions.remove(this);
        }
    }
}
