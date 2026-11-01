package vusiif.ztohub.desktopshare.protocol;

import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

class ControlErrorTest {
    @Test
    void peekRequestIdAcceptsOnlyRequestFrames() {
        assertEquals(7, ControlProtocol.peekRequestId(bytes(
                "{\"v\":1,\"id\":7,\"op\":\"ping\",\"data\":{}}")));
        assertEquals(8, ControlProtocol.peekRequestId(bytes(
                "{\"v\":1,\"id\":8,\"op\":\"unknown\",\"data\":{}}")));
        assertEquals(9, ControlProtocol.peekRequestId(bytes(
                "{\"v\":99,\"id\":9,\"op\":\"ping\",\"data\":{}}")));
        assertEquals(0, ControlProtocol.peekRequestId(bytes(
                "{\"v\":1,\"id\":10,\"ok\":false,\"error\":{\"code\":\"INTERNAL\","
                        + "\"message\":\"x\"}}")));
        assertEquals(0, ControlProtocol.peekRequestId(bytes(
                "{\"v\":1,\"event\":\"room.closed\",\"data\":{\"roomId\":\"1\","
                        + "\"reason\":\"owner_stopped\"}}")));
        assertEquals(0, ControlProtocol.peekRequestId(bytes("{\"v\":1,\"id\":0,"
                + "\"op\":\"ping\",\"data\":{}}")));
        assertEquals(0, ControlProtocol.peekRequestId(bytes(
                "{\"v\":1,\"id\":4294967296,\"op\":\"ping\",\"data\":{}}")));
        assertEquals(0, ControlProtocol.peekRequestId(bytes("{not json")));
        assertEquals(0, ControlProtocol.peekRequestId(new byte[0]));
    }

    @Test
    void errorWithoutOperationDecodesAsNormalFailure() throws Exception {
        byte[] encoded = ControlProtocol.encodeError(11,
                ControlProtocol.ErrorCode.UNKNOWN_OPERATION, "未知操作");
        ControlProtocol.Response response = ControlProtocol.decodeResponse(encoded,
                ControlProtocol.Operation.PING);
        assertFalse(response.ok());
        assertEquals(11, response.id());
        assertEquals(ControlProtocol.ErrorCode.UNKNOWN_OPERATION, response.error());
        assertEquals("未知操作", response.message());
    }

    @Test
    void unknownOperationStillYieldsAnAnswerableRequestId() {
        byte[] payload = bytes(
                "{\"v\":1,\"id\":12,\"op\":\"room.enumerate\",\"data\":{}}");
        assertEquals(12, ControlProtocol.peekRequestId(payload));
        ControlProtocol.ProtocolException error = org.junit.jupiter.api.Assertions.assertThrows(
                ControlProtocol.ProtocolException.class,
                () -> ControlProtocol.decodeRequest(payload));
        assertEquals(ControlProtocol.ErrorCode.UNKNOWN_OPERATION, error.code());
    }

    @Test
    void streamFormatEpochCopyKeepsPayloadAndDimensions() {
        byte[] initialization = {9, 8, 7};
        ControlProtocol.StreamFormat original = new ControlProtocol.StreamFormat(
                1280, 720, 30, 2_000_000, initialization, 0, "h264");
        ControlProtocol.StreamFormat withEpoch = original.withStreamEpoch(4);
        assertEquals(4, withEpoch.streamEpoch());
        assertEquals(original.width(), withEpoch.width());
        assertEquals(original.height(), withEpoch.height());
        assertEquals(original.fps(), withEpoch.fps());
        assertEquals(original.bitrate(), withEpoch.bitrate());
        assertArrayEquals(initialization, withEpoch.initializationData());
        assertTrue(ControlProtocol.encodeRequest(ControlProtocol.Request.configure(1, 2,
                withEpoch)).length > 0);
    }

    private static byte[] bytes(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }
}
