package vusiif.ztohub.desktopshare.room;

import java.net.InetSocketAddress;
import org.junit.jupiter.api.Test;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;
import vusiif.ztohub.desktopshare.protocol.MediaProtocol;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

class RoomTransportTest {
    @Test
    void registrationBindsPeerAddressAndAcknowledges() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        InetSocketAddress endpoint = harness.endpoint("10.0.0.1", 40000);

        MediaProtocol.Decoded ack = harness.register(publisher, room.roomId(),
                room.tokenHex(), endpoint);
        assertNotNull(ack);
        assertEquals(MediaProtocol.Type.ACK, ack.header().type());
        assertEquals(publisher, ack.header().sessionId());
        assertEquals(room.roomId(), ack.header().roomId());
        assertArrayEquals(ControlProtocol.tokenFromHex(room.tokenHex()), ack.header().token());
        assertEquals(1, harness.stats().registeredEndpoints());

        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(), endpoint));
        assertEquals(1, harness.stats().registeredEndpoints());
    }

    @Test
    void wrongTokenRoomSessionOrAddressCannotBind() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        InetSocketAddress endpoint = harness.endpoint("10.0.0.1", 40000);
        String wrongToken = corrupt(room.tokenHex());

        assertNull(harness.register(publisher, room.roomId(), wrongToken, endpoint));
        assertNull(harness.sendRegister(publisher, publisher + 100, room.roomId(),
                room.tokenHex(), endpoint));
        assertNull(harness.sendRegister(publisher, publisher, room.roomId() + 1,
                room.tokenHex(), endpoint));
        InetSocketAddress foreign = harness.endpoint("10.0.0.2", 40000);
        assertNull(harness.register(publisher, room.roomId(), room.tokenHex(), foreign));
        assertEquals(0, harness.stats().registeredEndpoints());

        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(), endpoint));
        assertEquals(1, harness.stats().registeredEndpoints());
    }

    @Test
    void videoBeforeRegistrationDoesNotBindThePort() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        assertTrue(harness.configure720(publisher, room.roomId()).ok());
        InetSocketAddress endpoint = harness.endpoint("10.0.0.1", 40000);

        harness.sendVideo(publisher, room.roomId(), room.tokenHex(), 1, endpoint, true);
        assertEquals(0, harness.stats().registeredEndpoints());
        assertEquals(1L, harness.stats().droppedUdpPackets());
        assertEquals(0L, harness.stats().acceptedVideoPackets());

        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(), endpoint));
        assertEquals(1, harness.stats().registeredEndpoints());
    }

    @Test
    void portChangeAfterBindingIsRejectedWithoutRebinding() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        InetSocketAddress bound = harness.endpoint("10.0.0.1", 40000);
        InetSocketAddress changed = harness.endpoint("10.0.0.1", 40001);

        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(), bound));
        assertNull(harness.register(publisher, room.roomId(), room.tokenHex(), changed));
        assertEquals(1, harness.stats().registeredEndpoints());
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(), bound));
    }

    @Test
    void tokensAreInvalidatedByLeaveRejoinAndStop() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        InetSocketAddress viewerEndpoint = harness.endpoint("10.0.0.1", 40001);
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                publisherEndpoint));
        ControlProtocol.RoomInfo joined = harness.join(viewer, room.roomCode()).room();
        assertNotNull(harness.register(viewer, room.roomId(), joined.tokenHex(),
                viewerEndpoint));
        assertEquals(2, harness.stats().registeredEndpoints());

        assertTrue(harness.leave(viewer, room.roomId()).ok());
        assertNull(harness.register(viewer, room.roomId(), joined.tokenHex(),
                viewerEndpoint));

        ControlProtocol.RoomInfo rejoined = harness.join(viewer, room.roomCode()).room();
        assertNull(harness.register(viewer, room.roomId(), joined.tokenHex(),
                viewerEndpoint));
        assertNotNull(harness.register(viewer, room.roomId(), rejoined.tokenHex(),
                viewerEndpoint));

        assertTrue(harness.stop(publisher, room.roomId()).ok());
        assertNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                publisherEndpoint));
        assertEquals(0, harness.stats().registeredEndpoints());

        ControlProtocol.RoomInfo second = harness.create(publisher, "720p30").room();
        assertNull(harness.register(publisher, second.roomId(), room.tokenHex(),
                publisherEndpoint));
        assertNotNull(harness.register(publisher, second.roomId(), second.tokenHex(),
                publisherEndpoint));
    }

    @Test
    void viewerCannotPublishVideoAndStaleEpochsAreDropped() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        InetSocketAddress viewerEndpoint = harness.endpoint("10.0.0.1", 40001);
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                publisherEndpoint));
        ControlProtocol.RoomInfo joined = harness.join(viewer, room.roomCode()).room();
        assertNotNull(harness.register(viewer, room.roomId(), joined.tokenHex(),
                viewerEndpoint));

        harness.sendVideo(publisher, room.roomId(), room.tokenHex(), 1, publisherEndpoint,
                true);
        assertEquals(0L, harness.stats().acceptedVideoPackets());
        assertTrue(harness.configure720(publisher, room.roomId()).ok());
        harness.sendVideo(viewer, room.roomId(), joined.tokenHex(), 1, viewerEndpoint, true);
        assertEquals(0L, harness.stats().acceptedVideoPackets());

        harness.sendVideo(publisher, room.roomId(), room.tokenHex(), 1, publisherEndpoint,
                true);
        assertEquals(1L, harness.stats().acceptedVideoPackets());
        long dropped = harness.stats().droppedUdpPackets();
        harness.sendVideo(publisher, room.roomId(), room.tokenHex(), 2, publisherEndpoint,
                true);
        assertEquals(1L, harness.stats().acceptedVideoPackets());
        assertEquals(dropped + 1, harness.stats().droppedUdpPackets());
    }

    private static String corrupt(String tokenHex) {
        char first = tokenHex.charAt(0);
        char replacement = first == '0' ? '1' : '0';
        return replacement + tokenHex.substring(1);
    }
}
