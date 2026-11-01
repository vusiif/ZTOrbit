package vusiif.ztohub.api;

import java.util.concurrent.CancellationException;

/**
 * Read-only cancellation signal handed to {@link ServerPlugin#onStart}.
 */
public interface CancellationToken {
    boolean isCancellationRequested();

    default void throwIfCancellationRequested() {
        if (isCancellationRequested()) {
            throw new CancellationException("Plugin startup cancelled");
        }
    }
}
