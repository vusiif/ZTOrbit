package vusiif.ztohub.main.network;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import io.netty.channel.EventLoop;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.Base64;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Assumptions;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import vusiif.ztohub.api.NetworkMessage;
import vusiif.ztohub.api.NetworkSubscription;
import vusiif.ztohub.api.RelayInfo;
import vusiif.ztohub.api.RelayResult;
import vusiif.ztohub.api.SendResult;
import vusiif.ztohub.main.HostLog;
import vusiif.ztohub.main.server.Wire;

/**
 * End-to-end transport test against an in-process fake controller. A single server transport talks
 * to the fake controller over the real ZTS1/builtin TCP protocol and to a UDP receiver on
 * 127.0.0.2:18101 for forwarding.
 */
class NetworkTransportTest {
    private static final ObjectMapper JSON = new ObjectMapper();
    private static final String NETWORK = "0123456789abcdef";
    private static final String SELF = "1111111111";
    private static final String PEER = "2222222222";
    private static final int CLIENT_PORT = 18101;

    @TempDir
    Path root;

    private EventLoopGroup hostGroup;
    private EventLoop hostLoop;
    private HostLog log;
    private FakeController controller;
    private NetworkTransport transport;
    private DatagramSocket probeSocket;
    private DatagramSocket peerReceiver;
    private Thread probeThread;
    private Thread forwardThread;

    @BeforeEach
    void setUp() throws Exception {
        hostGroup = new NioEventLoopGroup(1,
                (java.util.concurrent.ThreadFactory) runnable -> new Thread(runnable, "zt-test-host"));
        hostLoop = hostGroup.next();
        log = new HostLog(root.resolve("host"));
        controller = new FakeController();
        transport = new NetworkTransport(true, hostLoop, log, root);
        transport.setNetworks(SELF, JSON.readTree("[{\"nwid\":\"" + NETWORK
                + "\",\"assignedAddresses\":[\"127.0.0.1/24\"],"
                + "\"transportController\":\"127.0.0.1\",\"transportControllerPort\":"
                + controller.port() + "}]"));
        awaitMembers();
    }

    @AfterEach
    void tearDown() throws Exception {
        if (transport != null) {
            transport.close();
        }
        if (probeSocket != null) {
            probeSocket.close();
        }
        if (peerReceiver != null) {
            peerReceiver.close();
        }
        if (controller != null) {
            controller.close();
        }
        log.close();
        hostGroup.shutdownGracefully(0, 5, TimeUnit.SECONDS).syncUninterruptibly();
    }

    private void awaitMembers() throws Exception {
        RelayResult last = null;
        for (int attempt = 0; attempt < 100; attempt++) {
            last = relays();
            if (last.ok() && !last.relays().isEmpty()) {
                return;
            }
            Thread.sleep(50);
        }
        throw new AssertionError("transport did not load members/relays in time: " + last);
    }

    private RelayResult relays() throws Exception {
        CompletableFuture<RelayResult> future = new CompletableFuture<>();
        transport.relays(NETWORK, future::complete);
        return future.get(10, TimeUnit.SECONDS);
    }

    private ObjectNode builtin(InetAddress source, String operation, ObjectNode data) throws Exception {
        ObjectNode request = JSON.createObjectNode();
        request.put("v", 1);
        request.put("id", 1);
        request.put("op", operation);
        request.set("data", data);
        CompletableFuture<ObjectNode> future = new CompletableFuture<>();
        transport.builtin(source, request, future::complete);
        return future.get(20, TimeUnit.SECONDS);
    }

    private ObjectNode requestData() {
        ObjectNode data = JSON.createObjectNode();
        data.put("networkId", NETWORK);
        data.put("senderId", SELF);
        return data;
    }

    @Test
    void rejectsRelaysFromNonMember() throws Exception {
        ObjectNode data = requestData();
        JsonNode response = builtin(InetAddress.getByName("127.0.0.5"), "network.relays", data);
        assertFalse(response.path("ok").asBoolean());
        assertEquals("unauthorized", response.path("error").asText());
    }

    @Test
    void registersEndpointAndReturnsRelays() throws Exception {
        ObjectNode register = requestData();
        register.put("port", CLIENT_PORT);
        JsonNode registered = builtin(InetAddress.getByName("127.0.0.1"), "network.register",
                register);
        assertTrue(registered.path("ok").asBoolean(), registered.toString());

        ObjectNode data = requestData();
        JsonNode relays = builtin(InetAddress.getByName("127.0.0.1"), "network.relays", data);
        assertTrue(relays.path("ok").asBoolean(), relays.toString());
        assertEquals(CLIENT_PORT, relays.path("data").path("endpoints").path(SELF).asInt());
        assertEquals(1, relays.path("data").path("relays").size());
        assertEquals("127.0.0.1", relays.path("data").path("relays").get(0)
                .path("virtualIp").asText());
    }

    @Test
    void rejectsInvalidRegisterPortAndUnknownOperation() throws Exception {
        ObjectNode register = requestData();
        register.put("port", 1234);
        JsonNode response = builtin(InetAddress.getByName("127.0.0.1"), "network.register", register);
        assertEquals("invalid_port", response.path("error").asText());
        JsonNode unknown = builtin(InetAddress.getByName("127.0.0.1"), "network.unknown",
                requestData());
        assertEquals("unknown_operation", unknown.path("error").asText());
        JsonNode missing = builtin(InetAddress.getByName("127.0.0.1"), "network.forward",
                JSON.createObjectNode());
        assertEquals("network_unavailable", missing.path("error").asText());
    }

    @Test
    void forwardsZtd1ToMemberEndpoint() throws Exception {
        InetAddress target = InetAddress.getByName("127.0.0.2");
        try {
            peerReceiver = new DatagramSocket(new InetSocketAddress(target, CLIENT_PORT));
        } catch (IOException occupied) {
            Assumptions.assumeTrue(false, "127.0.0.2:18101 is not available");
        }
        peerReceiver.setSoTimeout(5000);

        ObjectNode register = requestData();
        register.put("port", CLIENT_PORT);
        builtin(InetAddress.getByName("127.0.0.1"), "network.register", register);

        byte[] messageId = new byte[16];
        new java.util.Random(11).nextBytes(messageId);
        String payload = Base64.getEncoder().encodeToString("voice".getBytes(StandardCharsets.UTF_8));
        ObjectNode forward = requestData();
        forward.put("pluginId", "voice-chat");
        forward.put("messageId", NetworkProtocol.hex(messageId));
        forward.put("payload", payload);
        forward.putArray("members").add(PEER);
        JsonNode response = builtin(InetAddress.getByName("127.0.0.1"), "network.forward", forward);
        assertTrue(response.path("ok").asBoolean(), response.toString());
        assertEquals(1, response.path("data").path("recipients").asInt());

        DatagramPacket datagram = new DatagramPacket(new byte[1500], 1500);
        peerReceiver.receive(datagram);
        Wire.Packet outer = Wire.decode(java.util.Arrays.copyOf(datagram.getData(),
                datagram.getLength()));
        assertNotNull(outer);
        assertEquals("builtin", outer.route());
        NetworkProtocol.Delivery delivery = NetworkProtocol.decodeDelivery(outer.payload());
        assertNotNull(delivery);
        assertEquals(NETWORK, delivery.network());
        assertEquals(SELF, delivery.sender());
        assertEquals(PEER, delivery.target());
        assertEquals("voice-chat", delivery.plugin());
        assertEquals("voice", new String(delivery.data(), StandardCharsets.UTF_8));
    }

    @Test
    void sendApiUsesSingleRelayAndReturnsRecipients() throws Exception {
        CompletableFuture<SendResult> future = new CompletableFuture<>();
        transport.send("voice-chat", NETWORK, List.of(PEER),
                "audio".getBytes(StandardCharsets.UTF_8), false, future::complete);
        SendResult result = future.get(20, TimeUnit.SECONDS);
        assertTrue(result.ok(), result.error());
        assertEquals(1, result.recipients());
        assertEquals(List.of(), result.failedMembers());
    }

    @Test
    void subscriptionReceivesAuthorizedDeliveryOnly() throws Exception {
        java.util.concurrent.atomic.AtomicInteger deliveries =
                new java.util.concurrent.atomic.AtomicInteger();
        AtomicReference<NetworkMessage> message = new AtomicReference<>();
        NetworkSubscription subscription = transport.subscribe("voice-chat", NETWORK, value -> {
            message.set(value);
            deliveries.incrementAndGet();
        });
        assertNotNull(subscription);

        NetworkProtocol.Delivery delivery = new NetworkProtocol.Delivery(
                NETWORK, PEER, SELF, "voice-chat",
                new byte[16], "hello".getBytes(StandardCharsets.UTF_8));
        byte[] encoded = NetworkProtocol.encodeDelivery(NETWORK, PEER, SELF, "voice-chat",
                delivery.messageId(), delivery.data());
        Wire.Packet outer = Wire.decode(encoded);
        assertNotNull(outer);

        transport.datagram(InetAddress.getByName("127.0.0.5"), 4000, outer.payload());
        Thread.sleep(300);
        assertEquals(0, deliveries.get());

        transport.datagram(InetAddress.getByName("127.0.0.1"), 4000, outer.payload());
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (deliveries.get() == 0 && System.nanoTime() < deadline) {
            Thread.sleep(20);
        }
        assertEquals(1, deliveries.get());
        assertNotNull(message.get());
        assertEquals(NETWORK, message.get().networkId());
        assertEquals(PEER, message.get().senderId());
        assertEquals("hello", new String(message.get().data(), StandardCharsets.UTF_8));

        transport.datagram(InetAddress.getByName("127.0.0.1"), 4000, outer.payload());
        Thread.sleep(300);
        assertEquals(1, deliveries.get(), "deduplicated delivery");
        subscription.cancel();
    }

    @Test
    void probeMakesRelayAvailable() throws Exception {
        java.util.concurrent.atomic.AtomicInteger probeRequests =
                new java.util.concurrent.atomic.AtomicInteger();
        probeSocket = new DatagramSocket(new InetSocketAddress(
                InetAddress.getByName("127.0.0.1"), controller.port()));
        probeThread = new Thread(() -> {
            byte[] buffer = new byte[1500];
            while (!probeSocket.isClosed()) {
                try {
                    DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                    probeSocket.receive(packet);
                    Wire.Packet outer = Wire.decode(java.util.Arrays.copyOf(
                            packet.getData(), packet.getLength()));
                    if (outer == null) {
                        continue;
                    }
                    JsonNode request = JSON.readTree(outer.payload());
                    if (!"network.probe".equals(request.path("op").asText())
                            || request.path("reply").asBoolean(false)
                            || !SELF.equals(request.path("senderId").asText())) {
                        continue;
                    }
                    probeRequests.incrementAndGet();
                    ObjectNode reply = ((ObjectNode) request).deepCopy();
                    reply.put("reply", true);
                    reply.put("bytesPerSecond", 4096);
                    reply.put("queuedBytes", 0);
                    byte[] response = Wire.encode("builtin",
                            JSON.writeValueAsBytes(reply));
                    probeSocket.send(new DatagramPacket(response, response.length,
                            packet.getSocketAddress()));
                } catch (IOException ignored) {
                    return;
                }
            }
        }, "fake-probe");
        probeThread.setDaemon(true);
        probeThread.start();

        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        boolean available = false;
        while (System.nanoTime() < deadline) {
            RelayResult result = relays();
            for (RelayInfo relay : result.relays()) {
                if (relay.available() && relay.sampledAt() > 0) {
                    available = true;
                }
            }
            if (available) {
                break;
            }
            Thread.sleep(250);
        }
        assertTrue(available, "probe should mark the fallback relay available; requests="
                + probeRequests.get() + "; " + debugLog());
    }

    private String debugLog() {
        try {
            Path path = Path.of(log.path());
            if (java.nio.file.Files.isRegularFile(path)) {
                return java.nio.file.Files.readString(path);
            }
        } catch (IOException ignored) {
            // Fall through to an empty diagnostic.
        }
        return "no host log";
    }

    private final class FakeController implements AutoCloseable {
        private final ServerSocket server;
        private final Thread acceptThread;
        private final java.util.Set<Socket> sockets = ConcurrentHashMap.newKeySet();

        private FakeController() throws IOException {
            server = new ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"));
            acceptThread = new Thread(this::acceptLoop, "fake-controller");
            acceptThread.setDaemon(true);
            acceptThread.start();
        }

        private int port() {
            return server.getLocalPort();
        }

        private void acceptLoop() {
            while (!server.isClosed()) {
                try {
                    Socket socket = server.accept();
                    sockets.add(socket);
                    Thread worker = new Thread(() -> handle(socket), "fake-controller-rpc");
                    worker.setDaemon(true);
                    worker.start();
                } catch (IOException closed) {
                    return;
                }
            }
        }

        private void handle(Socket socket) {
            try (socket) {
                DataInputStream input = new DataInputStream(socket.getInputStream());
                OutputStream output = socket.getOutputStream();
                while (true) {
                    int length;
                    try {
                        length = input.readInt();
                    } catch (IOException end) {
                        return;
                    }
                    if (length < 7 || length > 1024 * 1024) {
                        return;
                    }
                    byte[] packet = new byte[length];
                    input.readFully(packet);
                    Wire.Packet parsed = Wire.decode(packet);
                    if (parsed == null) {
                        return;
                    }
                    JsonNode request = JSON.readTree(parsed.payload());
                    ObjectNode response = respond(request);
                    byte[] framed = Wire.frame("builtin", JSON.writeValueAsBytes(response));
                    output.write(framed);
                    output.flush();
                }
            } catch (IOException ignored) {
                // Connection ends when the RPC completes.
            } finally {
                sockets.remove(socket);
            }
        }

        private ObjectNode respond(JsonNode request) {
            ObjectNode response = JSON.createObjectNode();
            response.put("v", 1);
            response.put("id", request.path("id").asInt(1));
            response.put("ok", true);
            String operation = request.path("op").asText();
            switch (operation) {
                case "controller.members" -> {
                    var members = response.putArray("data");
                    members.addObject().put("nodeId", "0123456789").put("name", "controller")
                            .put("authorized", true).putArray("ipAssignments").add("127.0.0.1/24");
                    members.addObject().put("nodeId", SELF).put("name", "self")
                            .put("authorized", true).putArray("ipAssignments").add("127.0.0.1/24");
                    members.addObject().put("nodeId", PEER).put("name", "peer")
                            .put("authorized", true).putArray("ipAssignments").add("127.0.0.2/24");
                }
                case "network.relays" -> {
                    ObjectNode data = response.putObject("data");
                    data.putArray("relays");
                    data.putObject("endpoints");
                }
                case "network.register" -> response.putObject("data");
                case "network.forward" -> {
                    ObjectNode data = response.putObject("data");
                    data.put("recipients", request.path("data").path("members").size());
                    data.putArray("failedMembers");
                }
                default -> {
                    response.put("ok", false);
                    response.put("error", "unknown_operation");
                }
            }
            return response;
        }

        @Override
        public void close() throws IOException {
            server.close();
            for (Socket socket : sockets) {
                try {
                    socket.close();
                } catch (IOException ignored) {
                    // Best effort during teardown.
                }
            }
        }
    }
}
