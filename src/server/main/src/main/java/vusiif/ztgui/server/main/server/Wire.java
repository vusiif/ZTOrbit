package vusiif.ztgui.server.main.server;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * ZTS1 wire protocol: version 1, 1-byte route length plus route, then the plugin payload. TCP
 * transports each packet with a 4-byte big-endian length prefix; UDP carries exactly one packet
 * per datagram.
 */
public final class Wire {
    public static final byte[] MAGIC = {'Z', 'T', 'S', '1'};
    public static final int VERSION = 1;
    public static final int MAX_TCP_BYTES = 1024 * 1024;
    public static final int MAX_UDP_BYTES = 1232;
    public static final int HEADER_BYTES = 6;
    public static final String BUILTIN = "builtin";

    private Wire() {}

    public record Packet(String route, byte[] payload) {
        public Packet {
            if (route == null || payload == null) {
                throw new IllegalArgumentException("route and payload are required");
            }
        }
    }

    public static boolean validId(String id) {
        if (id == null || id.isEmpty() || id.length() > 128) {
            return false;
        }
        for (int index = 0; index < id.length(); index++) {
            char character = id.charAt(index);
            boolean valid = (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '_'
                    || character == '-';
            if (!valid) {
                return false;
            }
        }
        return true;
    }

    public static boolean validId(byte[] id) {
        if (id == null || id.length < 1 || id.length > 128) {
            return false;
        }
        for (byte value : id) {
            int character = value & 0xff;
            boolean valid = (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '_'
                    || character == '-';
            if (!valid) {
                return false;
            }
        }
        return true;
    }

    /** Encodes a packet, or returns {@code null} when the route or payload is invalid. */
    public static byte[] encode(String route, byte[] payload) {
        if (!validId(route) || payload == null || payload.length > MAX_TCP_BYTES - 6 - route.length()) {
            return null;
        }
        byte[] routeBytes = route.getBytes(StandardCharsets.US_ASCII);
        byte[] packet = new byte[6 + routeBytes.length + payload.length];
        System.arraycopy(MAGIC, 0, packet, 0, MAGIC.length);
        packet[4] = (byte) VERSION;
        packet[5] = (byte) routeBytes.length;
        System.arraycopy(routeBytes, 0, packet, 6, routeBytes.length);
        System.arraycopy(payload, 0, packet, 6 + routeBytes.length, payload.length);
        return packet;
    }

    /** Decodes one packet, or returns {@code null} when it is not a valid ZTS1 packet. */
    public static Packet decode(byte[] bytes) {
        if (bytes == null || bytes.length < 7 || bytes.length > MAX_TCP_BYTES) {
            return null;
        }
        if (bytes[0] != MAGIC[0] || bytes[1] != MAGIC[1] || bytes[2] != MAGIC[2] || bytes[3] != MAGIC[3]) {
            return null;
        }
        if ((bytes[4] & 0xff) != VERSION) {
            return null;
        }
        int routeLength = bytes[5] & 0xff;
        if (routeLength < 1 || routeLength > 128 || bytes.length < 6 + routeLength) {
            return null;
        }
        byte[] routeBytes = Arrays.copyOfRange(bytes, 6, 6 + routeLength);
        if (!validId(routeBytes)) {
            return null;
        }
        String route = new String(routeBytes, StandardCharsets.US_ASCII);
        return new Packet(route, Arrays.copyOfRange(bytes, 6 + routeLength, bytes.length));
    }

    /** Encodes a full TCP frame (4-byte length prefix plus ZTS1 packet), or null. */
    public static byte[] frame(String route, byte[] payload) {
        byte[] packet = encode(route, payload);
        if (packet == null) {
            return null;
        }
        byte[] framed = new byte[4 + packet.length];
        framed[0] = (byte) (packet.length >>> 24);
        framed[1] = (byte) (packet.length >>> 16);
        framed[2] = (byte) (packet.length >>> 8);
        framed[3] = (byte) packet.length;
        System.arraycopy(packet, 0, framed, 4, packet.length);
        return framed;
    }
}
