package vusiif.ztgui.server.api;

import java.util.Arrays;

/**
 * Result of a plugin data-file operation.
 *
 * @param data file contents ({@code null} on error, empty for an empty file)
 * @param error error description, or {@code null}/empty on success
 */
public record FileResult(byte[] data, String error) {
    public FileResult {
        error = error == null ? "" : error;
    }

    public boolean ok() {
        return error.isEmpty();
    }

    /** Defensive copy so callers cannot mutate a stored result. */
    @Override
    public byte[] data() {
        return data == null ? null : Arrays.copyOf(data, data.length);
    }

    public static FileResult success(byte[] data) {
        return new FileResult(data == null ? new byte[0] : Arrays.copyOf(data, data.length), "");
    }

    public static FileResult failure(String error) {
        return new FileResult(null, error);
    }
}
