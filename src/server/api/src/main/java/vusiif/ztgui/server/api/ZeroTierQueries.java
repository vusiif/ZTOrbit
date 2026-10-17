package vusiif.ztgui.server.api;

import com.fasterxml.jackson.databind.JsonNode;

/**
 * ZeroTier read-only query interface.
 *
 * <p>Supported operations: {@code info}, {@code peers}, {@code networks}, {@code moons},
 * {@code controller.networks}, {@code controller.members}. Queries run asynchronously and the
 * callback is invoked on the host event thread. Cancelling the returned handle, stopping the
 * host, or stopping the plugin suppresses the callback. Event thread only.
 */
public interface ZeroTierQueries {
    Cancellable query(String operation, JsonNode parameters, QueryCallback completed);
}
