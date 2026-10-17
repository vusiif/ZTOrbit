package vusiif.ztgui.server.api;

/**
 * Handle for cancelling asynchronous host work. Cancellation is idempotent.
 */
@FunctionalInterface
public interface Cancellable {
    void cancel();
}
