package vusiif.ztohub.api;

import java.util.List;

/**
 * Result of sending plugin data to network members.
 *
 * <p>{@code ok()} means the data was queued to the local transport socket, not that remote
 * members received it.
 *
 * @param recipients number of members the transport queued data for
 * @param error transport error, empty when the request succeeded
 * @param failedMembers members that could not be reached
 */
public record SendResult(int recipients, String error, List<String> failedMembers) {
    public SendResult {
        error = error == null ? "" : error;
        failedMembers = failedMembers == null ? List.of() : List.copyOf(failedMembers);
    }

    public boolean ok() {
        return error.isEmpty();
    }

    public static SendResult success(int recipients) {
        return new SendResult(recipients, "", List.of());
    }

    public static SendResult failure(String error, List<String> failedMembers) {
        return new SendResult(0, error, failedMembers);
    }

    public static SendResult nothing() {
        return new SendResult(0, "", List.of());
    }
}
