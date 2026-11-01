package vusiif.ztohub.api;

/**
 * Result of writing one plugin log line. Both destinations are attempted independently.
 *
 * @param hostError error writing the host log, or empty
 * @param pluginError error writing the plugin log, or empty
 */
public record LogResult(String hostError, String pluginError) {
    public LogResult {
        hostError = hostError == null ? "" : hostError;
        pluginError = pluginError == null ? "" : pluginError;
    }

    public boolean ok() {
        return hostError.isEmpty() && pluginError.isEmpty();
    }

    public static LogResult success() {
        return new LogResult("", "");
    }
}
