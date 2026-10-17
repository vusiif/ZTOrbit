package vusiif.ztgui.server.api;

import java.util.List;

/**
 * Result of a network relay lookup.
 *
 * @param relays relays for the network, never {@code null}
 * @param error error description, empty on success
 */
public record RelayResult(List<RelayInfo> relays, String error) {
    public RelayResult {
        relays = relays == null ? List.of() : List.copyOf(relays);
        error = error == null ? "" : error;
    }

    public boolean ok() {
        return error.isEmpty();
    }

    public static RelayResult success(List<RelayInfo> relays) {
        return new RelayResult(relays, "");
    }

    public static RelayResult failure(String error) {
        return new RelayResult(List.of(), error);
    }
}
