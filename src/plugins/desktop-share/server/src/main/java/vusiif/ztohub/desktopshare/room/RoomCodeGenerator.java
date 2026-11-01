package vusiif.ztohub.desktopshare.room;

/** Produces one candidate eight-digit room code per call. */
@FunctionalInterface
public interface RoomCodeGenerator {
    String nextCode();
}
