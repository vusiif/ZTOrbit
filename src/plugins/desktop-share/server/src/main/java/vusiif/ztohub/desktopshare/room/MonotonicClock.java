package vusiif.ztohub.desktopshare.room;

/**
 * Monotonic nanosecond clock. Heartbeats, endpoint expiry, rate windows and the room
 * preparation deadline must never use wall-clock time, so changing the system clock
 * cannot extend or shorten a session.
 */
@FunctionalInterface
public interface MonotonicClock {
    long nanoTime();
}
