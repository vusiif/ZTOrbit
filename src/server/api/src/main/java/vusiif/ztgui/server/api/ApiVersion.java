package vusiif.ztgui.server.api;

import java.io.IOException;
import java.io.InputStream;
import java.util.Properties;

/**
 * API version compiled into this SDK jar, for example {@code 0.2-beta}. The value comes from
 * the filtered {@code api.properties} resource installed with the jar.
 */
public final class ApiVersion {
    private static final String VALUE = load();

    private ApiVersion() {}

    /** Returns the API version, or {@code 0.0} when the resource is missing. */
    public static String value() {
        return VALUE;
    }

    /** Exact match against {@link #value()}; {@code null} and longer versions never match. */
    public static boolean matches(String candidate) {
        return candidate != null && candidate.equals(VALUE);
    }

    private static String load() {
        Properties properties = new Properties();
        try (InputStream input =
                ApiVersion.class.getResourceAsStream("api.properties")) {
            if (input != null) {
                properties.load(input);
            }
        } catch (IOException ignored) {
            // Fall through to the development default; the build filters this resource.
        }
        String version = properties.getProperty("version", "0.0").trim();
        return version.isEmpty() ? "0.0" : version;
    }
}
