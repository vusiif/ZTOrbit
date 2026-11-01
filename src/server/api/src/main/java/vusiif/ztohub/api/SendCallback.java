package vusiif.ztohub.api;

/** Receives a network send result on the host event thread. */
@FunctionalInterface
public interface SendCallback {
    void completed(SendResult result);
}
