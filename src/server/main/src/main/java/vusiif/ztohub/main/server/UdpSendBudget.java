package vusiif.ztohub.main.server;

import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Global in-flight budget shared by every plugin that calls
 * {@link vusiif.ztohub.api.HostServices#sendUdp}. The budget is reserved
 * before a write is handed to the Netty event loop, so the event-loop task queue
 * itself is bounded by the same limits instead of becoming an unbounded buffer.
 *
 * <p>One reservation costs the complete ZTS1 datagram length plus a fixed
 * {@link #PER_PACKET_OVERHEAD_BYTES} per-packet overhead. Both the byte limit and
 * the packet limit are hard: a reservation succeeds only when both counters keep
 * their limit after the increment. Checking the two counters separately or only
 * looking at {@code Channel.isWritable()} cannot provide that guarantee.
 *
 * <p>Every reservation is released exactly once by the write completion path
 * (success, failure, cancellation or submission error). {@link #beginStop()}
 * rejects new reservations and {@link #forceReleaseAll()} ends writes accepted
 * before the stop so the counters return to zero.
 */
public final class UdpSendBudget {
    /** Hard limit for bytes currently reserved across all plugins. */
    public static final int MAX_INFLIGHT_BYTES = 1024 * 1024;

    /** Hard limit for datagrams currently reserved across all plugins. */
    public static final int MAX_INFLIGHT_PACKETS = 1024;

    /** Fixed per-datagram reservation cost in addition to the ZTS1 bytes. */
    public static final int PER_PACKET_OVERHEAD_BYTES = 256;

    private final Set<Reservation> outstanding = ConcurrentHashMap.newKeySet();
    private long bytes;
    private long packets;
    private long peakBytes;
    private long peakPackets;
    private long accepted;
    private long rejected;
    private boolean accepting = true;

    /**
     * Reserves one datagram. Returns {@code null} when the host is stopping or when
     * either hard limit would be exceeded; the caller must then drop the datagram.
     */
    public Reservation reserve(int zts1Bytes) {
        if (zts1Bytes < 0) {
            throw new IllegalArgumentException("zts1Bytes must not be negative");
        }
        long cost = (long) zts1Bytes + PER_PACKET_OVERHEAD_BYTES;
        synchronized (this) {
            if (!accepting) {
                ++rejected;
                return null;
            }
            if (packets + 1 > MAX_INFLIGHT_PACKETS
                    || bytes + cost > MAX_INFLIGHT_BYTES) {
                ++rejected;
                return null;
            }
            bytes += cost;
            ++packets;
            if (bytes > peakBytes) {
                peakBytes = bytes;
            }
            if (packets > peakPackets) {
                peakPackets = packets;
            }
            ++accepted;
            Reservation reservation = new Reservation(cost);
            outstanding.add(reservation);
            return reservation;
        }
    }

    /** Rejects every new reservation. Already accepted writes still complete and release. */
    public void beginStop() {
        synchronized (this) {
            accepting = false;
        }
    }

    /**
     * Releases reservations that were accepted but whose write task can no longer run
     * (for example after the UDP channel and event loop are gone). Idempotent.
     *
     * @return the number of reservations released by this call
     */
    public int forceReleaseAll() {
        int released = 0;
        for (Reservation reservation : outstanding.toArray(new Reservation[0])) {
            if (reservation.releaseInternal()) {
                ++released;
            }
        }
        return released;
    }

    /** Point-in-time counters. Contains no endpoint, payload or plugin identity. */
    public Snapshot snapshot() {
        synchronized (this) {
            return new Snapshot(bytes, packets, peakBytes, peakPackets, accepted, rejected,
                    outstanding.size(), accepting);
        }
    }

    private void release(Reservation reservation) {
        synchronized (this) {
            if (!reservation.released.compareAndSet(false, true)) {
                return;
            }
            bytes -= reservation.cost;
            --packets;
            outstanding.remove(reservation);
        }
    }

    /** One accepted datagram. Only the owning write path may release it. */
    public final class Reservation {
        private final long cost;
        private final AtomicBoolean released = new AtomicBoolean();

        private Reservation(long cost) {
            this.cost = cost;
        }

        /** Releases the reservation exactly once; later calls are ignored. */
        public void release() {
            UdpSendBudget.this.release(this);
        }

        public boolean isReleased() {
            return released.get();
        }

        private boolean releaseInternal() {
            boolean wasReleased = released.get();
            release();
            return !wasReleased;
        }
    }

    /** Observable counters; no payloads, tokens or endpoints. */
    public record Snapshot(long inflightBytes, long inflightPackets, long peakBytes,
            long peakPackets, long accepted, long rejected, int outstanding,
            boolean accepting) {}
}
