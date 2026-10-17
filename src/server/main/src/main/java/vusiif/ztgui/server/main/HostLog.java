package vusiif.ztgui.server.main;

import java.io.Closeable;
import java.io.IOException;
import java.io.PrintStream;
import java.nio.file.Path;

/**
 * Host log in {@code <working directory>/logs/YYMMDD-N.log}. The log file is fixed for the
 * lifetime of the process; write failures fall back to stderr.
 */
public final class HostLog implements Closeable {
    private final DailyLogFile file;
    private final PrintStream error = System.err;

    public HostLog(Path applicationDirectory) {
        DailyLogFile opened;
        try {
            opened = DailyLogFile.open(applicationDirectory.resolve("logs"));
        } catch (IOException failure) {
            error.println("Cannot open application log: " + failure.getMessage());
            opened = null;
        }
        this.file = opened;
    }

    public String path() {
        return file == null ? "" : file.path().toString();
    }

    public void debug(String message) {
        write("DEBUG", message);
    }

    public void info(String message) {
        write("INFO", message);
    }

    public void warn(String message) {
        write("WARN", message);
    }

    public void error(String message) {
        write("ERROR", message);
    }

    public void write(String level, String message) {
        writeChecked(level + " " + message);
    }

    /** Returns an error description, or an empty string after a successful write. */
    public String writeChecked(String text) {
        if (file == null) {
            error.println(text);
            return "Cannot open application log";
        }
        String result = file.writeChecked(text);
        if (!result.isEmpty()) {
            error.println("Cannot write application log: " + result);
        }
        return result;
    }

    /** Writes the startup summary required by the deployment documentation. */
    public void writeStartup(String version) {
        info("服务启动 zt-server v" + version);
        info("Java 运行时 " + System.getProperty("java.version")
                + " (" + System.getProperty("java.vendor") + ")");
    }

    @Override
    public void close() {
        if (file != null) {
            file.close();
        }
    }
}
