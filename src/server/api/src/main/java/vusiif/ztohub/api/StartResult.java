package vusiif.ztohub.api;

/**
 * Result of the optional background startup task.
 *
 * @param success whether the plugin finished initializing and may become ready
 * @param message human-readable detail written to the host log
 */
public record StartResult(boolean success, String message) {
    public StartResult {
        message = message == null ? "" : message;
    }

    public static StartResult ok() {
        return new StartResult(true, "");
    }

    public static StartResult failure(String message) {
        return new StartResult(false, message);
    }
}
