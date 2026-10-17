package vusiif.ztgui.server.main.network;

import com.fasterxml.jackson.databind.JsonNode;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Base64;
import vusiif.ztgui.server.main.server.Wire;

/**
 * Network relay protocol helpers: IDs, member addresses, port constants and ZTD1 delivery
 * encoding. Mirrors {@code src/shared/include/ztprotocol/NetworkDelivery.h} and
 * {@code NetworkAddress.h}.
 */
public final class NetworkProtocol {
    public static final int NETWORK_PORT = 18100;
    public static final int CLIENT_NETWORK_PORT = 18101;
    public static final int MAX_UDP_BYTES = 1232;
    public static final int MAX_TCP_BYTES = 1024 * 1024;

    private NetworkProtocol() {}

    /** Decoded ZTD1 delivery payload (inside the outer ZTS1/builtin packet). */
    public record Delivery(
            String network, String sender, String target, String plugin,
            byte[] messageId, byte[] data) {}

    public static boolean validNetworkId(String id) {
        return id != null && id.matches("[0-9a-fA-F]{16}");
    }

    public static boolean validMemberId(String id) {
        return id != null && id.matches("[0-9a-fA-F]{10}");
    }

    public static boolean validPluginId(String id) {
        return Wire.validId(id) && !Wire.BUILTIN.equalsIgnoreCase(id);
    }

    /** Maximum ZTD1 application payload for a plugin route. */
    public static int maxNetworkDataBytes(String plugin) {
        return MAX_UDP_BYTES - 52 - plugin.getBytes(StandardCharsets.US_ASCII).length;
    }

    /** Encodes a ZTD1 delivery into a complete ZTS1/builtin UDP payload, or null if invalid. */
    public static byte[] encodeDelivery(
            String network, String sender, String target, String plugin,
            byte[] messageId, byte[] data) {
        if (!validNetworkId(network) || !validMemberId(sender) || !validMemberId(target)
                || !validPluginId(plugin) || messageId == null || messageId.length != 16
                || data == null || data.length > maxNetworkDataBytes(plugin)) {
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

    /** Decodes a ZTD1 payload, or returns null when it is not a valid delivery. */
    public static Delivery decodeDelivery(byte[] payload) {
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
                hex(Arrays.copyOfRange(payload, 4, 12)),
                hex(Arrays.copyOfRange(payload, 12, 17)),
                hex(Arrays.copyOfRange(payload, 17, 22)),
                plugin,
                Arrays.copyOfRange(payload, 22, 38),
                Arrays.copyOfRange(payload, 39 + pluginLength, payload.length));
    }

    /** First assigned IPv4 address from a controller member, or null when unauthorized. */
    public static InetAddress memberAddress(JsonNode member) {
        if (member == null || !member.path("authorized").asBoolean(false)) {
            return null;
        }
        JsonNode assignments = member.path("ipAssignments");
        if (!assignments.isArray()) {
            return null;
        }
        for (JsonNode value : assignments) {
            InetAddress address = parseIpv4(value.asText("").split("/", 2)[0]);
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

    /** First IPv4 value in an {@code assignedAddresses} array, or null. */
    public static InetAddress firstIpv4(JsonNode values) {
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

    /** Controller address: assigned IPv4 masked to /24 with host part .1. */
    public static InetAddress networkServer(JsonNode descriptor) {
        InetAddress assigned = firstIpv4(descriptor.path("assignedAddresses"));
        if (assigned == null) {
            return null;
        }
        return fromIpv4((ipv4ToLong(assigned) & 0xffffff00L) | 1);
    }

    public static InetAddress parseIpv4(String text) {
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

    public static long ipv4ToLong(InetAddress address) {
        byte[] bytes = address.getAddress();
        return ((long) (bytes[0] & 0xff) << 24)
                | ((long) (bytes[1] & 0xff) << 16)
                | ((long) (bytes[2] & 0xff) << 8)
                | (bytes[3] & 0xff);
    }

    public static InetAddress fromIpv4(long value) {
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

    public static String hex(byte[] bytes) {
        StringBuilder builder = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            builder.append(Character.forDigit((value >>> 4) & 0xf, 16));
            builder.append(Character.forDigit(value & 0xf, 16));
        }
        return builder.toString();
    }

    public static byte[] decodeHex(String text) {
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

    public static byte[] decodeBase64(String text) {
        try {
            return Base64.getDecoder().decode(text == null ? "" : text);
        } catch (IllegalArgumentException invalid) {
            return null;
        }
    }
}
