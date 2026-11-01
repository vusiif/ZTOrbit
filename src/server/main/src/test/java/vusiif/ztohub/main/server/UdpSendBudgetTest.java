package vusiif.ztohub.main.server;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Deterministic hard-limit and release-once tests for the shared UDP budget. The
 * fixture makes the writes hang, so the only thing that can let a second datagram
 * through is a counter going back down, not the OS accepting the send.
 */
class UdpSendBudgetTest {
    @Test
    void byteCapPlateausUnderConcurrentReservations() throws Exception {
        UdpSendBudget budget = new UdpSendBudget();
        int cost = 1200 + UdpSendBudget.PER_PACKET_OVERHEAD_BYTES;
        int expected = UdpSendBudget.MAX_INFLIGHT_BYTES / cost;
        int threads = 8;
        int attempts = 300;
        AtomicInteger accepted = new AtomicInteger();
        AtomicInteger rejected = new AtomicInteger();
        List<UdpSendBudget.Reservation> held = new ArrayList<>();
        Object lock = new Object();
        CountDownLatch start = new CountDownLatch(1);
        List<Thread> workers = new ArrayList<>();
        for (int index = 0; index < threads; index++) {
            Thread worker = new Thread(() -> {
                try {
                    start.await();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return;
                }
                for (int attempt = 0; attempt < attempts; attempt++) {
                    UdpSendBudget.Reservation reservation = budget.reserve(1200);
                    if (reservation == null) {
                        rejected.incrementAndGet();
                    } else {
                        accepted.incrementAndGet();
                        synchronized (lock) {
                            held.add(reservation);
                        }
                    }
                }
            }, "udp-budget-" + index);
            workers.add(worker);
            worker.start();
        }
        start.countDown();
        for (Thread worker : workers) {
            worker.join();
        }

        assertEquals(expected, accepted.get(), "the byte cap must admit exactly its capacity");
        assertEquals(threads * attempts - expected, rejected.get());
        UdpSendBudget.Snapshot saturated = budget.snapshot();
        assertTrue(saturated.inflightBytes() <= UdpSendBudget.MAX_INFLIGHT_BYTES);
        assertTrue(saturated.inflightPackets() <= UdpSendBudget.MAX_INFLIGHT_PACKETS);
        assertEquals((long) expected * cost, saturated.inflightBytes());
        assertEquals(expected, saturated.inflightPackets());
        assertEquals(saturated.inflightBytes(), saturated.peakBytes());
        assertEquals(saturated.inflightPackets(), saturated.peakPackets());

        // A plateau: continued attempts change neither counter nor the peak.
        for (int index = 0; index < 100; index++) {
            assertNull(budget.reserve(1200));
        }
        UdpSendBudget.Snapshot plateau = budget.snapshot();
        assertEquals(saturated.inflightBytes(), plateau.inflightBytes());
        assertEquals(saturated.inflightPackets(), plateau.inflightPackets());
        assertEquals(saturated.peakBytes(), plateau.peakBytes());
        assertEquals(saturated.rejected() + 100, plateau.rejected());

        for (UdpSendBudget.Reservation reservation : held) {
            reservation.release();
        }
        UdpSendBudget.Snapshot drained = budget.snapshot();
        assertEquals(0, drained.inflightBytes());
        assertEquals(0, drained.inflightPackets());
        assertEquals(0, drained.outstanding());
        assertEquals(saturated.peakBytes(), drained.peakBytes());
    }

    @Test
    void packetCapAppliesEvenWhenBytesAreFarBelowTheLimit() {
        UdpSendBudget budget = new UdpSendBudget();
        List<UdpSendBudget.Reservation> held = new ArrayList<>();
        for (int index = 0; index < UdpSendBudget.MAX_INFLIGHT_PACKETS; index++) {
            UdpSendBudget.Reservation reservation = budget.reserve(0);
            assertNotNull(reservation);
            held.add(reservation);
        }
        UdpSendBudget.Snapshot packetsFull = budget.snapshot();
        assertEquals(UdpSendBudget.MAX_INFLIGHT_PACKETS, packetsFull.inflightPackets());
        assertEquals((long) UdpSendBudget.MAX_INFLIGHT_PACKETS
                * UdpSendBudget.PER_PACKET_OVERHEAD_BYTES, packetsFull.inflightBytes());
        assertTrue(packetsFull.inflightBytes() < UdpSendBudget.MAX_INFLIGHT_BYTES);

        // One more minimum-sized packet is rejected by the packet counter, not the byte
        // counter: checking the two counters independently would have admitted it.
        assertNull(budget.reserve(0));
        assertEquals(UdpSendBudget.MAX_INFLIGHT_PACKETS, budget.snapshot().inflightPackets());
        assertTrue(budget.snapshot().inflightBytes() < UdpSendBudget.MAX_INFLIGHT_BYTES);

        for (UdpSendBudget.Reservation reservation : held) {
            reservation.release();
        }
        assertEquals(0, budget.snapshot().inflightPackets());
    }

    @Test
    void releaseIsIdempotentAndStopRejectsAndDrains() {
        UdpSendBudget budget = new UdpSendBudget();
        UdpSendBudget.Reservation reservation = budget.reserve(1234);
        assertNotNull(reservation);
        long charge = 1234L + UdpSendBudget.PER_PACKET_OVERHEAD_BYTES;
        assertEquals(charge, budget.snapshot().inflightBytes());

        reservation.release();
        reservation.release();
        assertEquals(0, budget.snapshot().inflightBytes());
        assertEquals(0, budget.snapshot().inflightPackets());

        UdpSendBudget.Reservation next = budget.reserve(1);
        assertNotNull(next);
        budget.beginStop();
        assertNull(budget.reserve(1), "stop must reject new reservations");
        assertEquals(1, budget.snapshot().rejected());
        assertFalse(budget.snapshot().accepting());

        assertEquals(1, budget.forceReleaseAll());
        assertEquals(0, budget.forceReleaseAll());
        UdpSendBudget.Snapshot stopped = budget.snapshot();
        assertEquals(0, stopped.inflightBytes());
        assertEquals(0, stopped.inflightPackets());
        assertEquals(0, stopped.outstanding());
        assertNull(budget.reserve(1));
    }
}
