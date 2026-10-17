package vusiif.ztgui.server.main.plugin;

import com.fasterxml.jackson.databind.JsonNode;
import java.io.IOException;
import vusiif.ztgui.server.main.JsonSupport;
import vusiif.ztgui.server.main.server.Wire;

/**
 * Parsed {@code META-INF/ztserver-plugin.json} descriptor.
 *
 * @param formatVersion must be {@code 1}
 * @param host must be {@code server}
 * @param id plugin route ID
 * @param name plugin display name (also the data directory name)
 * @param version plugin version
 * @param apiVersion host API version this plugin was built against, including any qualifier
 * @param mainClass {@code ServerPlugin} implementation class
 */
public record PluginManifest(
        int formatVersion,
        String host,
        String id,
        String name,
        String version,
        String apiVersion,
        String mainClass) {

    private static final int MAX_MANIFEST_BYTES = 64 * 1024;

    public static PluginManifest parse(byte[] bytes) throws IOException {
        if (bytes == null || bytes.length == 0 || bytes.length > MAX_MANIFEST_BYTES) {
            throw new IOException("invalid plugin manifest size");
        }
        JsonNode node = JsonSupport.parse(bytes);
        if (node == null || !node.isObject()) {
            throw new IOException("plugin manifest must be a JSON object");
        }
        JsonNode format = node.get("formatVersion");
        if (format == null || !format.isIntegralNumber() || format.asInt() != 1) {
            throw new IOException("unsupported plugin manifest formatVersion");
        }
        int formatVersion = format.asInt();
        String host = text(node, "host");
        if (!"server".equals(host)) {
            throw new IOException("plugin host must be server");
        }
        String id = text(node, "id");
        if (!Wire.validId(id) || id.equalsIgnoreCase(Wire.BUILTIN)) {
            throw new IOException("invalid plugin ID");
        }
        String name = text(node, "name");
        if (name.isEmpty() || !name.equals(name.strip())) {
            throw new IOException("invalid plugin name");
        }
        String version = text(node, "version");
        if (version.isEmpty() || !version.equals(version.strip())) {
            throw new IOException("invalid plugin version");
        }
        String apiVersion = text(node, "apiVersion");
        if (apiVersion.isEmpty() || !apiVersion.equals(apiVersion.strip())) {
            throw new IOException("invalid plugin API version");
        }
        String mainClass = text(node, "mainClass");
        if (mainClass.isEmpty() || !mainClass.equals(mainClass.strip())) {
            throw new IOException("invalid plugin mainClass");
        }
        return new PluginManifest(formatVersion, host, id, name, version, apiVersion, mainClass);
    }

    private static String text(JsonNode node, String field) throws IOException {
        JsonNode value = node.get(field);
        if (value == null || !value.isTextual()) {
            throw new IOException("plugin manifest field " + field + " must be a string");
        }
        return value.asText();
    }
}
