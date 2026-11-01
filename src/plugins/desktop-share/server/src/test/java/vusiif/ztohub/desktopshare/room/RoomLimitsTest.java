package vusiif.ztohub.desktopshare.room;

import org.junit.jupiter.api.Test;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

class RoomLimitsTest {
    @Test
    void ninthViewerIsRejectedAndCapacityIsReleased() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();

        long[] viewers = new long[9];
        for (int index = 0; index < 9; index++) {
            viewers[index] = harness.connect();
            harness.hello(viewers[index]);
        }
        for (int index = 0; index < 8; index++) {
            assertTrue(harness.join(viewers[index], room.roomCode()).ok());
        }
        assertEquals(9, harness.stats().members());
        assertEquals(20_000_000L, harness.stats().reservedEgressBitsPerSecond());

        ControlProtocol.Response rejected = harness.join(viewers[8], room.roomCode());
        assertFalse(rejected.ok());
        assertEquals(ControlProtocol.ErrorCode.ROOM_FULL, rejected.error());
        assertEquals(9, harness.stats().members());
        assertEquals(20_000_000L, harness.stats().reservedEgressBitsPerSecond());

        assertTrue(harness.leave(viewers[0], room.roomId()).ok());
        assertEquals(8, harness.stats().members());
        assertEquals(17_500_000L, harness.stats().reservedEgressBitsPerSecond());
        assertTrue(harness.join(viewers[8], room.roomCode()).ok());
        assertEquals(9, harness.stats().members());
        assertEquals(20_000_000L, harness.stats().reservedEgressBitsPerSecond());
    }

    @Test
    void roomCapacityIsRejectedWithoutChangingState() {
        Harness harness = new Harness();
        long[] publishers = new long[9];
        for (int index = 0; index < 9; index++) {
            publishers[index] = harness.connect();
            harness.hello(publishers[index]);
        }
        for (int index = 0; index < 8; index++) {
            assertTrue(harness.create(publishers[index], "720p30").ok());
        }
        assertEquals(8, harness.stats().rooms());
        assertEquals(8, harness.stats().members());

        ControlProtocol.Response rejected = harness.create(publishers[8], "720p30");
        assertFalse(rejected.ok());
        assertEquals(ControlProtocol.ErrorCode.CAPACITY, rejected.error());
        assertEquals(8, harness.stats().rooms());
        assertEquals(8, harness.stats().members());

        long firstRoom = harness.create(publishers[0], "720p30").room().roomId();
        // The repeated create returns the existing room, so stop it explicitly.
        assertTrue(harness.stop(publishers[0], firstRoom).ok());
        assertEquals(7, harness.stats().rooms());
        assertTrue(harness.create(publishers[8], "720p30").ok());
        assertEquals(8, harness.stats().rooms());
    }

    @Test
    void sessionCapacityRejectsAndRecovers() {
        Tuning tuning = Tuning.builder().maxMembersPerServer(3).build();
        Harness harness = new Harness(tuning);
        long publisher = harness.connect();
        long viewerOne = harness.connect();
        long viewerTwo = harness.connect();
        long viewerThree = harness.connect();
        for (long connection : new long[] {publisher, viewerOne, viewerTwo, viewerThree}) {
            harness.hello(connection);
        }
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        assertTrue(harness.join(viewerOne, room.roomCode()).ok());
        assertTrue(harness.join(viewerTwo, room.roomCode()).ok());
        assertFalse(harness.join(viewerThree, room.roomCode()).ok());
        assertEquals(ControlProtocol.ErrorCode.CAPACITY,
                harness.join(viewerThree, room.roomCode()).error());
        assertEquals(3, harness.stats().members());

        assertTrue(harness.leave(viewerOne, room.roomId()).ok());
        assertTrue(harness.join(viewerThree, room.roomCode()).ok());
        assertEquals(3, harness.stats().members());
    }

    @Test
    void egressBudgetRejectsAndReleasesReservations() {
        Tuning tuning = Tuning.builder().egressBudgetBitsPerSecond(12_500_000L).build();
        Harness harness = new Harness(tuning);
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();

        long[] viewers = new long[6];
        for (int index = 0; index < 6; index++) {
            viewers[index] = harness.connect();
            harness.hello(viewers[index]);
        }
        for (int index = 0; index < 5; index++) {
            assertTrue(harness.join(viewers[index], room.roomCode()).ok());
        }
        assertEquals(12_500_000L, harness.stats().reservedEgressBitsPerSecond());
        ControlProtocol.Response rejected = harness.join(viewers[5], room.roomCode());
        assertFalse(rejected.ok());
        assertEquals(ControlProtocol.ErrorCode.CAPACITY, rejected.error());
        assertEquals(6, harness.stats().members());

        assertTrue(harness.leave(viewers[0], room.roomId()).ok());
        assertEquals(10_000_000L, harness.stats().reservedEgressBitsPerSecond());
        assertTrue(harness.join(viewers[5], room.roomCode()).ok());
        assertEquals(12_500_000L, harness.stats().reservedEgressBitsPerSecond());

        assertTrue(harness.stop(publisher, room.roomId()).ok());
        assertEquals(0, harness.stats().members());
        assertEquals(0L, harness.stats().reservedEgressBitsPerSecond());
    }

    @Test
    void joinFailuresAreLimitedPerSourceAcrossConnectionsAndExpireAtBoundary() {
        Harness harness = new Harness();
        harness.codes.enqueue("00000001");
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();

        for (int index = 0; index < 10; index++) {
            long attacker = harness.connect("10.0.0.2");
            harness.hello(attacker);
            ControlProtocol.Response failed = harness.join(attacker, "99999999");
            assertFalse(failed.ok());
            assertEquals(ControlProtocol.ErrorCode.ROOM_NOT_FOUND, failed.error());
        }
        long blocked = harness.connect("10.0.0.2");
        harness.hello(blocked);
        ControlProtocol.Response limited = harness.join(blocked, room.roomCode());
        assertFalse(limited.ok());
        assertEquals(ControlProtocol.ErrorCode.RATE_LIMITED, limited.error());
        assertEquals(1, harness.stats().rooms());

        long otherSource = harness.connect("10.0.0.3");
        harness.hello(otherSource);
        assertTrue(harness.join(otherSource, room.roomCode()).ok());

        // Exactly one minute after the last failure the window is empty again.
        harness.advanceMillis(60_000);
        long recovered = harness.connect("10.0.0.2");
        harness.hello(recovered);
        assertTrue(harness.join(recovered, room.roomCode()).ok());
    }

    @Test
    void fullJoinFailureTableRejectsNewSourcesWithoutEvictingActiveEntries() {
        Tuning tuning = Tuning.builder()
                .joinFailureSources(4)
                .joinFailureLimit(10)
                .joinFailureWindowMillis(60_000)
                .joinFailureIdleMillis(120_000)
                .build();
        Harness harness = new Harness(tuning);
        harness.codes.enqueue("00000001");
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();

        for (int index = 1; index <= 4; index++) {
            long source = harness.connect("10.0.1." + index);
            harness.hello(source);
            assertEquals(ControlProtocol.ErrorCode.ROOM_NOT_FOUND,
                    harness.join(source, "99999999").error());
        }
        assertEquals(4, harness.stats().joinFailureSources());

        long fifth = harness.connect("10.0.1.5");
        harness.hello(fifth);
        ControlProtocol.Response tableFull = harness.join(fifth, room.roomCode());
        assertFalse(tableFull.ok());
        assertEquals(ControlProtocol.ErrorCode.CAPACITY, tableFull.error());
        assertEquals(4, harness.stats().joinFailureSources());

        // An already tracked source is not evicted to make room for a new one.
        long active = harness.connect("10.0.1.1");
        harness.hello(active);
        assertTrue(harness.join(active, room.roomCode()).ok());

        // After the two-minute idle window the table can accept new sources again.
        harness.advanceMillis(120_000);
        long sixth = harness.connect("10.0.1.6");
        harness.hello(sixth);
        assertTrue(harness.join(sixth, room.roomCode()).ok());
    }

    @Test
    void successfulJoinAndReconnectDoNotClearFailureWindow() {
        Harness harness = new Harness();
        harness.codes.enqueue("00000001");
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        String source = "10.0.9.2";

        for (int index = 0; index < 9; index++) {
            long failed = harness.connect(source);
            harness.hello(failed);
            assertEquals(ControlProtocol.ErrorCode.ROOM_NOT_FOUND,
                    harness.join(failed, "99999999").error());
        }
        long successful = harness.connect(source);
        harness.hello(successful);
        assertTrue(harness.join(successful, room.roomCode()).ok());
        assertTrue(harness.leave(successful, room.roomId()).ok());

        long tenth = harness.connect(source);
        harness.hello(tenth);
        assertEquals(ControlProtocol.ErrorCode.ROOM_NOT_FOUND,
                harness.join(tenth, "99999999").error());

        long eleventh = harness.connect(source);
        harness.hello(eleventh);
        assertEquals(ControlProtocol.ErrorCode.RATE_LIMITED,
                harness.join(eleventh, "99999999").error());

        long unrelated = harness.connect("10.0.9.3");
        harness.hello(unrelated);
        assertTrue(harness.join(unrelated, room.roomCode()).ok());
        assertEquals(1, harness.stats().rooms());
    }

    @Test
    void roomCodeCollisionRetriesAndFailsWithoutOverwritingExistingRoom() {
        Harness harness = new Harness();
        harness.codes.enqueue("00000001", "00000001", "00000002");
        long first = harness.connect();
        long second = harness.connect();
        harness.hello(first);
        harness.hello(second);
        ControlProtocol.RoomInfo roomOne = harness.create(first, "720p30").room();
        assertEquals("00000001", roomOne.roomCode());
        ControlProtocol.RoomInfo roomTwo = harness.create(second, "720p30").room();
        assertEquals("00000002", roomTwo.roomCode());
        assertEquals(3, harness.codes.calls);
        assertEquals(2, harness.stats().rooms());
        assertEquals(2, harness.stats().members());
        ControlProtocol.Response stillOne = harness.create(first, "720p30");
        assertEquals("00000001", stillOne.room().roomCode());

        Harness collision = new Harness();
        collision.codes.enqueue("00000001");
        long owner = collision.connect();
        long blocked = collision.connect();
        collision.hello(owner);
        collision.hello(blocked);
        assertEquals("00000001",
                collision.create(owner, "720p30").room().roomCode());
        collision.codes.resetCalls();
        collision.codes.always("00000001");
        ControlProtocol.Response failed = collision.create(blocked, "720p30");
        assertFalse(failed.ok());
        assertEquals(ControlProtocol.ErrorCode.INTERNAL, failed.error());
        assertEquals(32, collision.codes.calls);
        assertEquals(1, collision.stats().rooms());
        assertEquals(1, collision.stats().members());

        collision.codes.clearAlways();
        assertTrue(collision.create(blocked, "720p30").ok());
        assertEquals(2, collision.stats().rooms());
        assertEquals(2, collision.stats().members());
    }

    @Test
    void controlRateLimitAllowsDocumentedBurstThenRecovers() {
        Harness harness = new Harness();
        long connection = harness.connect();
        harness.hello(connection);
        for (int index = 0; index < 19; index++) {
            assertTrue(harness.ping(connection).ok());
        }
        ControlProtocol.Response limited = harness.ping(connection);
        assertFalse(limited.ok());
        assertEquals(ControlProtocol.ErrorCode.RATE_LIMITED, limited.error());
        harness.advanceMillis(49);
        assertEquals(ControlProtocol.ErrorCode.RATE_LIMITED,
                harness.ping(connection).error());
        harness.advanceMillis(1_000);
        assertTrue(harness.ping(connection).ok());
    }

    @Test
    void failedJoinsDoNotLeakMembersOrReservations() {
        Harness harness = new Harness();
        long publisher = harness.connect();
        long viewer = harness.connect();
        harness.hello(publisher);
        harness.hello(viewer);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        for (int index = 0; index < 5; index++) {
            long source = harness.connect("10.0.2." + (index + 1));
            harness.hello(source);
            assertFalse(harness.join(source, "99999999").ok());
        }
        RoomController.Stats stats = harness.stats();
        assertEquals(1, stats.rooms());
        assertEquals(1, stats.members());
        assertEquals(0L, stats.reservedEgressBitsPerSecond());

        assertTrue(harness.join(viewer, room.roomCode()).ok());
        assertEquals(2, harness.stats().members());
        assertEquals(2_500_000L, harness.stats().reservedEgressBitsPerSecond());
    }
}
