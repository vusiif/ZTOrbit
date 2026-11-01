package vusiif.ztohub.main.server;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import org.junit.jupiter.api.Test;

class WireTest {
    @Test
    void idsFollowTheProtocolAlphabet() {
        assertTrue(Wire.validId("a"));
        assertTrue(Wire.validId("voice-chat"));
        assertTrue(Wire.validId("A_1-2"));
        assertTrue(Wire.validId("x".repeat(128)));
        assertFalse(Wire.validId(""));
        assertFalse(Wire.validId("x".repeat(129)));
        assertFalse(Wire.validId("bad.id"));
        assertFalse(Wire.validId("bad id"));
        assertFalse(Wire.validId("builtin\n"));
        assertFalse(Wire.validId((String) null));
    }

    @Test
    void encodeAndDecodeRoundTrip() {
        byte[] payload = {0, 1, 2, (byte) 0xff};
        byte[] encoded = Wire.encode("test-echo", payload);
        assertNotNull(encoded);
        assertEquals("ZTS1", new String(encoded, 0, 4, StandardCharsets.US_ASCII));
        assertEquals(1, encoded[4]);
        assertEquals(9, encoded[5]);
        Wire.Packet packet = Wire.decode(encoded);
        assertNotNull(packet);
        assertEquals("test-echo", packet.route());
        assertArrayEquals(payload, packet.payload());
    }

    @Test
    void emptyPayloadsAreValid() {
        Wire.Packet packet = Wire.decode(Wire.encode("x", new byte[0]));
        assertNotNull(packet);
        assertEquals(0, packet.payload().length);
    }

    @Test
    void decodeRejectsInvalidPackets() {
        byte[] good = Wire.encode("route", new byte[] {1});
        assertNull(Wire.decode(null));
        assertNull(Wire.decode(new byte[0]));
        assertNull(Wire.decode(Arrays.copyOf(good, 6)));
        byte[] wrongMagic = good.clone();
        wrongMagic[0] = 'X';
        assertNull(Wire.decode(wrongMagic));
        byte[] wrongVersion = good.clone();
        wrongVersion[4] = 2;
        assertNull(Wire.decode(wrongVersion));
        byte[] wrongLength = good.clone();
        wrongLength[5] = 0;
        assertNull(Wire.decode(wrongLength));
        byte[] badRoute = Arrays.copyOf(good, good.length);
        badRoute[6] = '.';
        assertNull(Wire.decode(badRoute));
        assertNull(Wire.decode(Arrays.copyOf(good, 8)));
    }

    @Test
    void encodeHonoursTcpAndRouteLimits() {
        assertNull(Wire.encode("bad.route", new byte[0]));
        int allowed = Wire.MAX_TCP_BYTES - 6 - 1;
        assertNotNull(Wire.encode("r", new byte[allowed]));
        assertNull(Wire.encode("r", new byte[allowed + 1]));
    }

    @Test
    void frameAddsBigEndianLengthPrefix() {
        byte[] framed = Wire.frame("x", new byte[] {7});
        assertNotNull(framed);
        int length = ((framed[0] & 0xff) << 24) | ((framed[1] & 0xff) << 16)
                | ((framed[2] & 0xff) << 8) | (framed[3] & 0xff);
        assertEquals(framed.length - 4, length);
        Wire.Packet packet = Wire.decode(Arrays.copyOfRange(framed, 4, framed.length));
        assertNotNull(packet);
        assertEquals("x", packet.route());
    }
}
