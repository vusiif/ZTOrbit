package vusiif.ztgui.voicechat;

import java.util.Arrays;

/**
 * ZTV1 voice protocol: 30-byte header (magic, version, type, 16-byte token, sender, sequence)
 * followed by an optional audio payload. Must stay byte-compatible with
 * {@code src/plugins/voice-chat/shared/Protocol.h} and the desktop client.
 */
public final class VoiceProtocol {
    public static final int VERSION = 1;
    public static final int MAX_CONTROL_BYTES = 65_536;
    public static final int MAX_DATAGRAM_BYTES = 1200;
    public static final int HEADER_BYTES = 30;
    public static final int FRAME_SAMPLES = 960;
    public static final int REGISTER = 1;
    public static final int REGISTERED = 2;
    public static final int AUDIO = 3;

    private VoiceProtocol() {}

    public record Packet(int type, byte[] token, long sender, long sequence, byte[] payload) {
        public Packet {
            token = token == null ? new byte[0] : Arrays.copyOf(token, token.length);
            payload = payload == null ? new byte[0] : Arrays.copyOf(payload, payload.length);
        }
    }

    /** Encodes one datagram, or returns {@code null} when the packet is invalid. */
    public static byte[] encode(Packet packet) {
        if (packet == null || packet.token().length != 16
                || packet.payload().length > MAX_DATAGRAM_BYTES - HEADER_BYTES) {
            return null;
        }
        byte[] data = new byte[HEADER_BYTES + packet.payload().length];
        data[0] = 'Z';
        data[1] = 'T';
        data[2] = 'V';
        data[3] = '1';
        data[4] = (byte) VERSION;
        data[5] = (byte) packet.type();
        System.arraycopy(packet.token(), 0, data, 6, 16);
        putInt(data, 22, packet.sender());
        putInt(data, 26, packet.sequence());
        System.arraycopy(packet.payload(), 0, data, HEADER_BYTES, packet.payload().length);
        return data;
    }

    /** Decodes one datagram, or returns {@code null} when it is invalid. */
    public static Packet decode(byte[] data) {
        if (data == null || data.length < HEADER_BYTES || data.length > MAX_DATAGRAM_BYTES
                || data[0] != 'Z' || data[1] != 'T' || data[2] != 'V' || data[3] != '1'
                || (data[4] & 0xff) != VERSION) {
            return null;
        }
        int type = data[5] & 0xff;
        if (type < REGISTER || type > AUDIO) {
            return null;
        }
        byte[] token = Arrays.copyOfRange(data, 6, 22);
        long sender = readInt(data, 22);
        long sequence = readInt(data, 26);
        byte[] payload = Arrays.copyOfRange(data, HEADER_BYTES, data.length);
        if (type == AUDIO ? payload.length == 0 : payload.length != 0) {
            return null;
        }
        return new Packet(type, token, sender, sequence, payload);
    }

    private static void putInt(byte[] data, int offset, long value) {
        data[offset] = (byte) (value >>> 24);
        data[offset + 1] = (byte) (value >>> 16);
        data[offset + 2] = (byte) (value >>> 8);
        data[offset + 3] = (byte) value;
    }

    private static long readInt(byte[] data, int offset) {
        return ((long) (data[offset] & 0xff) << 24)
                | ((long) (data[offset + 1] & 0xff) << 16)
                | ((long) (data[offset + 2] & 0xff) << 8)
                | (data[offset + 3] & 0xff);
    }
}
