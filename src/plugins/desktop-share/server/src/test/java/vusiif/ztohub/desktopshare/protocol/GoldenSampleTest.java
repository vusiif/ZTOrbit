package vusiif.ztohub.desktopshare.protocol;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class GoldenSampleTest {
    private static final ObjectMapper JSON = new ObjectMapper();
    private static final Path SAMPLES = Path.of("..", "shared", "samples");
    private static final HexFormat HEX = HexFormat.of();

    private static JsonNode load(String name) throws Exception {
        return JSON.readTree(Files.readAllBytes(SAMPLES.resolve(name)));
    }

    private static byte[] bytes(JsonNode sample) {
        if (sample.has("bytes"))
            return sample.get("bytes").textValue().getBytes(StandardCharsets.UTF_8);
        return HEX.parseHex(sample.get("bytesHex").textValue());
    }

    private static List<String> strings(JsonNode array) {
        List<String> result = new ArrayList<>();
        array.forEach(value -> result.add(value.textValue()));
        return result;
    }

    private static void checkFormat(JsonNode expect, ControlProtocol.StreamFormat format) {
        if (expect.has("formatCodec")) assertEquals(expect.get("formatCodec").asText(), format.codec());
        if (expect.has("formatWidth")) assertEquals(expect.get("formatWidth").asInt(), format.width());
        if (expect.has("formatHeight")) assertEquals(expect.get("formatHeight").asInt(), format.height());
        if (expect.has("formatFps")) assertEquals(expect.get("formatFps").asInt(), format.fps());
        if (expect.has("formatBitrate")) assertEquals(expect.get("formatBitrate").asInt(), format.bitrate());
        if (expect.has("formatInit")) assertEquals(expect.get("formatInit").asText(), format.initializationHex());
        if (expect.has("formatEpoch"))
            assertEquals(expect.get("formatEpoch").asText(), Long.toUnsignedString(format.streamEpoch()));
    }

    private static void checkRequest(JsonNode expect, ControlProtocol.Request request) {
        if (expect.has("id")) assertEquals(expect.get("id").asLong(), request.id());
        if (expect.has("op")) assertEquals(expect.get("op").asText(), request.op().wireName());
        if (expect.has("profile")) assertEquals(expect.get("profile").asText(), request.profile());
        if (expect.has("roomCode")) assertEquals(expect.get("roomCode").asText(), request.roomCode());
        if (expect.has("roomId"))
            assertEquals(expect.get("roomId").asText(), Long.toUnsignedString(request.roomId()));
        if (expect.has("streamEpoch"))
            assertEquals(expect.get("streamEpoch").asText(), Long.toUnsignedString(request.streamEpoch()));
        if (expect.has("completePackets"))
            assertEquals(expect.get("completePackets").asText(),
                    Long.toUnsignedString(request.feedback().completePackets()));
        if (expect.has("missingFragments"))
            assertEquals(expect.get("missingFragments").asText(),
                    Long.toUnsignedString(request.feedback().missingFragments()));
        if (expect.has("decodedFrames"))
            assertEquals(expect.get("decodedFrames").asText(),
                    Long.toUnsignedString(request.feedback().decodedFrames()));
        if (expect.has("displayedFrames"))
            assertEquals(expect.get("displayedFrames").asText(),
                    Long.toUnsignedString(request.feedback().displayedFrames()));
        if (expect.has("waiting")) assertEquals(expect.get("waiting").asBoolean(), request.feedback().waiting());
        if (expect.has("receivedBytes"))
            assertEquals(expect.get("receivedBytes").asText(),
                    Long.toUnsignedString(request.feedback().receivedBytes()));
        if (request.format() != null) checkFormat(expect, request.format());
    }

    private static void checkRoom(JsonNode expect, ControlProtocol.RoomInfo room) {
        if (expect.has("roomId"))
            assertEquals(expect.get("roomId").asText(), Long.toUnsignedString(room.roomId()));
        if (expect.has("roomCode")) assertEquals(expect.get("roomCode").asText(), room.roomCode());
        if (expect.has("profile")) assertEquals(expect.get("profile").asText(), room.profile());
        if (expect.has("role")) assertEquals(expect.get("role").asText(), room.role().wireName());
        if (expect.has("token")) assertEquals(expect.get("token").asText(), room.tokenHex());
        if (expect.has("formatWidth")) {
            assertNotNull(room.format());
            checkFormat(expect, room.format());
        }
    }

    private static void checkResponse(JsonNode expect, ControlProtocol.Response response) {
        if (expect.has("id")) assertEquals(expect.get("id").asLong(), response.id());
        if (expect.has("ok")) assertEquals(expect.get("ok").asBoolean(), response.ok());
        if (!response.ok()) {
            if (expect.has("errorCode"))
                assertEquals(expect.get("errorCode").asText(), response.error().wireName());
            if (expect.has("errorMessage"))
                assertEquals(expect.get("errorMessage").asText(), response.message());
            return;
        }
        switch (response.op()) {
            case HELLO -> {
                if (expect.has("sessionId"))
                    assertEquals(expect.get("sessionId").asText(),
                            Long.toUnsignedString(response.hello().sessionId()));
                if (expect.has("udpPort"))
                    assertEquals(expect.get("udpPort").asInt(), response.hello().udpPort());
                if (expect.has("profiles"))
                    assertEquals(strings(expect.get("profiles")), response.hello().profiles());
                if (expect.has("maxViewers"))
                    assertEquals(expect.get("maxViewers").asInt(), response.hello().maxViewers());
            }
            case CREATE, JOIN -> checkRoom(expect, response.room());
            default -> {
                if (expect.has("roomId"))
                    assertEquals(expect.get("roomId").asText(),
                            Long.toUnsignedString(response.roomId()));
                if (expect.has("streamEpoch"))
                    assertEquals(expect.get("streamEpoch").asText(),
                            Long.toUnsignedString(response.streamEpoch()));
            }
        }
    }

    private static void checkEvent(JsonNode expect, ControlProtocol.Event event) {
        if (expect.has("event")) assertEquals(expect.get("event").asText(), event.kind().wireName());
        if (expect.has("roomId"))
            assertEquals(expect.get("roomId").asText(), Long.toUnsignedString(event.roomId()));
        if (expect.has("state")) assertEquals(expect.get("state").asText(), event.roomState().wireName());
        if (expect.has("viewerCount")) assertEquals(expect.get("viewerCount").asInt(), event.viewerCount());
        if (expect.has("reason")) assertEquals(expect.get("reason").asText(), event.reason().wireName());
        if (expect.has("streamEpoch"))
            assertEquals(expect.get("streamEpoch").asText(), Long.toUnsignedString(event.streamEpoch()));
        if (event.format() != null) checkFormat(expect, event.format());
    }

    private static void checkMediaHeader(JsonNode expect, MediaProtocol.Header header) {
        if (expect.has("type")) assertEquals(expect.get("type").asInt(), header.type().wireValue());
        if (expect.has("flags")) assertEquals(expect.get("flags").asInt(), header.flags());
        if (expect.has("token")) assertEquals(expect.get("token").asText(), HEX.formatHex(header.token()));
        if (expect.has("sessionId"))
            assertEquals(expect.get("sessionId").asText(), Long.toUnsignedString(header.sessionId()));
        if (expect.has("roomId"))
            assertEquals(expect.get("roomId").asText(), Long.toUnsignedString(header.roomId()));
        if (expect.has("streamEpoch"))
            assertEquals(expect.get("streamEpoch").asText(), Long.toUnsignedString(header.streamEpoch()));
        if (expect.has("sequence"))
            assertEquals(expect.get("sequence").asText(), Long.toUnsignedString(header.sequence()));
        if (expect.has("pts")) assertEquals(expect.get("pts").asText(), Long.toString(header.pts()));
        if (expect.has("dts")) assertEquals(expect.get("dts").asText(), Long.toString(header.dts()));
        if (expect.has("duration")) assertEquals(expect.get("duration").asLong(), header.duration());
        if (expect.has("packetBytes"))
            assertEquals(expect.get("packetBytes").asText(), Long.toUnsignedString(header.packetBytes()));
        if (expect.has("fragmentIndex")) assertEquals(expect.get("fragmentIndex").asInt(), header.fragmentIndex());
        if (expect.has("fragmentCount")) assertEquals(expect.get("fragmentCount").asInt(), header.fragmentCount());
        if (expect.has("fragmentBytes")) assertEquals(expect.get("fragmentBytes").asInt(), header.fragmentBytes());
    }

    @Test
    void controlGolden() throws Exception {
        JsonNode root = load("control-golden.json");
        assertEquals("zds-control-golden/1", root.get("format").asText());
        JsonNode samples = root.get("valid");
        assertTrue(samples.size() > 0);
        for (JsonNode sample : samples) {
            String name = sample.get("name").asText();
            byte[] bytes = bytes(sample);
            JsonNode expect = sample.get("expect");
            String kind = sample.get("kind").asText();
            switch (kind) {
                case "request" -> {
                    ControlProtocol.Request request = ControlProtocol.decodeRequest(bytes);
                    assertArrayEquals(bytes, ControlProtocol.encodeRequest(request), name);
                    checkRequest(expect, request);
                }
                case "response" -> {
                    ControlProtocol.Operation op =
                            ControlProtocol.Operation.fromWire(sample.get("op").asText());
                    ControlProtocol.Response response = ControlProtocol.decodeResponse(bytes, op);
                    assertArrayEquals(bytes, ControlProtocol.encodeResponse(response), name);
                    checkResponse(expect, response);
                }
                default -> {
                    ControlProtocol.Event event = ControlProtocol.decodeEvent(bytes);
                    assertArrayEquals(bytes, ControlProtocol.encodeEvent(event), name);
                    checkEvent(expect, event);
                }
            }
        }
    }

    @Test
    void controlInvalidGolden() throws Exception {
        JsonNode root = load("control-golden.json");
        for (JsonNode sample : root.get("invalid")) {
            String name = sample.get("name").asText();
            byte[] bytes = bytes(sample);
            String expected = sample.get("error").asText();
            String kind = sample.get("kind").asText();
            ControlProtocol.ProtocolException failure = assertThrows(
                    ControlProtocol.ProtocolException.class, () -> {
                        switch (kind) {
                            case "request" -> ControlProtocol.decodeRequest(bytes);
                            case "response" -> ControlProtocol.decodeResponse(bytes,
                                    ControlProtocol.Operation.fromWire(sample.get("op").asText()));
                            default -> ControlProtocol.decodeEvent(bytes);
                        }
                    }, name);
            assertEquals(expected, failure.code().wireName(), name);
        }
    }

    @Test
    void mediaGolden() throws Exception {
        JsonNode root = load("media-golden.json");
        assertEquals("zds-media-golden/1", root.get("format").asText());
        for (JsonNode sample : root.get("valid")) {
            String name = sample.get("name").asText();
            byte[] bytes = bytes(sample);
            MediaProtocol.Decoded decoded = MediaProtocol.decode(bytes);
            checkMediaHeader(sample.get("expect"), decoded.header());
            int expectedDatagram = sample.get("zts1DatagramBytes").asInt();
            assertEquals(expectedDatagram, MediaProtocol.datagramBytes(bytes.length), name);
            assertTrue(MediaProtocol.fitsDatagram(bytes.length), name);
            switch (decoded.header().type()) {
                case REGISTER -> assertArrayEquals(bytes, MediaProtocol.encodeRegister(
                        decoded.header().token(), decoded.header().sessionId(),
                        decoded.header().roomId()), name);
                case ACK -> assertArrayEquals(bytes, MediaProtocol.encodeAck(
                        decoded.header().token(), decoded.header().sessionId(),
                        decoded.header().roomId()), name);
                case VIDEO -> assertArrayEquals(bytes, MediaProtocol.encodeVideo(
                        decoded.header(), decoded.fragment()), name);
            }
        }
    }

    @Test
    void mediaInvalidGolden() throws Exception {
        JsonNode root = load("media-golden.json");
        for (JsonNode sample : root.get("invalid")) {
            String name = sample.get("name").asText();
            String expected = sample.get("error").asText();
            MediaProtocol.MediaException failure = assertThrows(
                    MediaProtocol.MediaException.class,
                    () -> MediaProtocol.decode(bytes(sample)), name);
            assertEquals(expected, failure.error().wireName(), name);
        }
    }

    @Test
    void controlSizeBoundary() throws Exception {
        String prefix = "{\"v\":1,\"id\":1,\"op\":\"hello\",\"data\":{},\"x\":\"";
        String suffix = "\"}";
        String exact = prefix + "a".repeat(ControlProtocol.MAX_CONTROL_BYTES
                - prefix.length() - suffix.length()) + suffix;
        byte[] bytes = exact.getBytes(StandardCharsets.UTF_8);
        assertEquals(ControlProtocol.MAX_CONTROL_BYTES, bytes.length);
        ControlProtocol.Request request = ControlProtocol.decodeRequest(bytes);
        assertEquals(1, request.id());
        byte[] tooLarge = Arrays.copyOf(bytes, bytes.length + 1);
        ControlProtocol.ProtocolException failure = assertThrows(
                ControlProtocol.ProtocolException.class,
                () -> ControlProtocol.decodeRequest(tooLarge));
        assertEquals(ControlProtocol.ErrorCode.INVALID_REQUEST, failure.code());
    }

    @Test
    void deepNestingRejected() {
        String nested = "{\"v\":1,\"id\":1,\"op\":\"hello\",\"data\":{\"x\":"
                + "[".repeat(70) + "]".repeat(70) + "}}";
        ControlProtocol.ProtocolException failure = assertThrows(
                ControlProtocol.ProtocolException.class,
                () -> ControlProtocol.decodeRequest(nested.getBytes(StandardCharsets.UTF_8)));
        assertEquals(ControlProtocol.ErrorCode.INVALID_REQUEST, failure.code());
    }

    @Test
    void udpFragmentBoundaries() throws Exception {
        byte[] token = HexFormat.of().parseHex("00112233445566778899aabbccddeeff");
        byte[] data = new byte[MediaProtocol.MAX_PACKET_BYTES];
        java.util.Arrays.fill(data, (byte) 0x5a);
        MediaProtocol.Packet packet = new MediaProtocol.Packet(
                MediaProtocol.Flag.KEY_FRAME.mask(), token, 1, 7, 1, 0, 1000, 0, 33333, data);
        List<byte[]> fragments = MediaProtocol.fragmentVideo(packet);
        assertEquals(933, fragments.size());
        MediaProtocol.Header firstHeader = null;
        for (int i = 0; i < fragments.size(); ++i) {
            byte[] fragment = fragments.get(i);
            assertTrue(fragment.length <= MediaProtocol.MAX_PAYLOAD_BYTES);
            int expectedDatagram = i == fragments.size() - 1 ? 19 + 88 + 76 : 1232;
            assertEquals(expectedDatagram, MediaProtocol.datagramBytes(fragment.length));
            MediaProtocol.Decoded decoded = MediaProtocol.decode(fragment);
            assertEquals(i, decoded.header().fragmentIndex());
            assertEquals(933, decoded.header().fragmentCount());
            if (firstHeader == null) firstHeader = decoded.header();
            else assertTrue(MediaProtocol.sameFragmentMetadata(firstHeader, decoded.header()));
        }
        assertNotNull(firstHeader);
        assertEquals(1232, MediaProtocol.datagramBytes(MediaProtocol.MAX_PAYLOAD_BYTES));
        assertFalse(MediaProtocol.fitsDatagram(MediaProtocol.MAX_PAYLOAD_BYTES + 1));
        MediaProtocol.MediaException failure = assertThrows(
                MediaProtocol.MediaException.class,
                () -> MediaProtocol.decode(new byte[MediaProtocol.MAX_PAYLOAD_BYTES + 1]));
        assertEquals(MediaProtocol.Error.TOO_LONG, failure.error());
        MediaProtocol.Header header = new MediaProtocol.Header(
                MediaProtocol.Type.VIDEO, MediaProtocol.Flag.KEY_FRAME.mask(), token, 1, 7, 1, 0,
                0, 0, 0, 1126, 0, 1, 1126);
        MediaProtocol.MediaException oversized = assertThrows(
                MediaProtocol.MediaException.class,
                () -> MediaProtocol.encodeVideo(header, new byte[1126]));
        assertEquals(MediaProtocol.Error.TOO_LONG, oversized.error());
    }

    @Test
    void numericStringsRoundTripWithoutFloatingPoint() throws Exception {
        long maximum = -1L; // 2^64-1 as an unsigned long bit pattern
        ControlProtocol.Request request = ControlProtocol.Request.leave(1, maximum);
        byte[] encoded = ControlProtocol.encodeRequest(request);
        assertTrue(new String(encoded, StandardCharsets.UTF_8).contains("\"18446744073709551615\""));
        ControlProtocol.Request decoded = ControlProtocol.decodeRequest(encoded);
        assertEquals(maximum, decoded.roomId());
        assertEquals(BigInteger.valueOf(2).pow(64).subtract(BigInteger.ONE).toString(),
                Long.toUnsignedString(decoded.roomId()));
    }
}
