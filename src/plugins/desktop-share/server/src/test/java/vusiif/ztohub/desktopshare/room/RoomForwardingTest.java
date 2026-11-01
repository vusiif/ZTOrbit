package vusiif.ztohub.desktopshare.room;

import java.net.InetSocketAddress;
import java.util.List;
import org.junit.jupiter.api.Test;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;
import vusiif.ztohub.desktopshare.protocol.MediaProtocol;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Stage-4 forwarding tests. The controller must validate one publisher fragment and copy
 * it to the room's registered, ready viewers of the current epoch, replacing only the
 * destination token and session. It must never reassemble, decode or cache video.
 */
class RoomForwardingTest {
    private record Viewer(long connection, ControlProtocol.RoomInfo room,
                          InetSocketAddress endpoint) {}

    private static Viewer viewer(Harness harness, long publisherRoom, String roomCode) {
        long connection = harness.connect();
        harness.hello(connection);
        ControlProtocol.RoomInfo joined = harness.join(connection, roomCode).room();
        InetSocketAddress endpoint = harness.endpoint("10.0.0.1", 41000 + (int) connection);
        harness.register(connection, publisherRoom, joined.tokenHex(), endpoint);
        return new Viewer(connection, joined, endpoint);
    }

    private static ControlProtocol.RoomInfo publisher(Harness harness, long connection,
            String profile) {
        harness.hello(connection);
        return harness.create(connection, profile).room();
    }

    @Test
    void oneFragmentReachesEveryReadyViewerWithOnlyTokenAndSessionReplaced() {
        Harness harness = new Harness();
        long publisherConnection = harness.connect();
        ControlProtocol.RoomInfo room = publisher(harness, publisherConnection, "720p30");
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        harness.register(publisherConnection, room.roomId(), room.tokenHex(), publisherEndpoint);
        Viewer first = viewer(harness, room.roomId(), room.roomCode());
        Viewer second = viewer(harness, room.roomId(), room.roomCode());

        ControlProtocol.Response configured = harness.configure720(publisherConnection,
                room.roomId());
        long epoch = configured.streamEpoch();
        assertTrue(harness.ready(first.connection(), room.roomId(), epoch).ok());
        assertTrue(harness.ready(second.connection(), room.roomId(), epoch).ok());
        harness.events(first.connection());
        harness.events(second.connection());

        int before = harness.udpSends();
        byte[] payload = new byte[1125];
        for (int index = 0; index < payload.length; index++) {
            payload[index] = (byte) (index * 7 + 1);
        }
        harness.sendVideoFragment(publisherConnection, publisherConnection, room.roomId(),
                room.tokenHex(), epoch, 0, publisherEndpoint, true, 0, 2, payload);

        assertEquals(1L, harness.stats().acceptedVideoPackets());
        assertEquals(2L, harness.stats().forwardedFragments());

        List<MediaProtocol.Decoded> firstSends = harness.udpTo(first.endpoint(), before);
        List<MediaProtocol.Decoded> secondSends = harness.udpTo(second.endpoint(), before);
        List<MediaProtocol.Decoded> publisherSends = harness.udpTo(publisherEndpoint, before);
        assertEquals(1, firstSends.size());
        assertEquals(1, secondSends.size());
        assertTrue(publisherSends.isEmpty(), "server must not echo video to the publisher");

        MediaProtocol.Header source = harnessSentHeader(payload, room, epoch);
        for (MediaProtocol.Decoded send : firstSends) {
            assertArrayEquals(payload, send.fragment(), "encoded payload stays verbatim");
            assertArrayEquals(ControlProtocol.tokenFromHex(first.room().tokenHex()),
                    send.header().token());
            assertEquals(first.connection(), send.header().sessionId());
            assertEquals(source.sequence(), send.header().sequence());
            assertEquals(source.pts(), send.header().pts());
            assertEquals(source.streamEpoch(), send.header().streamEpoch());
            assertEquals(source.flags(), send.header().flags());
            assertEquals(source.packetBytes(), send.header().packetBytes());
            assertEquals(source.fragmentIndex(), send.header().fragmentIndex());
            assertEquals(source.fragmentCount(), send.header().fragmentCount());
        }
        for (MediaProtocol.Decoded send : secondSends) {
            assertArrayEquals(ControlProtocol.tokenFromHex(second.room().tokenHex()),
                    send.header().token());
            assertEquals(second.connection(), send.header().sessionId());
        }
        assertNotEquals(first.connection(), second.connection());
        assertNotEquals(first.endpoint(), second.endpoint());
    }

    @Test
    void oneFragmentIsForwardedOnceEvenWhenTheWholePacketWouldNeedThree() {
        Harness harness = new Harness();
        long publisherConnection = harness.connect();
        ControlProtocol.RoomInfo room = publisher(harness, publisherConnection, "720p30");
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        harness.register(publisherConnection, room.roomId(), room.tokenHex(), publisherEndpoint);
        Viewer viewer = viewer(harness, room.roomId(), room.roomCode());
        long epoch = harness.configure720(publisherConnection, room.roomId()).streamEpoch();
        assertTrue(harness.ready(viewer.connection(), room.roomId(), epoch).ok());

        int before = harness.udpSends();
        byte[] payload = new byte[1125];
        harness.sendVideoFragment(publisherConnection, publisherConnection, room.roomId(),
                room.tokenHex(), epoch, 5, publisherEndpoint, true, 0, 3, payload);
        List<MediaProtocol.Decoded> sends = harness.udpTo(viewer.endpoint(), before);
        assertEquals(1, sends.size(), "the server forwards fragments, it does not reassemble");
        assertEquals(0, sends.get(0).header().fragmentIndex());
        assertEquals(3, sends.get(0).header().fragmentCount());
        // A later single-fragment packet is independent; the server keeps no history.
        int second = harness.udpSends();
        harness.sendVideoFragment(publisherConnection, publisherConnection, room.roomId(),
                room.tokenHex(), epoch, 6, publisherEndpoint, false, 0, 1, new byte[] {9, 8, 7});
        assertEquals(1, harness.udpTo(viewer.endpoint(), second).size());
    }

    @Test
    void onlyRegisteredAndReadyViewersOfTheCurrentEpochReceive() {
        Harness harness = new Harness();
        long publisherConnection = harness.connect();
        ControlProtocol.RoomInfo room = publisher(harness, publisherConnection, "720p30");
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        harness.register(publisherConnection, room.roomId(), room.tokenHex(), publisherEndpoint);
        Viewer readyViewer = viewer(harness, room.roomId(), room.roomCode());

        long unready = harness.connect();
        harness.hello(unready);
        ControlProtocol.RoomInfo unreadyRoom = harness.join(unready, room.roomCode()).room();
        InetSocketAddress unreadyEndpoint = harness.endpoint("10.0.0.1", 42001);
        harness.register(unready, room.roomId(), unreadyRoom.tokenHex(), unreadyEndpoint);

        long unregistered = harness.connect();
        harness.hello(unregistered);
        harness.join(unregistered, room.roomCode());
        InetSocketAddress unregisteredEndpoint = harness.endpoint("10.0.0.1", 42002);

        long epoch = harness.configure720(publisherConnection, room.roomId()).streamEpoch();
        assertTrue(harness.ready(readyViewer.connection(), room.roomId(), epoch).ok());

        int before = harness.udpSends();
        harness.sendVideoFragment(publisherConnection, publisherConnection, room.roomId(),
                room.tokenHex(), epoch, 0, publisherEndpoint, true, 0, 1, new byte[] {1, 2});
        assertEquals(1, harness.udpTo(readyViewer.endpoint(), before).size());
        assertTrue(harness.udpTo(unreadyEndpoint, before).isEmpty(),
                "a viewer that did not confirm the epoch must not receive video");
        assertTrue(harness.udpTo(unregisteredEndpoint, before).isEmpty());
        assertEquals(2L, harness.stats().notReadyFragments());
    }

    @Test
    void staleSequenceWrongEpochWrongTokenAndCrossRoomFragmentsAreDropped() {
        Harness harness = new Harness();
        long publisherConnection = harness.connect();
        ControlProtocol.RoomInfo room = publisher(harness, publisherConnection, "720p30");
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        harness.register(publisherConnection, room.roomId(), room.tokenHex(), publisherEndpoint);
        Viewer viewer = viewer(harness, room.roomId(), room.roomCode());
        long epoch = harness.configure720(publisherConnection, room.roomId()).streamEpoch();
        assertTrue(harness.ready(viewer.connection(), room.roomId(), epoch).ok());

        int before = harness.udpSends();
        // Move the window forward, then inject a fragment far behind it.
        for (long sequence = 0; sequence <= 2048; sequence += 512) {
            harness.sendVideoFragment(publisherConnection, publisherConnection, room.roomId(),
                    room.tokenHex(), epoch, sequence, publisherEndpoint, true, 0, 1,
                    new byte[] {1});
        }
        int accepted = (int) harness.stats().acceptedVideoPackets();
        harness.sendVideoFragment(publisherConnection, publisherConnection, room.roomId(),
                room.tokenHex(), epoch, 0, publisherEndpoint, true, 0, 1, new byte[] {1});
        assertEquals(accepted, harness.stats().acceptedVideoPackets(),
                "a sequence far behind the window is stale");
        assertEquals(1L, harness.stats().staleSequenceFragments());

        // Wrong epoch, wrong token and another room's token must all be rejected.
        harness.sendVideoFragment(publisherConnection, publisherConnection, room.roomId(),
                room.tokenHex(), epoch + 7, 0, publisherEndpoint, true, 0, 1, new byte[] {1});
        harness.sendVideoFragment(publisherConnection, publisherConnection, room.roomId(),
                "00112233445566778899aabbccddeeff", epoch, 0, publisherEndpoint, true, 0, 1,
                new byte[] {1});
        harness.sendVideoFragment(publisherConnection, publisherConnection, room.roomId() + 99,
                room.tokenHex(), epoch, 0, publisherEndpoint, true, 0, 1, new byte[] {1});
        assertEquals(accepted, harness.stats().acceptedVideoPackets());

        // Only the five legitimate fragments reached the viewer.
        List<MediaProtocol.Decoded> sends = harness.udpTo(viewer.endpoint(), before);
        assertEquals(5, sends.size());
        for (MediaProtocol.Decoded send : sends) {
            assertArrayEquals(ControlProtocol.tokenFromHex(viewer.room().tokenHex()),
                    send.header().token());
        }
    }

    @Test
    void twoRoomsNeverReceiveEachOthersVideo() {
        Harness harness = new Harness();
        long publisherA = harness.connect();
        long publisherB = harness.connect();
        ControlProtocol.RoomInfo roomA = publisher(harness, publisherA, "720p30");
        ControlProtocol.RoomInfo roomB = publisher(harness, publisherB, "1080p30");
        InetSocketAddress endpointA = harness.endpoint("10.0.0.1", 40000);
        InetSocketAddress endpointB = harness.endpoint("10.0.0.1", 40001);
        harness.register(publisherA, roomA.roomId(), roomA.tokenHex(), endpointA);
        harness.register(publisherB, roomB.roomId(), roomB.tokenHex(), endpointB);
        Viewer viewerA = viewer(harness, roomA.roomId(), roomA.roomCode());
        Viewer viewerB = viewer(harness, roomB.roomId(), roomB.roomCode());
        long epochA = harness.configure720(publisherA, roomA.roomId()).streamEpoch();
        long epochB = harness.configure1080(publisherB, roomB.roomId()).streamEpoch();
        assertTrue(harness.ready(viewerA.connection(), roomA.roomId(), epochA).ok());
        assertTrue(harness.ready(viewerB.connection(), roomB.roomId(), epochB).ok());

        int before = harness.udpSends();
        harness.sendVideoFragment(publisherA, publisherA, roomA.roomId(), roomA.tokenHex(),
                epochA, 0, endpointA, true, 0, 1, new byte[] {0x0a});
        assertEquals(1, harness.udpTo(viewerA.endpoint(), before).size());
        assertTrue(harness.udpTo(viewerB.endpoint(), before).isEmpty());
        int second = harness.udpSends();
        harness.sendVideoFragment(publisherB, publisherB, roomB.roomId(), roomB.tokenHex(),
                epochB, 0, endpointB, true, 0, 1, new byte[] {0x0b});
        assertTrue(harness.udpTo(viewerA.endpoint(), second).isEmpty());
        assertEquals(1, harness.udpTo(viewerB.endpoint(), second).size());
    }

    @Test
    void leavingOrStoppingRemovesForwardingImmediately() {
        Harness harness = new Harness();
        long publisherConnection = harness.connect();
        ControlProtocol.RoomInfo room = publisher(harness, publisherConnection, "720p30");
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        harness.register(publisherConnection, room.roomId(), room.tokenHex(), publisherEndpoint);
        Viewer viewer = viewer(harness, room.roomId(), room.roomCode());
        long epoch = harness.configure720(publisherConnection, room.roomId()).streamEpoch();
        assertTrue(harness.ready(viewer.connection(), room.roomId(), epoch).ok());
        assertTrue(harness.leave(viewer.connection(), room.roomId()).ok());

        int before = harness.udpSends();
        harness.sendVideoFragment(publisherConnection, publisherConnection, room.roomId(),
                room.tokenHex(), epoch, 0, publisherEndpoint, true, 0, 1, new byte[] {1});
        assertTrue(harness.udpTo(viewer.endpoint(), before).isEmpty());
        assertEquals(1, harness.stats().members());
        assertTrue(harness.stop(publisherConnection, room.roomId()).ok());
        assertNotEquals(0, harness.stats().closedRooms());
    }

    private static MediaProtocol.Header harnessSentHeader(byte[] payload, ControlProtocol.RoomInfo room,
            long epoch) {
        // Mirrors what Harness.sendVideoFragment encodes: sequence 0, first of two
        // fragments, key frame, 1125 payload bytes and a 2250-byte packet.
        return new MediaProtocol.Header(MediaProtocol.Type.VIDEO,
                MediaProtocol.Flag.KEY_FRAME.mask(), new byte[16], 0, room.roomId(), epoch, 0,
                0, 0, 0, 2250, 0, 2, payload.length);
    }
}
