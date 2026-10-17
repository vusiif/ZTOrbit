package vusiif.ztgui.server.api;

/** Receives a relay lookup result on the host event thread. */
@FunctionalInterface
public interface RelayCallback {
    void completed(RelayResult result);
}
