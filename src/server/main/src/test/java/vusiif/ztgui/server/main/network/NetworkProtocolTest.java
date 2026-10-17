package vusiif.ztgui.server.main.network;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.Random;
import org.junit.jupiter.api.Test;
import vusiif.ztgui.server.main.server.Wire;

class NetworkProtocolTest {
    private static final ObjectMapper JSON = new ObjectMapper();

    @Test
    void idAndDataLimitsMatchTheProtocol() {
        assertTrue(NetworkProtocol.validNetworkId("0123456789abcdef"));
        assertTrue(NetworkProtocol.validNetworkId("0123456789ABCDEF"));
        assertFalse(NetworkProtocol.validNetworkId("0123456789abcde"));
        assertFalse(NetworkProtocol.validNetworkId("0123456789abcdeg"));
        assertTrue(NetworkProtocol.validMemberId("0123456789"));
        assertFalse(NetworkProtocol.validMemberId("012345678"));
        assertTrue(NetworkProtocol.validPluginId("voice-chat"));
        assertFalse(NetworkProtocol.validPluginId("builtin"));
        assertFalse(NetworkProtocol.validPluginId("BuiltIn"));
        assertEquals(1232 - 52 - "voice-chat".length(),
                NetworkProtocol.maxNetworkDataBytes("voice-chat"));
    }

    @Test
    void deliveryRoundTrips() {
        byte[] message = new byte[16];
        new Random(7).nextBytes(message);
        byte[] data = "audio".getBytes(StandardCharsets.UTF_8);
        byte[] packet = NetworkProtocol.encodeDelivery(
                "0123456789abcdef", "0123456789", "abcdef0123", "voice-chat", message, data);
        assertNotNull(packet);
        Wire.Packet outer = Wire.decode(packet);
        assertNotNull(outer);
        assertEquals("builtin", outer.route());
        NetworkProtocol.Delivery delivery = NetworkProtocol.decodeDelivery(outer.payload());
        assertNotNull(delivery);
        assertEquals("0123456789abcdef", delivery.network());
        assertEquals("0123456789", delivery.sender());
        assertEquals("abcdef0123", delivery.target());
        assertEquals("voice-chat", delivery.plugin());
        assertArrayEquals(message, delivery.messageId());
        assertArrayEquals(data, delivery.data());
    }

    @Test
    void deliveryRejectsInvalidInputs() {
        byte[] message = new byte[16];
        assertNull(NetworkProtocol.encodeDelivery(
                "bad", "0123456789", "abcdef0123", "plugin", message, new byte[0]));
        assertNull(NetworkProtocol.encodeDelivery(
                "0123456789abcdef", "bad", "abcdef0123", "plugin", message, new byte[0]));
        assertNull(NetworkProtocol.encodeDelivery(
                "0123456789abcdef", "0123456789", "bad", "plugin", message, new byte[0]));
        assertNull(NetworkProtocol.encodeDelivery(
                "0123456789abcdef", "0123456789", "abcdef0123", "bad.plugin", message, new byte[0]));
        assertNull(NetworkProtocol.encodeDelivery(
                "0123456789abcdef", "0123456789", "abcdef0123", "plugin", new byte[15], new byte[0]));
        assertNull(NetworkProtocol.encodeDelivery(
                "0123456789abcdef", "0123456789", "abcdef0123", "plugin", message,
                new byte[NetworkProtocol.maxNetworkDataBytes("plugin") + 1]));
        byte[] tooLarge = NetworkProtocol.encodeDelivery(
                "0123456789abcdef", "0123456789", "abcdef0123", "plugin", message,
                new byte[NetworkProtocol.maxNetworkDataBytes("plugin")]);
        assertNotNull(tooLarge);
        Wire.Packet outer = Wire.decode(tooLarge);
        assertNotNull(outer);
        assertTrue(outer.payload().length + 13 == NetworkProtocol.MAX_UDP_BYTES);
        assertNull(NetworkProtocol.decodeDelivery(new byte[0]));
        assertNull(NetworkProtocol.decodeDelivery("ZTD1".getBytes()));
    }

    @Test
    void memberAddressUsesFirstAuthorizedIpv4() throws Exception {
        JsonNode member = JSON.readTree("""
                {"authorized":true,"ipAssignments":["10.0.0.0/24","10.0.0.8/24"]}
                """);
        InetAddress address = NetworkProtocol.memberAddress(member);
        assertNotNull(address);
        assertEquals("10.0.0.8", address.getHostAddress());
        assertNull(NetworkProtocol.memberAddress(JSON.readTree(
                "{\"authorized\":false,\"ipAssignments\":[\"10.0.0.8/24\"]}")));
        assertNull(NetworkProtocol.memberAddress(JSON.readTree(
                "{\"authorized\":true,\"ipAssignments\":[\"10.0.0.0/24\",\"10.0.0.255/24\"]}")));
        assertNull(NetworkProtocol.memberAddress(JSON.readTree(
                "{\"authorized\":true,\"ipAssignments\":[\"IPv6\"]}")));
        assertNull(NetworkProtocol.memberAddress(null));
    }

    @Test
    void networkServerMasksToSlash24() throws Exception {
        assertEquals("10.20.30.1", NetworkProtocol.networkServer(JSON.readTree(
                "{\"assignedAddresses\":[\"10.20.30.57/24\"]}")).getHostAddress());
        assertNull(NetworkProtocol.networkServer(JSON.readTree(
                "{\"assignedAddresses\":[\"not-an-ip\"]}")));
        assertEquals("127.0.0.1", NetworkProtocol.networkServer(JSON.readTree(
                "{\"assignedAddresses\":[\"127.0.0.9/24\"]}")).getHostAddress());
    }

    @Test
    void ipv4HelpersRoundTrip() throws Exception {
        InetAddress address = InetAddress.getByName("192.168.1.200");
        assertEquals(0xC0A801C8L, NetworkProtocol.ipv4ToLong(address));
        assertEquals("192.168.1.200",
                NetworkProtocol.fromIpv4(NetworkProtocol.ipv4ToLong(address)).getHostAddress());
        assertEquals("10.0.0.1", NetworkProtocol.fromIpv4(0x0A000001L).getHostAddress());
        assertNull(NetworkProtocol.parseIpv4("example.com"));
        assertNull(NetworkProtocol.parseIpv4("999.1.1.1"));
        assertNull(NetworkProtocol.parseIpv4("::1"));
    }

    @Test
    void hexAndBase64HelpersAreStrict() {
        assertEquals("00ff10", NetworkProtocol.hex(new byte[] {0, (byte) 0xff, 0x10}));
        assertArrayEquals(new byte[] {0, (byte) 0xff}, NetworkProtocol.decodeHex("00fF"));
        assertNull(NetworkProtocol.decodeHex("0"));
        assertNull(NetworkProtocol.decodeHex("zz"));
        assertArrayEquals("hi".getBytes(StandardCharsets.UTF_8),
                NetworkProtocol.decodeBase64("aGk="));
        assertNull(NetworkProtocol.decodeBase64("not base64!"));
    }
}
