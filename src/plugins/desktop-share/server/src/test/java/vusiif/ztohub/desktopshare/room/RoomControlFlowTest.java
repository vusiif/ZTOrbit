package vusiif.ztohub.desktopshare.room;

import java.util.List;
import org.junit.jupiter.api.Test;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

class RoomControlFlowTest {
    @Test
    void createJoinConfigureReadyLeaveStopRoundTrip() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();

        ControlProtocol.Response hello = harness.hello(publisher);
        assertTrue(hello.ok());
        assertEquals(publisher, hello.hello().sessionId());
        assertEquals(18100, hello.hello().udpPort());
        assertEquals(List.of("720p30", "1080p30"), hello.hello().profiles());
        assertEquals(8, hello.hello().maxViewers());
        harness.hello(viewer);

        ControlProtocol.Response created = harness.create(publisher, "720p30");
        assertTrue(created.ok());
        ControlProtocol.RoomInfo room = created.room();
        assertEquals(ControlProtocol.Role.PUBLISHER, room.role());
        assertEquals("720p30", room.profile());
        assertTrue(room.roomCode().matches("[0-9]{8}"), room.roomCode());
        assertTrue(room.tokenHex().matches("[0-9a-f]{32}"));
        assertNull(room.format());
        String code = room.roomCode();
        long roomId = room.roomId();
        String publisherToken = room.tokenHex();

        ControlProtocol.Response joined = harness.join(viewer, code);
        assertTrue(joined.ok());
        ControlProtocol.RoomInfo viewerRoom = joined.room();
        assertEquals(ControlProtocol.Role.VIEWER, viewerRoom.role());
        assertEquals(roomId, viewerRoom.roomId());
        assertEquals(code, viewerRoom.roomCode());
        assertNull(viewerRoom.format());
        String viewerToken = viewerRoom.tokenHex();
        assertNotEquals(publisherToken, viewerToken);

        List<ControlProtocol.Event> preparing =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_STATE);
        assertEquals(1, preparing.size());
        assertEquals(ControlProtocol.RoomState.PREPARING, preparing.get(0).roomState());
        assertEquals(1, preparing.get(0).viewerCount());

        ControlProtocol.Response configured = harness.configure720(publisher, roomId);
        assertTrue(configured.ok());
        assertEquals(1, configured.streamEpoch());

        List<ControlProtocol.Event> configuredEvents =
                harness.eventsOf(viewer, ControlProtocol.EventKind.STREAM_CONFIG);
        assertEquals(1, configuredEvents.size());
        ControlProtocol.StreamFormat format = configuredEvents.get(0).format();
        assertNotNull(format);
        assertEquals(1, format.streamEpoch());
        assertEquals(1280, format.width());
        assertEquals(720, format.height());
        assertEquals(30, format.fps());
        assertEquals(2_000_000, format.bitrate());
        assertEquals(0, format.initializationData().length);

        List<ControlProtocol.Event> running =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_STATE);
        assertEquals(1, running.size());
        assertEquals(ControlProtocol.RoomState.RUNNING, running.get(0).roomState());
        assertEquals(1, running.get(0).viewerCount());

        assertTrue(harness.ready(viewer, roomId, 1).ok());
        assertTrue(harness.leave(viewer, roomId).ok());
        List<ControlProtocol.Event> afterLeave =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_STATE);
        assertEquals(1, afterLeave.size());
        assertEquals(0, afterLeave.get(0).viewerCount());

        ControlProtocol.Response rejoined = harness.join(viewer, code);
        assertTrue(rejoined.ok());
        assertNotNull(rejoined.room().format());
        assertEquals(1, rejoined.room().format().streamEpoch());
        assertNotEquals(viewerToken, rejoined.room().tokenHex());

        assertTrue(harness.stop(publisher, roomId).ok());
        List<ControlProtocol.Event> publisherClosed =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, publisherClosed.size());
        assertEquals(ControlProtocol.CloseReason.OWNER_STOPPED, publisherClosed.get(0).reason());
        List<ControlProtocol.Event> viewerClosed =
                harness.eventsOf(viewer, ControlProtocol.EventKind.ROOM_CLOSED);
        assertEquals(1, viewerClosed.size());
        assertEquals(ControlProtocol.CloseReason.OWNER_STOPPED, viewerClosed.get(0).reason());

        RoomController.Stats stats = harness.stats();
        assertEquals(0, stats.rooms());
        assertEquals(0, stats.members());
        assertEquals(0L, stats.reservedEgressBitsPerSecond());
        assertEquals(2, stats.sessions());

        ControlProtocol.Response oldCode = harness.join(viewer, code);
        assertFalse(oldCode.ok());
        assertEquals(ControlProtocol.ErrorCode.ROOM_NOT_FOUND, oldCode.error());

        assertRedacted(harness, code, publisherToken, viewerToken);
    }

    @Test
    void leadingZeroCodeWorksOnlyOnItsOwnServer() {
        Harness serverA = new Harness();
        serverA.codes.enqueue("00000001");
        long publisher = serverA.connect();
        serverA.hello(publisher);
        ControlProtocol.Response created = serverA.create(publisher, "720p30");
        assertTrue(created.ok());
        assertEquals("00000001", created.room().roomCode());

        Harness serverB = new Harness();
        long viewer = serverB.connect();
        serverB.hello(viewer);
        ControlProtocol.Response wrongServer = serverB.join(viewer, "00000001");
        assertFalse(wrongServer.ok());
        assertEquals(ControlProtocol.ErrorCode.ROOM_NOT_FOUND, wrongServer.error());

        long secondViewer = serverA.connect();
        serverA.hello(secondViewer);
        assertTrue(serverA.join(secondViewer, "00000001").ok());
        assertEquals(2, serverA.stats().members());
        assertEquals(0, serverB.stats().rooms());
    }

    @Test
    void repeatedCreateAndJoinReturnCurrentState() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        long third = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        harness.hello(third);

        ControlProtocol.Response first = harness.create(publisher, "720p30");
        ControlProtocol.Response repeat = harness.create(publisher, "1080p30");
        assertTrue(repeat.ok());
        assertEquals(first.room().roomId(), repeat.room().roomId());
        assertEquals(first.room().roomCode(), repeat.room().roomCode());
        assertEquals(first.room().tokenHex(), repeat.room().tokenHex());
        assertEquals("720p30", repeat.room().profile());

        ControlProtocol.Response joined = harness.join(viewer, first.room().roomCode());
        ControlProtocol.Response repeatJoin =
                harness.join(viewer, first.room().roomCode());
        assertTrue(repeatJoin.ok());
        assertEquals(joined.room().roomId(), repeatJoin.room().roomId());
        assertEquals(joined.room().tokenHex(), repeatJoin.room().tokenHex());

        ControlProtocol.Response secondRoom = harness.create(third, "1080p30");
        assertTrue(secondRoom.ok());
        ControlProtocol.Response otherRoom =
                harness.join(viewer, secondRoom.room().roomCode());
        assertFalse(otherRoom.ok());
        assertEquals(ControlProtocol.ErrorCode.ALREADY_IN_ROOM, otherRoom.error());

        ControlProtocol.Response publisherJoin =
                harness.join(publisher, first.room().roomCode());
        assertFalse(publisherJoin.ok());
        assertEquals(ControlProtocol.ErrorCode.ALREADY_IN_ROOM, publisherJoin.error());

        ControlProtocol.Response viewerCreate = harness.create(viewer, "720p30");
        assertFalse(viewerCreate.ok());
        assertEquals(ControlProtocol.ErrorCode.ALREADY_IN_ROOM, viewerCreate.error());

        assertEquals(2, harness.stats().rooms());
        assertEquals(3, harness.stats().members());
    }

    @Test
    void rolePermissionsAndStaleRoomIdsKeepCurrentState() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        harness.join(viewer, room.roomCode());

        assertEquals(ControlProtocol.ErrorCode.STALE_STREAM,
                harness.leave(viewer, room.roomId() + 1).error());
        assertEquals(2, harness.stats().members());

        assertEquals(ControlProtocol.ErrorCode.FORBIDDEN,
                harness.leave(publisher, room.roomId()).error());
        assertEquals(ControlProtocol.ErrorCode.FORBIDDEN,
                harness.stop(viewer, room.roomId()).error());
        assertEquals(ControlProtocol.ErrorCode.STALE_STREAM,
                harness.stop(publisher, room.roomId() + 1).error());
        assertEquals(1, harness.stats().rooms());

        assertEquals(ControlProtocol.ErrorCode.FORBIDDEN,
                harness.configure720(viewer, room.roomId()).error());
        assertEquals(ControlProtocol.ErrorCode.FORBIDDEN,
                harness.ready(publisher, room.roomId(), 1).error());
        assertEquals(ControlProtocol.ErrorCode.FORBIDDEN,
                harness.keyframe(publisher, room.roomId(), 1).error());
        assertEquals(ControlProtocol.ErrorCode.FORBIDDEN,
                harness.feedback(publisher, room.roomId(), 1).error());
        assertEquals(ControlProtocol.ErrorCode.NOT_READY,
                harness.ready(viewer, room.roomId(), 1).error());
        assertEquals(ControlProtocol.ErrorCode.NOT_READY,
                harness.keyframe(viewer, room.roomId(), 1).error());
        assertEquals(ControlProtocol.ErrorCode.NOT_READY,
                harness.feedback(viewer, room.roomId(), 1).error());
        assertEquals(2, harness.stats().members());
    }

    @Test
    void idleLeaveAndStopAreIdempotent() {
        Harness harness = new Harness();
        long connection = harness.connect();
        harness.hello(connection);
        assertTrue(harness.leave(connection, 999).ok());
        assertTrue(harness.leave(connection, 999).ok());
        assertTrue(harness.stop(connection, 999).ok());
        assertTrue(harness.stop(connection, 999).ok());
        assertEquals(0, harness.stats().rooms());
        assertEquals(0, harness.stats().members());
    }

    @Test
    void staleStopCannotCloseANewRoom() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long other = harness.connect();
        harness.hello(publisher);
        harness.hello(other);
        ControlProtocol.RoomInfo first = harness.create(publisher, "720p30").room();
        assertTrue(harness.stop(publisher, first.roomId()).ok());

        ControlProtocol.RoomInfo second = harness.create(publisher, "720p30").room();
        assertNotEquals(first.roomId(), second.roomId());
        assertEquals(ControlProtocol.ErrorCode.STALE_STREAM,
                harness.stop(publisher, first.roomId()).error());
        assertEquals(1, harness.stats().rooms());
        assertEquals(1, harness.stats().members());

        assertTrue(harness.stop(other, second.roomId()).ok());
        assertEquals(1, harness.stats().rooms());
        assertTrue(harness.stop(publisher, second.roomId()).ok());
        assertEquals(0, harness.stats().rooms());
    }

    @Test
    void configureMatchesRoomProfileAndIssuesNewEpochs() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        harness.join(viewer, room.roomCode());

        ControlProtocol.Response wrongProfile =
                harness.configure1080(publisher, room.roomId());
        assertFalse(wrongProfile.ok());
        assertEquals(ControlProtocol.ErrorCode.INVALID_REQUEST, wrongProfile.error());
        assertEquals(ControlProtocol.ErrorCode.NOT_READY,
                harness.ready(viewer, room.roomId(), 1).error());

        ControlProtocol.Response first = harness.configure720(publisher, room.roomId());
        assertTrue(first.ok());
        assertEquals(1, first.streamEpoch());
        assertTrue(harness.ready(viewer, room.roomId(), 1).ok());
        assertNull(first(harness.eventsOf(publisher,
                ControlProtocol.EventKind.STREAM_KEYFRAME)));
        assertTrue(harness.keyframe(viewer, room.roomId(), 1).ok());
        ControlProtocol.Event notified = first(harness.eventsOf(publisher,
                ControlProtocol.EventKind.STREAM_KEYFRAME));
        assertNotNull(notified);
        assertEquals(1, notified.streamEpoch());

        assertTrue(harness.feedback(viewer, room.roomId(), 1).ok());
        ControlProtocol.Response early = harness.feedback(viewer, room.roomId(), 1);
        assertEquals(ControlProtocol.ErrorCode.RATE_LIMITED, early.error());
        // The server's window is measured between its own processing instants and
        // is not relaxed: 999 ms is still too frequent, 1000 ms is accepted. The
        // message carries the measured interval so a client can tell a genuinely
        // too frequent reporter from a compressed network trip.
        assertTrue(early.message().contains("间隔 0 ms"), early.message());
        harness.advanceMillis(999);
        ControlProtocol.Response boundary = harness.feedback(viewer, room.roomId(), 1);
        assertEquals(ControlProtocol.ErrorCode.RATE_LIMITED, boundary.error());
        assertTrue(boundary.message().contains("间隔 999 ms"), boundary.message());
        harness.advanceMillis(1);
        assertTrue(harness.feedback(viewer, room.roomId(), 1).ok());

        ControlProtocol.Response second = harness.configure720(publisher, room.roomId());
        assertTrue(second.ok());
        assertEquals(2, second.streamEpoch());
        List<ControlProtocol.Event> reconfigured =
                harness.eventsOf(viewer, ControlProtocol.EventKind.STREAM_CONFIG);
        assertEquals(2, reconfigured.size());
        assertEquals(1, reconfigured.get(0).format().streamEpoch());
        assertEquals(2, reconfigured.get(1).format().streamEpoch());
        assertEquals(ControlProtocol.ErrorCode.STALE_STREAM,
                harness.ready(viewer, room.roomId(), 1).error());
        assertEquals(ControlProtocol.ErrorCode.STALE_STREAM,
                harness.keyframe(viewer, room.roomId(), 1).error());
        assertTrue(harness.ready(viewer, room.roomId(), 2).ok());
        assertTrue(harness.keyframe(viewer, room.roomId(), 2).ok());

        RoomController.Stats stats = harness.stats();
        assertEquals(2, stats.members());
        assertEquals(2_500_000L, stats.reservedEgressBitsPerSecond());
    }

    @Test
    void profileChangeAfterRunningUpdatesEgressAndFormat() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        harness.join(viewer, room.roomCode());
        assertTrue(harness.configure720(publisher, room.roomId()).ok());
        assertTrue(harness.ready(viewer, room.roomId(), 1).ok());

        ControlProtocol.Response changed = harness.configure1080(publisher, room.roomId());
        assertTrue(changed.ok());
        assertEquals(2, changed.streamEpoch());
        List<ControlProtocol.Event> configEvents =
                harness.eventsOf(viewer, ControlProtocol.EventKind.STREAM_CONFIG);
        assertEquals(2, configEvents.size());
        assertEquals(1920, configEvents.get(1).format().width());
        assertEquals(2, configEvents.get(1).format().streamEpoch());
        assertEquals(5_000_000L, harness.stats().reservedEgressBitsPerSecond());
        assertEquals(ControlProtocol.ErrorCode.STALE_STREAM,
                harness.ready(viewer, room.roomId(), 1).error());
        assertTrue(harness.ready(viewer, room.roomId(), 2).ok());

        ControlProtocol.Response current = harness.join(viewer, room.roomCode());
        assertTrue(current.ok());
        assertEquals("1080p30", current.room().profile());
        assertEquals(1920, current.room().format().width());

        ControlProtocol.Response back = harness.configure720(publisher, room.roomId());
        assertTrue(back.ok());
        assertEquals(3, back.streamEpoch());
        assertEquals(2_500_000L, harness.stats().reservedEgressBitsPerSecond());
    }

    @Test
    void profileChangeIsRejectedWhenEgressCannotCoverViewers() {
        Harness harness = new Harness(
                Tuning.builder().egressBudgetBitsPerSecond(10_000_000L).build());
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        long[] viewers = new long[4];
        for (int index = 0; index < 4; index++) {
            viewers[index] = harness.connect();
            harness.hello(viewers[index]);
            assertTrue(harness.join(viewers[index], room.roomCode()).ok());
        }
        assertEquals(10_000_000L, harness.stats().reservedEgressBitsPerSecond());
        assertTrue(harness.configure720(publisher, room.roomId()).ok());
        assertTrue(harness.ready(viewers[0], room.roomId(), 1).ok());

        ControlProtocol.Response rejected = harness.configure1080(publisher,
                room.roomId());
        assertFalse(rejected.ok());
        assertEquals(ControlProtocol.ErrorCode.CAPACITY, rejected.error());
        assertEquals(10_000_000L, harness.stats().reservedEgressBitsPerSecond());
        assertTrue(harness.ready(viewers[0], room.roomId(), 1).ok());
        ControlProtocol.Response current = harness.join(viewers[0], room.roomCode());
        assertEquals("720p30", current.room().profile());
        assertEquals(1, current.room().format().streamEpoch());
    }

    @Test
    void viewerCanJoinWhilePublisherPrepares() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();

        ControlProtocol.Response joined = harness.join(viewer, room.roomCode());
        assertTrue(joined.ok());
        assertNull(joined.room().format());
        List<ControlProtocol.Event> states =
                harness.eventsOf(publisher, ControlProtocol.EventKind.ROOM_STATE);
        assertEquals(ControlProtocol.RoomState.PREPARING, states.get(0).roomState());

        assertTrue(harness.configure720(publisher, room.roomId()).ok());
        List<ControlProtocol.Event> viewerEvents = harness.events(viewer);
        boolean sawConfig = viewerEvents.stream()
                .anyMatch(event -> event.kind() == ControlProtocol.EventKind.STREAM_CONFIG);
        boolean sawRunning = viewerEvents.stream()
                .anyMatch(event -> event.kind() == ControlProtocol.EventKind.ROOM_STATE
                        && event.roomState() == ControlProtocol.RoomState.RUNNING);
        assertTrue(sawConfig);
        assertTrue(sawRunning);
    }

    private static ControlProtocol.Event first(List<ControlProtocol.Event> events) {
        return events.isEmpty() ? null : events.get(0);
    }

    private static void assertRedacted(Harness harness, String code, String... tokens) {
        for (String log : harness.host.logs) {
            assertFalse(log.contains(code), "log leaked room code: " + log);
            for (String token : tokens) {
                assertFalse(log.contains(token), "log leaked token: " + log);
            }
        }
    }
}
