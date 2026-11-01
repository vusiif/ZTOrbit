package vusiif.ztohub.main.plugin;

import java.io.IOException;
import vusiif.ztohub.api.LogLevel;
import vusiif.ztohub.api.LogResult;
import vusiif.ztohub.api.PluginLogger;
import vusiif.ztohub.main.DailyLogFile;
import vusiif.ztohub.main.HostLog;

/**
 * Writes plugin log lines to the host log ({@code LEVEL [plugin-id] message}) and to the plugin's
 * own {@code plugins-data/<name>/logs/YYMMDD-N.log}. Thread-safe.
 */
public final class PluginLogWriter implements PluginLogger {
    private final HostLog hostLog;
    private final String pluginId;
    private final PluginFilesImpl files;
    private final Object lock = new Object();
    private DailyLogFile pluginLog;
    private String openError = "";

    PluginLogWriter(HostLog hostLog, String pluginId, PluginFilesImpl files) {
        this.hostLog = hostLog;
        this.pluginId = pluginId;
        this.files = files;
    }

    @Override
    public LogResult write(LogLevel level, String message) {
        String text = levelName(level) + " [" + pluginId + "] " + strip(message);
        String hostError = hostLog.writeChecked(text);
        String pluginError = "";
        synchronized (lock) {
            if (pluginLog == null && openError.isEmpty()) {
                try {
                    pluginLog = DailyLogFile.open(files.resolveDirectory("logs"));
                } catch (IOException failure) {
                    openError = failure.getMessage() == null ? failure.toString() : failure.getMessage();
                }
            }
            if (pluginLog != null) {
                pluginError = pluginLog.writeChecked(text);
            } else {
                pluginError = openError;
            }
        }
        return new LogResult(hostError, pluginError);
    }

    private static String levelName(LogLevel level) {
        return switch (level == null ? LogLevel.INFO : level) {
            case DEBUG -> "DEBUG";
            case INFO -> "INFO";
            case WARNING -> "WARN";
            case ERROR -> "ERROR";
        };
    }

    private static String strip(String message) {
        return message == null ? "" : message.strip();
    }
}
