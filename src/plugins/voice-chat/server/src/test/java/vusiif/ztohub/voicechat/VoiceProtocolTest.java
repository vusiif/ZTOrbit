package vusiif.ztohub.voicechat;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import org.junit.jupiter.api.Test;

class VoiceProtocolTest {
    private static final byte[] TOKEN = new byte[16];

    static {
        for (int index = 0; index < TOKEN.length; index++) {
            TOKEN[index] = (byte) index;
        }
    }

    @Test
    void encodesHeaderAndPayload() {
        VoiceProtocol.Packet packet = new VoiceProtocol.Packet(
                VoiceProtocol.AUDIO, TOKEN, 0x01020304L, 0x05060708L,
                "opus".getBytes(StandardCharsets.UTF_8));
        byte[] encoded = VoiceProtocol.encode(packet);
        assertNotNull(encoded);
        assertEquals(30 + 4, encoded.length);
        assertArrayEquals(new byte[] {'Z', 'T', 'V', '1', 1, 3}, Arrays.copyOf(encoded, 6));
        assertArrayEquals(TOKEN, Arrays.copyOfRange(encoded, 6, 22));
        assertArrayEquals(new byte[] {1, 2, 3, 4}, Arrays.copyOfRange(encoded, 22, 26));
        assertArrayEquals(new byte[] {5, 6, 7, 8}, Arrays.copyOfRange(encoded, 26, 30));
        assertArrayEquals("opus".getBytes(StandardCharsets.UTF_8),
                Arrays.copyOfRange(encoded, 30, encoded.length));
    }

    @Test
    void decodesIntoEqualPacket() {
        VoiceProtocol.Packet original = new VoiceProtocol.Packet(
                VoiceProtocol.REGISTER, TOKEN, 42, 7, new byte[0]);
        VoiceProtocol.Packet decoded = VoiceProtocol.decode(VoiceProtocol.encode(original));
        assertNotNull(decoded);
        assertEquals(VoiceProtocol.REGISTER, decoded.type());
        assertArrayEquals(TOKEN, decoded.token());
        assertEquals(42, decoded.sender());
        assertEquals(7, decoded.sequence());
        assertEquals(0, decoded.payload().length);
    }

    @Test
    void handlesMaximumDatagram() {
        byte[] payload = new byte[VoiceProtocol.MAX_DATAGRAM_BYTES - VoiceProtocol.HEADER_BYTES];
        java.util.Arrays.fill(payload, (byte) 0x5a);
        byte[] encoded = VoiceProtocol.encode(new VoiceProtocol.Packet(
                VoiceProtocol.AUDIO, TOKEN, 1, 2, payload));
        assertNotNull(encoded);
        assertEquals(VoiceProtocol.MAX_DATAGRAM_BYTES, encoded.length);
        assertNotNull(VoiceProtocol.decode(encoded));
        assertNull(VoiceProtocol.encode(new VoiceProtocol.Packet(
                VoiceProtocol.AUDIO, TOKEN, 1, 2, new byte[payload.length + 1])));
    }

    @Test
    void rejectsInvalidPackets() {
        assertNull(VoiceProtocol.encode(null));
        assertNull(VoiceProtocol.encode(new VoiceProtocol.Packet(
                VoiceProtocol.AUDIO, new byte[15], 1, 2, new byte[0])));
        assertNull(VoiceProtocol.decode(null));
        assertNull(VoiceProtocol.decode(new byte[0]));
        assertNull(VoiceProtocol.decode(new byte[VoiceProtocol.HEADER_BYTES - 1]));
        byte[] invalidMagic = VoiceProtocol.encode(new VoiceProtocol.Packet(
                VoiceProtocol.REGISTER, TOKEN, 1, 2, new byte[0]));
        invalidMagic[0] = 'X';
        assertNull(VoiceProtocol.decode(invalidMagic));
        byte[] invalidVersion = VoiceProtocol.encode(new VoiceProtocol.Packet(
                VoiceProtocol.REGISTER, TOKEN, 1, 2, new byte[0]));
        invalidVersion[4] = 2;
        assertNull(VoiceProtocol.decode(invalidVersion));
        byte[] invalidType = VoiceProtocol.encode(new VoiceProtocol.Packet(
                VoiceProtocol.REGISTER, TOKEN, 1, 2, new byte[0]));
        invalidType[5] = 9;
        assertNull(VoiceProtocol.decode(invalidType));
        byte[] registerWithPayload = VoiceProtocol.encode(new VoiceProtocol.Packet(
                VoiceProtocol.REGISTER, TOKEN, 1, 2, new byte[] {1}));
        assertNull(VoiceProtocol.decode(registerWithPayload));
        VoiceProtocol.Packet audioWithoutPayload = new VoiceProtocol.Packet(
                VoiceProtocol.AUDIO, TOKEN, 1, 2, new byte[0]);
        assertNull(VoiceProtocol.decode(VoiceProtocol.encode(audioWithoutPayload)));
        assertNull(VoiceProtocol.decode(new byte[VoiceProtocol.MAX_DATAGRAM_BYTES + 1]));
    }
}
