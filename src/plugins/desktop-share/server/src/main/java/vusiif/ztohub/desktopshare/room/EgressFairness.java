package vusiif.ztohub.desktopshare.room;

/**
 * Cross-room fairness gate for the shared egress token bucket.
 *
 * <p>The server-wide egress limit is one bucket for every room. Because publisher
 * fragments are processed in arrival order on the host event thread, a room whose
 * fragment is handled first always drains the bucket and a room handled second is
 * always refused, no matter how many times it tries. Rotating the viewers inside a
 * room does not change that: two rooms with one ready viewer each still end up with
 * one room forwarding everything and the other nothing while the budget is tight.
 *
 * <p>The gate grants one room the egress <em>turn</em>. A room that is refused a
 * copy because the bucket is empty keeps the turn together with its demand, so the
 * next refill serves it first; other rooms yield a fragment while that demand is
 * fresh and the bucket cannot satisfy both. A successful fan-out releases the turn
 * so the next room can progress. This bounds per-room progress by at most one
 * fragment interval instead of unbounded starvation, while the global bucket, the
 * per-viewer reservation and the host UDP budget remain the only rate limits.
 *
 * <p>State is constant size: at most one turn holder and at most one waiting room
 * with its byte cost. It is confined to the host event thread, expires after
 * {@link #DEMAND_TTL_NANOS}/{@link #MAX_TURN_HOLD_NANOS}, and is cleared when a
 * room closes, loses its last viewer, changes profile, or the controller stops. No
 * fragment, packet or video history is stored here.
 */
final class EgressFairness {
    /** Longest a room may keep the turn without any admitted copy. */
    static final long MAX_TURN_HOLD_NANOS = 100_000_000L;
    /** How long a refused room keeps priority for the next refill. */
    static final long DEMAND_TTL_NANOS = 100_000_000L;

    private long turnRoom;
    private long turnSinceNanos;
    private long waitingRoom;
    private long waitingCostBytes;
    private long waitingSinceNanos;

    /**
     * Decides whether {@code roomId} may try to consume the bucket now, before any
     * copy is retargeted or sent.
     *
     * @param unitCostBytes one retargeted datagram
     * @param requestCostBytes every copy this fragment would send in the room
     */
    boolean admit(long roomId, long unitCostBytes, long requestCostBytes,
            long availableBytes, long nowNanos) {
        if (roomId <= 0 || unitCostBytes <= 0) {
            return true;
        }
        expire(nowNanos);
        if (turnRoom == roomId) {
            return true;
        }
        if (turnRoom == 0) {
            takeTurn(roomId, nowNanos);
            return true;
        }
        if (nowNanos - turnSinceNanos >= MAX_TURN_HOLD_NANOS) {
            // The holder could not obtain the budget inside the hold window (for
            // example its fragment is larger than the remaining allowance); stop
            // protecting it so the room with data can make progress.
            takeTurn(roomId, nowNanos);
            return true;
        }
        if (waitingRoom == turnRoom
                && availableBytes < requestCostBytes + waitingCostBytes) {
            // The holder was refused a copy and will be served by the next refill.
            // Hold the requesting room back only when the bucket cannot satisfy
            // both datagrams; when it can, both rooms progress.
            return false;
        }
        // The holder has no outstanding demand, so its turn is spent. The room
        // that has data takes it; this is the round-robin step between rooms.
        takeTurn(roomId, nowNanos);
        return true;
    }

    /**
     * Records the outcome of a fan-out attempt the gate admitted.
     *
     * @param sentCopies copies actually accepted by the bucket and the host
     * @param tokenDenied copies refused only because the bucket was empty
     */
    void conclude(long roomId, int sentCopies, int tokenDenied, long requestCostBytes,
            long nowNanos) {
        if (turnRoom != roomId) {
            // Admitted while another room held a fresh demand that the bucket could
            // still satisfy together; that room keeps the turn.
            return;
        }
        if (sentCopies > 0) {
            releaseTurn();
            releaseWaiting(roomId);
            return;
        }
        if (tokenDenied > 0) {
            // Keep the turn for the next refill; this is what prevents the fixed
            // arrival order from always favouring the other room.
            waitingRoom = roomId;
            waitingCostBytes = requestCostBytes;
            waitingSinceNanos = nowNanos;
            return;
        }
        releaseTurn();
        releaseWaiting(roomId);
    }

    /** Drops any fairness state held by a room that closed, left or reconfigured. */
    void release(long roomId) {
        if (turnRoom == roomId) {
            releaseTurn();
        }
        if (waitingRoom == roomId) {
            clearWaiting();
        }
    }

    /** Drops every scheduling entry; used by {@code RoomController.stop()}. */
    void clear() {
        releaseTurn();
        clearWaiting();
    }

    /** Expires a demand that no fragment refreshed. */
    void maintenance(long nowNanos) {
        expire(nowNanos);
    }

    long turnHolderRoom() {
        return turnRoom;
    }

    long waitingRoom() {
        return waitingRoom;
    }

    long waitingCostBytes() {
        return waitingCostBytes;
    }

    private void expire(long nowNanos) {
        if (waitingRoom == 0 || nowNanos - waitingSinceNanos < DEMAND_TTL_NANOS) {
            return;
        }
        // The refused room never sent another fragment, so its priority is stale.
        clearWaiting();
        if (turnRoom != 0 && nowNanos - turnSinceNanos >= MAX_TURN_HOLD_NANOS) {
            releaseTurn();
        }
    }

    private void takeTurn(long roomId, long nowNanos) {
        turnRoom = roomId;
        turnSinceNanos = nowNanos;
        if (waitingRoom == roomId) {
            clearWaiting();
        }
    }

    private void releaseTurn() {
        turnRoom = 0;
        turnSinceNanos = 0;
    }

    private void releaseWaiting(long roomId) {
        if (roomId != 0 && waitingRoom != roomId) {
            return;
        }
        clearWaiting();
    }

    private void clearWaiting() {
        waitingRoom = 0;
        waitingCostBytes = 0;
        waitingSinceNanos = 0;
    }
}
