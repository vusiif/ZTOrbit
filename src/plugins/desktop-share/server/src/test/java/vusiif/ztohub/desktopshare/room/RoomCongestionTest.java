package vusiif.ztohub.desktopshare.room;

import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;
import vusiif.ztohub.desktopshare.protocol.MediaProtocol;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Stage-5 congestion tests at the plugin boundary: rotating fan-out under egress
 * exhaustion, publisher ingress limiting, per-viewer send failures, reservation
 * accounting across join/leave/profile/stop, and the 256 KiB control backlog.
 */
class RoomCongestionTest {
    private record Viewer(long connection, ControlProtocol.RoomInfo room,
                          InetSocketAddress endpoint) {}

    private static Viewer viewer(Harness harness, long roomId, String roomCode) {
        long connection = harness.connect();
        harness.hello(connection);
        ControlProtocol.RoomInfo joined = harness.join(connection, roomCode).room();
        InetSocketAddress endpoint = harness.endpoint("10.0.0.1", 41000 + (int) connection);
        harness.register(connection, roomId, joined.tokenHex(), endpoint);
        return new Viewer(connection, joined, endpoint);
    }

    private static ControlProtocol.RoomInfo createRoom(Harness harness, long publisher,
            String profile) {
        harness.hello(publisher);
        return harness.create(publisher, profile).room();
    }

    private static void sendFragment(Harness harness, long publisher,
            ControlProtocol.RoomInfo room, InetSocketAddress endpoint, long epoch,
            long sequence, int bytes) {
        harness.sendVideoFragment(publisher, publisher, room.roomId(), room.tokenHex(), epoch,
                sequence, endpoint, true, 0, 1, new byte[bytes]);
    }

    @Test
    void exhaustedEgressRotatesTheStartViewerAndKeepsCountingPerViewer() {
        // A burst that fits exactly one forwarded datagram: each fragment can serve only
        // one viewer, so the recorded recipient directly exposes the rotation.
        Tuning tuning = Tuning.builder().egressBurstBytes(300).build();
        Harness harness = new Harness(tuning);
        long publisher = harness.connect();
        ControlProtocol.RoomInfo room = createRoom(harness, publisher, "720p30");
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        harness.register(publisher, room.roomId(), room.tokenHex(), publisherEndpoint);
        Viewer first = viewer(harness, room.roomId(), room.roomCode());
        Viewer second = viewer(harness, room.roomId(), room.roomCode());
        Viewer third = viewer(harness, room.roomId(), room.roomCode());
        long epoch = harness.configure720(publisher, room.roomId()).streamEpoch();
        for (Viewer viewer : List.of(first, second, third)) {
            assertTrue(harness.ready(viewer.connection(), room.roomId(), epoch).ok());
        }

        List<InetSocketAddress> recipients = new ArrayList<>();
        for (long sequence = 0; sequence < 4; sequence++) {
            // Refill the tiny burst so each fragment can admit exactly one viewer; the
            // rotation, not the token exhaustion, decides which one.
            harness.advanceMillis(1);
            int before = harness.udpSends();
            sendFragment(harness, publisher, room, publisherEndpoint, epoch, sequence, 100);
            for (Viewer viewer : List.of(first, second, third)) {
                if (!harness.udpTo(viewer.endpoint(), before).isEmpty()) {
                    recipients.add(viewer.endpoint());
                }
            }
        }
        assertEquals(List.of(first.endpoint(), second.endpoint(), third.endpoint(),
                first.endpoint()), recipients, "fan-out must rotate its start viewer");

        RoomController.Stats stats = harness.stats();
        assertEquals(4L, stats.forwardedFragments());
        assertEquals(8L, stats.egressLimitedFragments());
        assertEquals(3, stats.viewers().size());
        long dropped = 0;
        for (RoomController.ViewerStats viewer : stats.viewers()) {
            dropped += viewer.egressLimitedFragments();
            assertEquals(2_500_000L, viewer.reservedBitsPerSecond());
        }
        assertEquals(8L, dropped, "each viewer records its own egress drops");

        // The bucket refills; the next fragment still rotates (cursor 4 -> viewer 2).
        harness.advanceMillis(1_000);
        int before = harness.udpSends();
        sendFragment(harness, publisher, room, publisherEndpoint, epoch, 4, 100);
        assertEquals(1, harness.udpTo(second.endpoint(), before).size());
        assertEquals(5L, harness.stats().forwardedFragments());
    }

    @Test
    void publisherIngressLimitDropsExcessFragmentsAndRecoversWithTime() {
        Tuning tuning = Tuning.builder().ingressBurstBytes(500).build();
        Harness harness = new Harness(tuning);
        long publisher = harness.connect();
        ControlProtocol.RoomInfo room = createRoom(harness, publisher, "720p30");
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        harness.register(publisher, room.roomId(), room.tokenHex(), publisherEndpoint);
        Viewer viewer = viewer(harness, room.roomId(), room.roomCode());
        long epoch = harness.configure720(publisher, room.roomId()).streamEpoch();
        assertTrue(harness.ready(viewer.connection(), room.roomId(), epoch).ok());

        // 188-byte ZDS1 payload plus the 19-byte ZTS1 frame costs 207 bytes; the 500-byte
        // burst accepts two fragments and rejects the third without waiting for tokens.
        sendFragment(harness, publisher, room, publisherEndpoint, epoch, 0, 100);
        sendFragment(harness, publisher, room, publisherEndpoint, epoch, 1, 100);
        int before = harness.udpSends();
        sendFragment(harness, publisher, room, publisherEndpoint, epoch, 2, 100);
        assertTrue(harness.udpTo(viewer.endpoint(), before).isEmpty());
        RoomController.Stats limited = harness.stats();
        assertEquals(2L, limited.acceptedVideoPackets());
        assertEquals(1L, limited.ingressLimitedFragments());
        assertEquals(2L, limited.forwardedFragments());

        // The 720p ingress rate is 500,000 B/s; one millisecond restores 500 bytes.
        harness.advanceMillis(1);
        sendFragment(harness, publisher, room, publisherEndpoint, epoch, 2, 100);
        assertEquals(3L, harness.stats().acceptedVideoPackets());
        assertEquals(1L, harness.stats().ingressLimitedFragments());
        assertEquals(3L, harness.stats().forwardedFragments());
    }

    @Test
    void oneViewerSendFailureDropsOnlyThatCopyAndOtherViewersContinue() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        ControlProtocol.RoomInfo room = createRoom(harness, publisher, "720p30");
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        harness.register(publisher, room.roomId(), room.tokenHex(), publisherEndpoint);
        Viewer first = viewer(harness, room.roomId(), room.roomCode());
        Viewer failing = viewer(harness, room.roomId(), room.roomCode());
        Viewer third = viewer(harness, room.roomId(), room.roomCode());
        long epoch = harness.configure720(publisher, room.roomId()).streamEpoch();
        for (Viewer viewer : List.of(first, failing, third)) {
            assertTrue(harness.ready(viewer.connection(), room.roomId(), epoch).ok());
        }
        harness.host.failUdpTo(failing.endpoint());

        int before = harness.udpSends();
        sendFragment(harness, publisher, room, publisherEndpoint, epoch, 0, 64);
        assertEquals(1, harness.udpTo(first.endpoint(), before).size());
        assertTrue(harness.udpTo(failing.endpoint(), before).isEmpty());
        assertEquals(1, harness.udpTo(third.endpoint(), before).size());
        RoomController.Stats stats = harness.stats();
        assertEquals(2L, stats.forwardedFragments());
        assertEquals(1L, stats.forwardedDropped());
        for (RoomController.ViewerStats viewer : stats.viewers()) {
            if (viewer.connectionId() == failing.connection()) {
                assertEquals(1L, viewer.sendDroppedFragments());
                assertEquals(0L, viewer.egressLimitedFragments());
            } else {
                assertEquals(1L, viewer.forwardedFragments());
                assertEquals(0L, viewer.sendDroppedFragments());
            }
        }

        // The failed copy is not retried or cached; the next fragment simply tries again.
        harness.host.allowUdpTo(failing.endpoint());
        int next = harness.udpSends();
        sendFragment(harness, publisher, room, publisherEndpoint, epoch, 1, 64);
        assertEquals(3, harness.udpSends() - next);
    }

    @Test
    void reservationsTrackJoinLeaveProfileChangeAndStop() {
        Tuning tuning = Tuning.builder().egressBudgetBitsPerSecond(7_500_000L).build();
        Harness harness = new Harness(tuning);
        long publisher = harness.connect();
        ControlProtocol.RoomInfo room = createRoom(harness, publisher, "720p30");
        Viewer first = viewer(harness, room.roomId(), room.roomCode());
        Viewer second = viewer(harness, room.roomId(), room.roomCode());
        assertEquals(5_000_000L, harness.stats().reservedEgressBitsPerSecond());
        assertTrue(harness.configure720(publisher, room.roomId()).ok());

        // 2 viewers at 1080p would need 10,000,000 bit/s; the 7.5 Mbit/s budget must
        // reject the switch and keep both the old profile and the old reservation.
        ControlProtocol.Response rejected = harness.configure1080(publisher, room.roomId());
        assertFalse(rejected.ok());
        assertEquals(ControlProtocol.ErrorCode.CAPACITY, rejected.error());
        assertEquals(5_000_000L, harness.stats().reservedEgressBitsPerSecond());
        assertEquals(2, harness.stats().viewers().size());

        assertTrue(harness.leave(first.connection(), room.roomId()).ok());
        assertEquals(2_500_000L, harness.stats().reservedEgressBitsPerSecond());
        assertTrue(harness.leave(second.connection(), room.roomId()).ok());
        assertEquals(0L, harness.stats().reservedEgressBitsPerSecond());
        assertTrue(harness.stop(publisher, room.roomId()).ok());
        assertEquals(0L, harness.stats().reservedEgressBitsPerSecond());
        assertEquals(0, harness.stats().members());
        assertEquals(0, harness.stats().rooms());
    }

    @Test
    void profileChangeUpdatesEveryViewerReservation() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        ControlProtocol.RoomInfo room = createRoom(harness, publisher, "720p30");
        Viewer first = viewer(harness, room.roomId(), room.roomCode());
        Viewer second = viewer(harness, room.roomId(), room.roomCode());
        assertEquals(5_000_000L, harness.stats().reservedEgressBitsPerSecond());
        assertTrue(harness.configure720(publisher, room.roomId()).ok());

        assertTrue(harness.configure1080(publisher, room.roomId()).ok());
        assertEquals(10_000_000L, harness.stats().reservedEgressBitsPerSecond());
        for (RoomController.ViewerStats viewer : harness.stats().viewers()) {
            assertEquals(5_000_000L, viewer.reservedBitsPerSecond());
            assertFalse(viewer.ready(), "a new epoch requires a fresh ready");
        }
        assertTrue(harness.leave(first.connection(), room.roomId()).ok());
        assertEquals(5_000_000L, harness.stats().reservedEgressBitsPerSecond());
        assertTrue(harness.stop(publisher, room.roomId()).ok());
        assertEquals(0L, harness.stats().reservedEgressBitsPerSecond());
        assertEquals(0, harness.stats().members());
    }

    @Test
    void controlBacklogOver256KiBIsDisconnectedAfterStateIsConsistent() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        ControlProtocol.RoomInfo room = createRoom(harness, publisher, "720p30");
        Viewer viewer = viewer(harness, room.roomId(), room.roomCode());

        // Exactly the threshold is still allowed; one byte more schedules a disconnect.
        harness.host.setQueuedBytes(publisher, 256L * 1024);
        assertTrue(harness.ping(publisher).ok());
        assertTrue(harness.host.disconnected().isEmpty());

        harness.host.setQueuedBytes(publisher, 256L * 1024 + 1);
        assertTrue(harness.ping(publisher).ok());
        assertEquals(List.of(publisher), harness.host.disconnected());
        assertEquals(1L, harness.stats().controlBacklogDisconnects());
        // The controller has not removed the room yet: the host performs the close.
        assertEquals(1, harness.stats().rooms());
        harness.close(publisher);
        assertEquals(0, harness.stats().rooms());
        assertEquals(0, harness.stats().members());
        assertEquals(0L, harness.stats().reservedEgressBitsPerSecond());

        // A backlogged viewer only releases its own reservation; the room survives.
        long publisher2 = harness.connect();
        ControlProtocol.RoomInfo room2 = createRoom(harness, publisher2, "720p30");
        Viewer first = viewer(harness, room2.roomId(), room2.roomCode());
        Viewer second = viewer(harness, room2.roomId(), room2.roomCode());
        harness.host.setQueuedBytes(first.connection(), 256L * 1024 + 1);
        assertTrue(harness.ping(first.connection()).ok());
        assertTrue(harness.host.disconnected().contains(first.connection()));
        harness.close(first.connection());
        assertEquals(1, harness.stats().rooms());
        assertEquals(2_500_000L, harness.stats().reservedEgressBitsPerSecond());
        assertEquals(1, harness.stats().viewers().size());
        assertTrue(harness.stop(publisher2, room2.roomId()).ok());
        assertEquals(0L, harness.stats().reservedEgressBitsPerSecond());
        assertEquals(0, harness.stats().members());
    }

    @Test
    void oversizedPublisherFloodIsDroppedWithoutGrowingState() {
        Tuning tuning = Tuning.builder().ingressBurstBytes(1000).build();
        Harness harness = new Harness(tuning);
        long publisher = harness.connect();
        ControlProtocol.RoomInfo room = createRoom(harness, publisher, "720p30");
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        harness.register(publisher, room.roomId(), room.tokenHex(), publisherEndpoint);
        Viewer viewer = viewer(harness, room.roomId(), room.roomCode());
        long epoch = harness.configure720(publisher, room.roomId()).streamEpoch();
        assertTrue(harness.ready(viewer.connection(), room.roomId(), epoch).ok());

        long started = System.nanoTime();
        for (int sequence = 0; sequence < 5000; sequence++) {
            sendFragment(harness, publisher, room, publisherEndpoint, epoch, sequence, 64);
        }
        long elapsedMillis = (System.nanoTime() - started) / 1_000_000L;
        RoomController.Stats stats = harness.stats();
        // The ingress bucket admits only a handful of these fragments; the rest are
        // dropped before any fan-out state is created.
        assertTrue(stats.ingressLimitedFragments() > 4900);
        assertTrue(stats.acceptedVideoPackets() < 10);
        assertEquals(1, stats.rooms());
        assertEquals(2, stats.members());
        assertEquals(1, stats.viewers().size());
        assertTrue(stats.viewers().get(0).forwardedFragments() < 10);
        assertTrue(elapsedMillis < 5000, "flood handling must stay bounded, took "
                + elapsedMillis + " ms");
    }

    @Test
    void stopDrainsReservationsAndClosesEveryRoom() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        ControlProtocol.RoomInfo room = createRoom(harness, publisher, "720p30");
        viewer(harness, room.roomId(), room.roomCode());
        viewer(harness, room.roomId(), room.roomCode());
        assertEquals(5_000_000L, harness.stats().reservedEgressBitsPerSecond());

        harness.stop();
        RoomController.Stats stopped = harness.stats();
        assertEquals(0, stopped.rooms());
        assertEquals(0, stopped.members());
        assertEquals(0, stopped.sessions());
        assertEquals(0L, stopped.reservedEgressBitsPerSecond());
        assertTrue(stopped.viewers().isEmpty());
    }
}
