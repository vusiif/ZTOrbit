package vusiif.ztohub.api;

/** Receives subscribed network data on the host event thread. */
@FunctionalInterface
public interface NetworkCallback {
    void completed(NetworkMessage message);
}
