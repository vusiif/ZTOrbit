package vusiif.ztohub.api;

/**
 * Handle for cancelling asynchronous host work. Cancellation is idempotent.
 */
@FunctionalInterface
public interface Cancellable {
    void cancel();
}
