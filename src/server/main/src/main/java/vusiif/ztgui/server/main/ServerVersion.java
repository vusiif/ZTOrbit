package vusiif.ztgui.server.main;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;

/** Application and API versions filtered into {@code version.properties} at build time. */
public record ServerVersion(String version, String apiVersion) {
    private static final ServerVersion CURRENT = load();

    public static ServerVersion current() {
        return CURRENT;
    }

    private static ServerVersion load() {
        Properties properties = new Properties();
        try (InputStream input =
                ServerVersion.class.getResourceAsStream("version.properties")) {
            if (input != null) {
                properties.load(input);
            }
        } catch (IOException ignored) {
            // The development default below keeps direct, unfiltered runs usable.
        }
        String version = properties.getProperty("version", "0.0.0-dev").trim();
        String apiVersion = properties.getProperty("apiVersion", "0.0").trim();
        return new ServerVersion(
                version.isEmpty() ? "0.0.0-dev" : version,
                apiVersion.isEmpty() ? "0.0" : apiVersion);
    }
}
