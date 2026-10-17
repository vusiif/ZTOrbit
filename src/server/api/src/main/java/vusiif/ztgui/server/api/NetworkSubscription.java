package vusiif.ztgui.server.api;

/**
 * Handle for a network data subscription. Cancelling removes the callback; cancellation is
 * idempotent and is also performed by the host in {@link ServerPlugin#onStop()}.
 */
public interface NetworkSubscription extends Cancellable {
}
