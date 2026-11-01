package vusiif.ztohub.desktopshare.room;

import java.net.InetSocketAddress;
import java.util.List;
import org.junit.jupiter.api.Test;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

class RoomTimeoutTest {
    @Test
    void preparationTimeoutClosesRoomAtBoundary() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        ControlProtocol.RoomInfo joined = harness.join(viewer, room.roomCode()).room();
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                harness.endpoint("10.0.0.1", 40000)));
        assertNotNull(harness.register(viewer, room.roomId(), joined.tokenHex(),
                harness.endpoint("10.0.0.1", 40001)));

        harness.advanceMillis(9_999);
        harness.tick();
        assertEquals(1, harness.stats().rooms());
        assertTrue(harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_CLOSED).isEmpty());

        harness.advanceMillis(1);
        harness.tick();
        List<ControlProtocol.Event> publisherClosed =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, publisherClosed.size());
        assertEquals(ControlProtocol.CloseReason.CONFIG_TIMEOUT,
                publisherClosed.get(0).reason());
        List<ControlProtocol.Event> viewerClosed =
                harness.eventsOf(viewer, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, viewerClosed.size());
        assertEquals(ControlProtocol.CloseReason.CONFIG_TIMEOUT, viewerClosed.get(0).reason());
        assertEquals(0, harness.stats().rooms());
        assertEquals(0, harness.stats().members());
        assertEquals(0L, harness.stats().reservedEgressBitsPerSecond());

        assertFalse(harness.join(viewer, room.roomCode()).ok());
        assertEquals(ControlProtocol.ErrorCode.ROOM_NOT_FOUND,
                harness.join(viewer, room.roomCode()).error());
    }

    @Test
    void preparationTimeoutDoesNotAffectAnotherRoom() {
        Harness harness = new Harness();
        long first = harness.connect();
        long second = harness.connect();
        harness.hello(first);
        harness.hello(second);
        ControlProtocol.RoomInfo firstRoom = harness.create(first, "720p30").room();
        assertNotNull(harness.register(first, firstRoom.roomId(), firstRoom.tokenHex(),
                harness.endpoint("10.0.0.1", 40000)));
        harness.advanceMillis(5_000);
        ControlProtocol.RoomInfo secondRoom = harness.create(second, "720p30").room();
        assertNotNull(harness.register(second, secondRoom.roomId(), secondRoom.tokenHex(),
                harness.endpoint("10.0.0.1", 40001)));
        long viewer = harness.connect();
        harness.hello(viewer);
        harness.advanceMillis(5_000);
        harness.tick();
        assertEquals(1, harness.stats().rooms());
        assertEquals(ControlProtocol.ErrorCode.ROOM_NOT_FOUND,
                harness.join(viewer, firstRoom.roomCode()).error());
        assertTrue(harness.stop(second, secondRoom.roomId()).ok());
        assertEquals(0, harness.stats().rooms());
    }

    @Test
    void controlIdleTimeoutClosesPublisherRoomAtBoundary() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        ControlProtocol.RoomInfo joined = harness.join(viewer, room.roomCode()).room();
        assertTrue(harness.configure720(publisher, room.roomId()).ok());
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                harness.endpoint("10.0.0.1", 40000)));
        assertNotNull(harness.register(viewer, room.roomId(), joined.tokenHex(),
                harness.endpoint("10.0.0.1", 40001)));

        harness.advanceMillis(14_999);
        harness.tick();
        assertEquals(1, harness.stats().rooms());
        harness.advanceMillis(1);
        harness.tick();
        List<ControlProtocol.Event> closed =
                harness.eventsOf(viewer, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, closed.size());
        assertEquals(ControlProtocol.CloseReason.OWNER_LOST, closed.get(0).reason());
        assertEquals(0, harness.stats().rooms());
    }

    @Test
    void viewerControlIdleTimeoutRemovesOnlyThatViewer() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long idleViewer = harness.connect();
        long activeViewer = harness.connect();
        harness.hello(publisher);
        harness.hello(idleViewer);
        harness.hello(activeViewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        ControlProtocol.RoomInfo idleJoin = harness.join(idleViewer, room.roomCode()).room();
        ControlProtocol.RoomInfo activeJoin =
                harness.join(activeViewer, room.roomCode()).room();
        assertTrue(harness.configure720(publisher, room.roomId()).ok());
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        InetSocketAddress activeEndpoint = harness.endpoint("10.0.0.1", 40001);
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                publisherEndpoint));
        assertNotNull(harness.register(idleViewer, room.roomId(), idleJoin.tokenHex(),
                harness.endpoint("10.0.0.1", 40002)));
        assertNotNull(harness.register(activeViewer, room.roomId(), activeJoin.tokenHex(),
                activeEndpoint));

        harness.advanceMillis(14_999);
        harness.ping(publisher);
        harness.ping(activeViewer);
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                publisherEndpoint));
        assertNotNull(harness.register(activeViewer, room.roomId(), activeJoin.tokenHex(),
                activeEndpoint));
        harness.advanceMillis(1);
        harness.tick();
        assertEquals(1, harness.stats().rooms());
        assertEquals(2, harness.stats().members());
        List<ControlProtocol.Event> states =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_STATE);
        assertEquals(1, states.get(states.size() - 1).viewerCount());
        List<ControlProtocol.Event> removed =
                harness.eventsOf(idleViewer, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, removed.size());
        assertEquals(ControlProtocol.CloseReason.STREAM_FAILED, removed.get(0).reason());
        assertTrue(harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_CLOSED).isEmpty());
        assertTrue(harness.eventsOf(activeViewer, ControlProtocol.EventKind.ROOM_CLOSED)
                .isEmpty());
        assertEquals(ControlProtocol.ErrorCode.NOT_READY, harness.ping(idleViewer).error());
        assertTrue(harness.ping(activeViewer).ok());
    }

    @Test
    void viewerUdpExpiryRemovesOnlyThatViewer() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long idleViewer = harness.connect();
        long activeViewer = harness.connect();
        harness.hello(publisher);
        harness.hello(idleViewer);
        harness.hello(activeViewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        ControlProtocol.RoomInfo idleJoin = harness.join(idleViewer, room.roomCode()).room();
        ControlProtocol.RoomInfo activeJoin =
                harness.join(activeViewer, room.roomCode()).room();
        assertTrue(harness.configure720(publisher, room.roomId()).ok());
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        InetSocketAddress idleEndpoint = harness.endpoint("10.0.0.1", 40001);
        InetSocketAddress activeEndpoint = harness.endpoint("10.0.0.1", 40002);
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                publisherEndpoint));
        assertNotNull(harness.register(idleViewer, room.roomId(), idleJoin.tokenHex(),
                idleEndpoint));
        assertNotNull(harness.register(activeViewer, room.roomId(), activeJoin.tokenHex(),
                activeEndpoint));

        harness.advanceMillis(14_999);
        harness.ping(publisher);
        harness.ping(idleViewer);
        harness.ping(activeViewer);
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                publisherEndpoint));
        assertNotNull(harness.register(activeViewer, room.roomId(), activeJoin.tokenHex(),
                activeEndpoint));
        harness.advanceMillis(1);
        harness.tick();
        assertEquals(1, harness.stats().rooms());
        assertEquals(2, harness.stats().members());
        assertEquals(2, harness.stats().registeredEndpoints());
        List<ControlProtocol.Event> states =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_STATE);
        assertEquals(1, states.get(states.size() - 1).viewerCount());
        assertEquals(ControlProtocol.ErrorCode.NOT_READY,
                harness.ready(idleViewer, room.roomId(), 1).error());
        assertNull(harness.register(idleViewer, room.roomId(), idleJoin.tokenHex(),
                idleEndpoint));
        List<ControlProtocol.Event> removed =
                harness.eventsOf(idleViewer, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, removed.size());
        assertEquals(ControlProtocol.CloseReason.STREAM_FAILED, removed.get(0).reason());
        assertTrue(harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_CLOSED).isEmpty());
        assertTrue(harness.eventsOf(activeViewer, ControlProtocol.EventKind.ROOM_CLOSED)
                .isEmpty());
        assertTrue(harness.ping(idleViewer).ok());
    }

    @Test
    void publisherUdpExpiryClosesRoomWithStreamFailed() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        ControlProtocol.RoomInfo joined = harness.join(viewer, room.roomCode()).room();
        assertTrue(harness.configure720(publisher, room.roomId()).ok());
        InetSocketAddress publisherEndpoint = harness.endpoint("10.0.0.1", 40000);
        InetSocketAddress viewerEndpoint = harness.endpoint("10.0.0.1", 40001);
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                publisherEndpoint));
        assertNotNull(harness.register(viewer, room.roomId(), joined.tokenHex(),
                viewerEndpoint));

        harness.advanceMillis(14_999);
        harness.ping(publisher);
        harness.ping(viewer);
        assertNotNull(harness.register(viewer, room.roomId(), joined.tokenHex(),
                viewerEndpoint));
        harness.advanceMillis(1);
        harness.tick();
        List<ControlProtocol.Event> closed =
                harness.eventsOf(viewer, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, closed.size());
        assertEquals(ControlProtocol.CloseReason.STREAM_FAILED, closed.get(0).reason());
        assertEquals(0, harness.stats().rooms());
        assertEquals(0, harness.stats().members());
    }

    @Test
    void publisherDisconnectClosesRoomImmediately() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        harness.join(viewer, room.roomCode());

        harness.close(publisher);
        List<ControlProtocol.Event> closed =
                harness.eventsOf(viewer, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, closed.size());
        assertEquals(ControlProtocol.CloseReason.OWNER_DISCONNECTED, closed.get(0).reason());
        assertEquals(0, harness.stats().rooms());
        assertEquals(1, harness.stats().sessions());
        assertTrue(harness.leave(viewer, room.roomId()).ok());
    }

    @Test
    void viewerDisconnectDoesNotCloseRoom() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long leaving = harness.connect();
        long remaining = harness.connect();
        harness.hello(publisher);
        harness.hello(leaving);
        harness.hello(remaining);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        harness.join(leaving, room.roomCode());
        harness.join(remaining, room.roomCode());

        harness.close(leaving);
        assertEquals(1, harness.stats().rooms());
        assertEquals(2, harness.stats().members());
        List<ControlProtocol.Event> states =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_STATE);
        assertEquals(1, states.get(states.size() - 1).viewerCount());
        assertTrue(harness.stop(publisher, room.roomId()).ok());
    }

    @Test
    void lastViewerLeavingKeepsPublisherRoom() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        long lateViewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        harness.hello(lateViewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        harness.join(viewer, room.roomCode());
        assertTrue(harness.leave(viewer, room.roomId()).ok());
        assertEquals(1, harness.stats().rooms());
        assertEquals(1, harness.stats().members());
        assertTrue(harness.join(lateViewer, room.roomCode()).ok());
        assertEquals(2, harness.stats().members());
    }

    @Test
    void serverStopCancelsTimerClosesRoomsAndInvalidatesCodes() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        ControlProtocol.RoomInfo joined = harness.join(viewer, room.roomCode()).room();
        assertTrue(harness.host.maintenanceScheduled());
        assertEquals(1_000, harness.host.scheduledPeriodMillis());

        harness.stop();
        assertTrue(harness.host.maintenanceCanceled());
        assertEquals(1, harness.host.scheduleCalls());
        assertEquals(0, harness.stats().rooms());
        assertEquals(0, harness.stats().members());
        assertEquals(0, harness.stats().sessions());
        List<ControlProtocol.Event> closed =
                harness.eventsOf(viewer, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, closed.size());
        assertEquals(ControlProtocol.CloseReason.SERVER_SHUTDOWN, closed.get(0).reason());
        harness.stop();

        Harness restarted = new Harness();
        long restartedViewer = restarted.connect();
        restarted.hello(restartedViewer);
        assertEquals(ControlProtocol.ErrorCode.ROOM_NOT_FOUND,
                restarted.join(restartedViewer, room.roomCode()).error());

        ControlProtocol.RoomInfo second = restarted.create(restartedViewer, "720p30").room();
        InetSocketAddress endpoint = restarted.endpoint("10.0.0.1", 40000);
        assertNull(restarted.register(restartedViewer, second.roomId(), room.tokenHex(),
                endpoint));
        assertNull(restarted.register(restartedViewer, second.roomId(),
                joined.tokenHex(), endpoint));
        assertNotNull(restarted.register(restartedViewer, second.roomId(),
                second.tokenHex(), endpoint));
    }

    @Test
    void publisherMustRegisterUdpWithinTheDeadline() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        harness.advanceMillis(4_999);
        harness.tick();
        assertEquals(1, harness.stats().rooms());
        assertTrue(harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_CLOSED).isEmpty());

        harness.advanceMillis(1);
        harness.tick();
        List<ControlProtocol.Event> closed =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, closed.size());
        assertEquals(ControlProtocol.CloseReason.STREAM_FAILED, closed.get(0).reason());
        assertEquals(0, harness.stats().rooms());
        assertEquals(0, harness.stats().members());
    }

    @Test
    void viewerMustRegisterUdpWithinTheDeadline() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        ControlProtocol.RoomInfo joined = harness.join(viewer, room.roomCode()).room();
        assertTrue(harness.configure720(publisher, room.roomId()).ok());
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                harness.endpoint("10.0.0.1", 40000)));

        harness.advanceMillis(4_999);
        harness.ping(publisher);
        harness.tick();
        assertEquals(2, harness.stats().members());
        harness.advanceMillis(1);
        harness.ping(publisher);
        harness.tick();
        assertEquals(1, harness.stats().rooms());
        assertEquals(1, harness.stats().members());
        List<ControlProtocol.Event> states =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_STATE);
        assertEquals(0, states.get(states.size() - 1).viewerCount());
        assertEquals(ControlProtocol.ErrorCode.NOT_READY,
                harness.ready(viewer, room.roomId(), 1).error());
        assertNull(harness.register(viewer, room.roomId(), joined.tokenHex(),
                harness.endpoint("10.0.0.1", 40001)));
        List<ControlProtocol.Event> removed =
                harness.eventsOf(viewer, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, removed.size());
        assertEquals(ControlProtocol.CloseReason.STREAM_FAILED, removed.get(0).reason());
        assertTrue(harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_CLOSED).isEmpty());
        assertTrue(harness.ping(viewer).ok());
    }

    @Test
    void registrationBeforeDeadlineClearsTheTimeout() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        harness.advanceMillis(4_999);
        assertNotNull(harness.register(publisher, room.roomId(), room.tokenHex(),
                harness.endpoint("10.0.0.1", 40000)));
        harness.advanceMillis(5_000);
        harness.ping(publisher);
        harness.tick();
        assertEquals(1, harness.stats().rooms());
        assertTrue(harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_CLOSED).isEmpty());
    }

    @Test
    void invalidOrRateLimitedTrafficDoesNotExtendControlActivity() {
        Tuning tuning = Tuning.builder()
                .controlIdleMillis(1_000)
                .requestsPerSecond(1)
                .requestBurst(1)
                .build();
        Harness harness = new Harness(tuning);
        long connection = harness.connect();
        assertTrue(harness.hello(connection).ok());
        harness.advanceMillis(500);
        assertEquals(ControlProtocol.ErrorCode.RATE_LIMITED,
                harness.ping(connection).error());
        harness.sendRaw(connection,
                "{\"v\":1,\"id\":99,\"op\":\"not-an-operation\",\"data\":{}}");
        harness.advanceMillis(500);
        harness.tick();
        assertEquals(0, harness.stats().sessions());
        assertEquals(ControlProtocol.ErrorCode.NOT_READY, harness.ping(connection).error());
    }
}
