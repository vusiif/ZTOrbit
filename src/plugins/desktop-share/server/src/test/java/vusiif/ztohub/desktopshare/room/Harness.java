package vusiif.ztohub.desktopshare.room;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;
import vusiif.ztohub.desktopshare.protocol.MediaProtocol;

/**
 * Test client that enters the controller through the real plugin callbacks and decodes
 * wire responses/events with the shared protocol code.
 */
final class Harness {
    private static final ObjectMapper MAPPER = new ObjectMapper();

    final FakeHost host = new FakeHost();
    final TestClock clock = new TestClock();
    final TestRoomCodes codes = new TestRoomCodes();
    final RoomController controller;

    private final Map<Long, Integer> eventCursors = new HashMap<>();
    private long nextConnection = 1;
    private long nextRequest = 1;

    Harness() {
        this(Tuning.defaults());
    }

    Harness(Tuning tuning) {
        controller = new RoomController(host, codes, clock, tuning);
        controller.start();
    }

    void stop() {
        controller.stop();
    }

    long connect() {
        return connect("10.0.0.1");
    }

    long connect(String address) {
        long id = nextConnection++;
        host.addConnection(id, address);
        return id;
    }

    void close(long connectionId) {
        controller.onConnectionClosed(connectionId);
        host.removeConnection(connectionId);
    }

    InetSocketAddress endpoint(String address, int port) {
        return new InetSocketAddress(address, port);
    }

    void advanceMillis(long millis) {
        clock.advanceMillis(millis);
    }

    void tick() {
        host.runMaintenance();
    }

    long outboundCount(long connectionId) {
        return host.messages(connectionId).size();
    }

    ControlProtocol.Response request(long connectionId, ControlProtocol.Request request) {
        byte[] raw = ControlProtocol.encodeRequest(request);
        if (raw.length == 0) {
            throw new AssertionError("request did not encode: " + request.op());
        }
        controller.onTcpPacket(connectionId, raw);
        byte[] response = findResponse(connectionId, request.id());
        if (response == null) {
            throw new AssertionError("no response for " + request.op() + " id="
                    + request.id());
        }
        try {
            return ControlProtocol.decodeResponse(response, request.op());
        } catch (ControlProtocol.ProtocolException error) {
            throw new AssertionError("invalid response for " + request.op(), error);
        }
    }

    void sendRaw(long connectionId, String json) {
        controller.onTcpPacket(connectionId, json.getBytes(StandardCharsets.UTF_8));
    }

    ControlProtocol.Response hello(long connectionId) {
        return request(connectionId, ControlProtocol.Request.hello(nextRequest()));
    }

    ControlProtocol.Response ping(long connectionId) {
        return request(connectionId, ControlProtocol.Request.ping(nextRequest()));
    }

    ControlProtocol.Response create(long connectionId, String profile) {
        return request(connectionId, ControlProtocol.Request.create(nextRequest(), profile));
    }

    ControlProtocol.Response join(long connectionId, String roomCode) {
        return request(connectionId, ControlProtocol.Request.join(nextRequest(), roomCode));
    }

    ControlProtocol.Response leave(long connectionId, long roomId) {
        return request(connectionId, ControlProtocol.Request.leave(nextRequest(), roomId));
    }

    ControlProtocol.Response stop(long connectionId, long roomId) {
        return request(connectionId, ControlProtocol.Request.stop(nextRequest(), roomId));
    }

    ControlProtocol.Response configure(long connectionId, long roomId, int width, int height,
            int fps, int bitrate, byte[] initializationData) {
        ControlProtocol.StreamFormat format = new ControlProtocol.StreamFormat(
                width, height, fps, bitrate, initializationData, 0, "h264");
        return request(connectionId, ControlProtocol.Request.configure(
                nextRequest(), roomId, format));
    }

    ControlProtocol.Response configure720(long connectionId, long roomId) {
        return configure(connectionId, roomId, 1280, 720, 30, 2_000_000, new byte[0]);
    }

    ControlProtocol.Response configure1080(long connectionId, long roomId) {
        return configure(connectionId, roomId, 1920, 1080, 30, 4_000_000, new byte[0]);
    }

    ControlProtocol.Response ready(long connectionId, long roomId, long epoch) {
        return request(connectionId, ControlProtocol.Request.ready(
                nextRequest(), roomId, epoch));
    }

    ControlProtocol.Response keyframe(long connectionId, long roomId, long epoch) {
        return request(connectionId, ControlProtocol.Request.keyframe(
                nextRequest(), roomId, epoch));
    }

    ControlProtocol.Response feedback(long connectionId, long roomId, long epoch) {
        ControlProtocol.Feedback feedback = new ControlProtocol.Feedback(
                1, 2, 3, 4, false, 5);
        return request(connectionId, ControlProtocol.Request.feedback(
                nextRequest(), roomId, epoch, feedback));
    }

    List<ControlProtocol.Event> events(long connectionId) {
        List<byte[]> messages = host.messages(connectionId);
        int start = eventCursors.getOrDefault(connectionId, 0);
        List<ControlProtocol.Event> events = new ArrayList<>();
        for (int index = start; index < messages.size(); index++) {
            JsonNode node = parse(messages.get(index));
            if (node != null && node.has("event")) {
                try {
                    events.add(ControlProtocol.decodeEvent(messages.get(index)));
                } catch (ControlProtocol.ProtocolException error) {
                    throw new AssertionError("invalid event", error);
                }
            }
        }
        eventCursors.put(connectionId, messages.size());
        return events;
    }

    List<ControlProtocol.Event> eventsOf(long connectionId,
            ControlProtocol.EventKind kind) {
        List<ControlProtocol.Event> result = new ArrayList<>();
        for (ControlProtocol.Event event : events(connectionId)) {
            if (event.kind() == kind) {
                result.add(event);
            }
        }
        return result;
    }

    /** Sends a registration and returns its acknowledgement, or null when rejected. */
    MediaProtocol.Decoded register(long connectionId, long roomId, String tokenHex,
            InetSocketAddress from) {
        return sendRegister(connectionId, connectionId, roomId, tokenHex, from);
    }

    MediaProtocol.Decoded sendRegister(long connectionId, long sessionId, long roomId,
            String tokenHex, InetSocketAddress from) {
        int before = host.udpSends().size();
        try {
            byte[] token = ControlProtocol.tokenFromHex(tokenHex);
            controller.onUdpPacket(from, MediaProtocol.encodeRegister(token, sessionId, roomId));
        } catch (MediaProtocol.MediaException error) {
            throw new AssertionError(error);
        }
        for (int index = before; index < host.udpSends().size(); index++) {
            FakeHost.UdpSend send = host.udpSends().get(index);
            if (!send.endpoint().equals(from)) {
                continue;
            }
            try {
                MediaProtocol.Decoded decoded = MediaProtocol.decode(send.payload());
                if (decoded.header().type() == MediaProtocol.Type.ACK) {
                    return decoded;
                }
            } catch (MediaProtocol.MediaException ignored) {
                // An unrelated malformed datagram cannot be an acknowledgement.
            }
        }
        return null;
    }

    /** Sends one explicit ZDS1 video fragment so tests can drive sequence/fragment state. */
    void sendVideoFragment(long connectionId, long sessionId, long roomId, String tokenHex,
            long epoch, long sequence, InetSocketAddress from, boolean keyFrame,
            int fragmentIndex, int fragmentCount, byte[] data) {
        try {
            byte[] token = ControlProtocol.tokenFromHex(tokenHex);
            int flags = keyFrame ? MediaProtocol.Flag.KEY_FRAME.mask() : 0;
            long packetBytes = (long) (fragmentCount - 1) * MediaProtocol.MAX_FRAGMENT_BYTES
                    + data.length;
            MediaProtocol.Header header = new MediaProtocol.Header(
                    MediaProtocol.Type.VIDEO, flags, token, sessionId, roomId, epoch, sequence,
                    0, 0, 0, packetBytes, fragmentIndex, fragmentCount, data.length);
            controller.onUdpPacket(from, MediaProtocol.encodeVideo(header, data));
        } catch (MediaProtocol.MediaException error) {
            throw new AssertionError(error);
        }
    }

    void sendVideo(long connectionId, long roomId, String tokenHex, long epoch,
            InetSocketAddress from, boolean keyFrame) {
        sendVideo(connectionId, connectionId, roomId, tokenHex, epoch, from, keyFrame);
    }

    void sendVideo(long connectionId, long sessionId, long roomId, String tokenHex, long epoch,
            InetSocketAddress from, boolean keyFrame) {
        try {
            byte[] token = ControlProtocol.tokenFromHex(tokenHex);
            byte[] data = {1, 2, 3, 4};
            int flags = keyFrame ? MediaProtocol.Flag.KEY_FRAME.mask() : 0;
            MediaProtocol.Header header = new MediaProtocol.Header(
                    MediaProtocol.Type.VIDEO, flags, token, sessionId, roomId, epoch, 1,
                    0, 0, 0, data.length, 0, 1, data.length);
            controller.onUdpPacket(from, MediaProtocol.encodeVideo(header, data));
        } catch (MediaProtocol.MediaException error) {
            throw new AssertionError(error);
        }
    }

    int udpSends() {
        return host.udpSends().size();
    }

    /** Decoded UDP payloads sent to one endpoint since a recorded index. */
    List<MediaProtocol.Decoded> udpTo(InetSocketAddress endpoint, int since) {
        List<MediaProtocol.Decoded> result = new ArrayList<>();
        List<FakeHost.UdpSend> sends = host.udpSends();
        for (int index = since; index < sends.size(); index++) {
            FakeHost.UdpSend send = sends.get(index);
            if (!send.endpoint().equals(endpoint)) {
                continue;
            }
            try {
                result.add(MediaProtocol.decode(send.payload()));
            } catch (MediaProtocol.MediaException error) {
                throw new AssertionError("server sent an invalid media payload", error);
            }
        }
        return result;
    }

    RoomController.Stats stats() {
        return controller.stats();
    }

    private long nextRequest() {
        return nextRequest++;
    }

    private byte[] findResponse(long connectionId, long id) {
        List<byte[]> messages = host.messages(connectionId);
        for (int index = messages.size() - 1; index >= 0; index--) {
            JsonNode node = parse(messages.get(index));
            if (node != null && node.has("ok") && node.path("id").asLong(0) == id) {
                return messages.get(index);
            }
        }
        return null;
    }

    private static JsonNode parse(byte[] payload) {
        try {
            return MAPPER.readTree(payload);
        } catch (Exception error) {
            return null;
        }
    }
}
