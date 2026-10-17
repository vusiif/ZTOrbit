package vusiif.ztgui.server.api;

import java.util.Objects;

/**
 * Plugin identity declared by {@link ServerPlugin#info()}.
 *
 * @param id route ID ({@code [A-Za-z0-9_-]{1,128}}, case-insensitively not {@code builtin})
 * @param name display name; also used as the data directory name and must be unique
 * @param version plugin version, for example {@code 0.1.0}
 */
public record PluginInfo(String id, String name, String version) {
    public PluginInfo {
        Objects.requireNonNull(id, "id");
        Objects.requireNonNull(name, "name");
        Objects.requireNonNull(version, "version");
    }
}
