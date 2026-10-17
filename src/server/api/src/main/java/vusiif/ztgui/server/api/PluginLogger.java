package vusiif.ztgui.server.api;

/** Plugin log writer. Thread-safe; one call writes one line to both log destinations. */
public interface PluginLogger {
    LogResult write(LogLevel level, String message);
}
