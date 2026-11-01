package vusiif.ztohub.desktopshare.room;

import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Stage-5 R3 regression: with a constrained server egress bucket and a fixed
 * publisher arrival order, the shared budget must keep serving every eligible
 * room. The egress bucket, the per-viewer reservation, the host UDP budget and
 * the per-room capacity stay the only limits; fairness adds at most one turn
 * holder and one waiting room, both expiring.
 */
class RoomFairnessTest {
    private record Room1(long publisher, ControlProtocol.RoomInfo room,
                         InetSocketAddress publisherEndpoint) {}

    private record Viewer(long connection, InetSocketAddress endpoint) {}

    /** Creates a room with one registered publisher. */
    private static Room1 openRoom(Harness harness, int port) {
        long publisher = harness.connect();
        harness.hello(publisher);
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();
        InetSocketAddress endpoint = harness.endpoint("10.0.0.1", port);
        harness.register(publisher, room.roomId(), room.tokenHex(), endpoint);
        return new Room1(publisher, room, endpoint);
    }

    private static List<Viewer> addViewers(Harness harness, ControlProtocol.RoomInfo room,
            int port, int count) {
        List<Viewer> viewers = new ArrayList<>();
        for (int index = 0; index < count; index++) {
            long connection = harness.connect();
            harness.hello(connection);
            ControlProtocol.RoomInfo joined = harness.join(connection, room.roomCode()).room();
            InetSocketAddress endpoint = harness.endpoint("10.0.0.1", port + index);
            harness.register(connection, room.roomId(), joined.tokenHex(), endpoint);
            viewers.add(new Viewer(connection, endpoint));
        }
        return viewers;
    }

    private static long configureAndReady(Harness harness, Room1 room, List<Viewer> viewers) {
        long epoch = harness.configure720(room.publisher(), room.room().roomId()).streamEpoch();
        for (Viewer viewer : viewers) {
            assertTrue(harness.ready(viewer.connection(), room.room().roomId(), epoch).ok());
        }
        return epoch;
    }

    private static void send(Harness harness, Room1 room, long epoch, long sequence) {
        harness.sendVideoFragment(room.publisher(), room.publisher(), room.room().roomId(),
                room.room().tokenHex(), epoch, sequence, room.publisherEndpoint(), true, 0, 1,
                new byte[100]);
    }

    private static long forwardedTo(Harness harness, InetSocketAddress endpoint, int since) {
        return harness.udpTo(endpoint, since).size();
    }

    private static RoomController.RoomFairnessStats roomFairness(RoomController.Stats stats,
            long roomId) {
        for (RoomController.RoomFairnessStats room : stats.roomFairness()) {
            if (room.roomId() == roomId) {
                return room;
            }
        }
        throw new AssertionError("room not present in stats: " + roomId);
    }

    @Test
    void fixedArrivalOrderServesBothRoomsFromOneConstrainedBurst() {
        // One 207-byte forwarded datagram fits a 300-byte burst per refill. Room A is
        // always processed first; without cross-room fairness it would take every
        // refill and B would never forward anything.
        Harness harness = new Harness(Tuning.builder().egressBurstBytes(300).build());
        Room1 a = openRoom(harness, 42000);
        Room1 b = openRoom(harness, 43000);
        Viewer viewerA = addViewers(harness, a.room(), 42100, 1).get(0);
        Viewer viewerB = addViewers(harness, b.room(), 43100, 1).get(0);
        long epochA = configureAndReady(harness, a, List.of(viewerA));
        long epochB = configureAndReady(harness, b, List.of(viewerB));
        assertEquals(5_000_000L, harness.stats().reservedEgressBitsPerSecond());

        int beforeA = harness.udpSends();
        int beforeB = harness.udpSends();
        for (int round = 0; round < 100; round++) {
            harness.advanceMillis(1);
            send(harness, a, epochA, round);
            send(harness, b, epochB, round);
        }
        long forwardedA = forwardedTo(harness, viewerA.endpoint(), beforeA);
        long forwardedB = forwardedTo(harness, viewerB.endpoint(), beforeB);
        RoomController.Stats stats = harness.stats();
        System.out.println("A forwarded=" + forwardedA + "; B forwarded=" + forwardedB
                + "; fairYield=" + stats.fairYieldFragments()
                + "; A yield=" + roomFairness(stats, a.room().roomId()).fairYieldFragments()
                + "; B yield=" + roomFairness(stats, b.room().roomId()).fairYieldFragments());

        // Both rooms keep making progress; neither is starved by the fixed order.
        assertTrue(forwardedA > 0, "room A must keep forwarding");
        assertTrue(forwardedB > 0, "room B must not be starved by the fixed arrival order");
        assertTrue(forwardedB >= 40 && forwardedA >= 40,
                "both rooms must get a comparable share: A=" + forwardedA + " B=" + forwardedB);
        // The 300-byte burst can carry one datagram per refill, so 100 refills can
        // forward at most 100 fragments in total: fairness never enlarges the budget.
        assertTrue(forwardedA + forwardedB <= 100,
                "egress budget must stay the hard limit: " + (forwardedA + forwardedB));
        assertEquals(forwardedA + forwardedB, stats.forwardedFragments());
        assertEquals(5_000_000L, stats.reservedEgressBitsPerSecond(),
                "two viewers keep their per-viewer reservation");
        // Every withheld copy is counted per room, not silently absorbed.
        long yielded = 0;
        for (RoomController.RoomFairnessStats room : stats.roomFairness()) {
            yielded += room.fairYieldFragments();
        }
        assertEquals(stats.fairYieldFragments(), yielded);
        assertTrue(yielded > 0, "the contested refills must be visible as fair yields");
        harness.stop();
        assertEquals(0L, harness.stats().reservedEgressBitsPerSecond());
        assertTrue(harness.stats().roomFairness().isEmpty());
    }

    @Test
    void threeRoomsUnderFixedOrderAllMakeProgress() {
        Harness harness = new Harness(Tuning.builder().egressBurstBytes(300).build());
        Room1 a = openRoom(harness, 44000);
        Room1 b = openRoom(harness, 45000);
        Room1 c = openRoom(harness, 46000);
        Viewer viewerA = addViewers(harness, a.room(), 44100, 1).get(0);
        Viewer viewerB = addViewers(harness, b.room(), 45100, 1).get(0);
        Viewer viewerC = addViewers(harness, c.room(), 46100, 1).get(0);
        long epochA = configureAndReady(harness, a, List.of(viewerA));
        long epochB = configureAndReady(harness, b, List.of(viewerB));
        long epochC = configureAndReady(harness, c, List.of(viewerC));

        int before = harness.udpSends();
        for (int round = 0; round < 90; round++) {
            harness.advanceMillis(1);
            send(harness, a, epochA, round);
            send(harness, b, epochB, round);
            send(harness, c, epochC, round);
        }
        long forwardedA = forwardedTo(harness, viewerA.endpoint(), before);
        long forwardedB = forwardedTo(harness, viewerB.endpoint(), before);
        long forwardedC = forwardedTo(harness, viewerC.endpoint(), before);
        System.out.println("A=" + forwardedA + " B=" + forwardedB + " C=" + forwardedC);
        assertTrue(forwardedA >= 20 && forwardedB >= 20 && forwardedC >= 20,
                "every room must progress: A=" + forwardedA + " B=" + forwardedB
                        + " C=" + forwardedC);
        assertTrue(forwardedA + forwardedB + forwardedC <= 90,
                "the shared bucket stays the hard limit");
        assertEquals(3, harness.stats().roomFairness().size());
        harness.stop();
    }

    @Test
    void multipleViewersPerRoomKeepTheirReservationWhileRoomsShareEgress() {
        // 500-byte burst: two 207-byte copies fit, the third does not. Room A and B
        // each have two viewers, so every refill can serve two of the four copies.
        Harness harness = new Harness(Tuning.builder().egressBurstBytes(500).build());
        Room1 a = openRoom(harness, 47000);
        Room1 b = openRoom(harness, 48000);
        List<Viewer> viewersA = addViewers(harness, a.room(), 47100, 2);
        List<Viewer> viewersB = addViewers(harness, b.room(), 48100, 2);
        long epochA = configureAndReady(harness, a, viewersA);
        long epochB = configureAndReady(harness, b, viewersB);
        assertEquals(10_000_000L, harness.stats().reservedEgressBitsPerSecond());

        int before = harness.udpSends();
        for (int round = 0; round < 60; round++) {
            harness.advanceMillis(1);
            send(harness, a, epochA, round);
            send(harness, b, epochB, round);
        }
        for (RoomController.ViewerStats viewer : harness.stats().viewers()) {
            assertEquals(2_500_000L, viewer.reservedBitsPerSecond());
        }
        long forwardedA = 0;
        long forwardedB = 0;
        for (Viewer viewer : viewersA) {
            forwardedA += forwardedTo(harness, viewer.endpoint(), before);
        }
        for (Viewer viewer : viewersB) {
            forwardedB += forwardedTo(harness, viewer.endpoint(), before);
        }
        System.out.println("A copies=" + forwardedA + " B copies=" + forwardedB);
        assertTrue(forwardedA > 0 && forwardedB > 0,
                "both rooms must make progress with multiple viewers");
        assertTrue(forwardedA + forwardedB <= 120,
                "two copies per refill is the hard egress limit");
        for (RoomController.RoomFairnessStats room : harness.stats().roomFairness()) {
            assertTrue(room.forwardedFragments() > 0, "per-room forwarding is observable");
        }
        harness.stop();
    }

    @Test
    void viewerLeaveAndRoomCloseReleaseFairnessState() {
        Harness harness = new Harness(Tuning.builder().egressBurstBytes(300).build());
        Room1 a = openRoom(harness, 49000);
        Room1 b = openRoom(harness, 50000);
        Viewer viewerA = addViewers(harness, a.room(), 49100, 1).get(0);
        Viewer viewerB = addViewers(harness, b.room(), 50100, 1).get(0);
        long epochA = configureAndReady(harness, a, List.of(viewerA));
        long epochB = configureAndReady(harness, b, List.of(viewerB));

        // Round 1: A forwards, B is refused and holds the fairness demand/turn.
        harness.advanceMillis(1);
        send(harness, a, epochA, 0);
        send(harness, b, epochB, 0);
        RoomController.Stats denied = harness.stats();
        assertTrue(roomFairness(denied, b.room().roomId()).waitingForEgress()
                        || roomFairness(denied, b.room().roomId()).holdingEgressTurn(),
                "the refused room holds the turn for the next refill");

        // B's last viewer leaves: its fairness state must go with it.
        assertTrue(harness.leave(viewerB.connection(), b.room().roomId()).ok());
        RoomController.Stats afterLeave = harness.stats();
        assertEquals(2_500_000L, afterLeave.reservedEgressBitsPerSecond());
        assertEquals(2, afterLeave.roomFairness().size());
        assertFalse(roomFairness(afterLeave, b.room().roomId()).holdingEgressTurn(),
                "a room without viewers cannot keep the egress turn");
        assertFalse(roomFairness(afterLeave, b.room().roomId()).waitingForEgress());
        assertFalse(roomFairness(afterLeave, a.room().roomId()).holdingEgressTurn(),
                "a released room cannot keep the egress turn");

        // Room A still forwards after B left.
        harness.advanceMillis(1);
        int before = harness.udpSends();
        send(harness, a, epochA, 1);
        assertEquals(1, harness.udpSends() - before);

        // A profile change moves the room to a new epoch and releases its turn.
        assertTrue(harness.configure1080(a.publisher(), a.room().roomId()).ok());
        assertEquals(5_000_000L, harness.stats().reservedEgressBitsPerSecond());
        assertFalse(roomFairness(harness.stats(), a.room().roomId()).holdingEgressTurn());

        // Closing the room and stopping the controller leave no fairness state.
        assertTrue(harness.stop(a.publisher(), a.room().roomId()).ok());
        assertEquals(0L, harness.stats().reservedEgressBitsPerSecond());
        // Room B still exists through its publisher (it has no viewers left), so a
        // fair entry may remain for it, but it holds no turn and no demand.
        for (RoomController.RoomFairnessStats room : harness.stats().roomFairness()) {
            assertFalse(room.holdingEgressTurn());
            assertFalse(room.waitingForEgress());
        }
        assertTrue(harness.stop(b.publisher(), b.room().roomId()).ok());
        assertTrue(harness.stats().roomFairness().isEmpty());
        harness.stop();
        assertTrue(harness.stats().roomFairness().isEmpty());
        assertEquals(0L, harness.stats().reservedEgressBitsPerSecond());
    }

    @Test
    void sendFailureDoesNotHoldTheEgressTurnAwayFromOtherRooms() {
        Harness harness = new Harness(Tuning.builder().egressBurstBytes(300).build());
        Room1 a = openRoom(harness, 51000);
        Room1 b = openRoom(harness, 52000);
        Viewer viewerA = addViewers(harness, a.room(), 51100, 1).get(0);
        Viewer viewerB = addViewers(harness, b.room(), 52100, 1).get(0);
        long epochA = configureAndReady(harness, a, List.of(viewerA));
        long epochB = configureAndReady(harness, b, List.of(viewerB));
        // Room A's endpoint always refuses the datagram: its copies are refunded and
        // must not turn into a fairness demand that starves room B.
        harness.host.failUdpTo(viewerA.endpoint());

        int beforeB = harness.udpSends();
        for (int round = 0; round < 20; round++) {
            harness.advanceMillis(1);
            send(harness, a, epochA, round);
            send(harness, b, epochB, round);
        }
        RoomController.Stats stats = harness.stats();
        long forwardedB = 0;
        for (RoomController.ViewerStats viewer : stats.viewers()) {
            if (viewer.connectionId() == viewerB.connection()) {
                forwardedB = viewer.forwardedFragments();
            } else {
                assertEquals(0L, viewer.forwardedFragments());
                assertTrue(viewer.sendDroppedFragments() > 0,
                        "the failed room reports its own send drops");
            }
        }
        assertTrue(forwardedB > 0, "room B keeps forwarding while room A fails to send");
        assertTrue(stats.fairYieldFragments() >= 0);
        harness.host.allowUdpTo(viewerA.endpoint());
        // Once the socket path works again the failed room is served as well.
        int beforeA = harness.udpSends();
        for (int round = 0; round < 10; round++) {
            harness.advanceMillis(1);
            send(harness, a, epochA, 100 + round);
            send(harness, b, epochB, 100 + round);
        }
        assertTrue(forwardedTo(harness, viewerA.endpoint(), beforeA) > 0);
        harness.stop();
    }

    @Test
    void staleDemandExpiresAndAnIdleRoomCannotBlockOthers() {
        Harness harness = new Harness(Tuning.builder().egressBurstBytes(300).build());
        Room1 a = openRoom(harness, 53000);
        Room1 b = openRoom(harness, 54000);
        Viewer viewerA = addViewers(harness, a.room(), 53100, 1).get(0);
        Viewer viewerB = addViewers(harness, b.room(), 54100, 1).get(0);
        long epochA = configureAndReady(harness, a, List.of(viewerA));
        long epochB = configureAndReady(harness, b, List.of(viewerB));

        harness.advanceMillis(1);
        send(harness, a, epochA, 0);
        send(harness, b, epochB, 0); // refused -> demand for B
        assertTrue(roomFairness(harness.stats(), b.room().roomId()).waitingForEgress()
                || roomFairness(harness.stats(), b.room().roomId()).holdingEgressTurn());

        // Room B stops sending. Its demand must expire instead of blocking A.
        harness.advanceMillis(EgressFairness.DEMAND_TTL_NANOS / 1_000_000L + 20);
        harness.tick();
        assertFalse(roomFairness(harness.stats(), b.room().roomId()).waitingForEgress(),
                "an idle room's demand expires");
        int before = harness.udpSends();
        send(harness, a, epochA, 1);
        assertEquals(1, harness.udpSends() - before,
                "the active room is not held back by an expired demand");
        assertFalse(roomFairness(harness.stats(), b.room().roomId()).holdingEgressTurn());
        harness.stop();
    }
}
