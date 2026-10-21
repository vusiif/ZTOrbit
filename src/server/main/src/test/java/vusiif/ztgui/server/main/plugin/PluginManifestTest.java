package vusiif.ztgui.server.main.plugin;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;
import vusiif.ztgui.server.api.ApiVersion;

class PluginManifestTest {
    private static PluginManifest parse(String json) throws IOException {
        return PluginManifest.parse(json.getBytes(StandardCharsets.UTF_8));
    }

    private static String manifest(int formatVersion, String host, String id, String name,
            String version, String apiVersion, String mainClass) {
        return "{\"formatVersion\":" + formatVersion
                + ",\"host\":\"" + host
                + "\",\"id\":\"" + id
                + "\",\"name\":\"" + name
                + "\",\"version\":\"" + version
                + "\",\"apiVersion\":\"" + apiVersion
                + "\",\"mainClass\":\"" + mainClass + "\"}";
    }

    private static String valid(int formatVersion, String host, String id, String name,
            String version, String apiVersion, String mainClass) {
        return manifest(formatVersion, host, id, name, version, apiVersion, mainClass);
    }

    @Test
    void parsesAValidManifest() throws IOException {
        PluginManifest manifest = parse(
                manifest(1, "server", "voice-chat", "语音聊天", "0.1.0", ApiVersion.value(), "example.Plugin"));
        assertEquals(1, manifest.formatVersion());
        assertEquals("server", manifest.host());
        assertEquals("voice-chat", manifest.id());
        assertEquals("语音聊天", manifest.name());
        assertEquals("0.1.0", manifest.version());
        assertEquals(ApiVersion.value(), manifest.apiVersion());
        assertEquals("example.Plugin", manifest.mainClass());
    }

    @Test
    void rejectsInvalidManifests() {
        assertThrows(IOException.class, () -> parse("not json"));
        assertThrows(IOException.class, () -> parse("[]"));
        assertThrows(IOException.class,
                () -> parse(valid(2, "server", "x", "x", "1", "0.4", "a.B")));
        assertThrows(IOException.class,
                () -> parse(valid(1, "client", "x", "x", "1", "0.4", "a.B")));
        assertThrows(IOException.class,
                () -> parse(valid(1, "server", "builtin", "x", "1", "0.4", "a.B")));
        assertThrows(IOException.class,
                () -> parse(valid(1, "server", "bad.id", "x", "1", "0.4", "a.B")));
        assertThrows(IOException.class,
                () -> parse(valid(1, "server", "x", " padded ", "1", "0.4", "a.B")));
        assertThrows(IOException.class,
                () -> parse(valid(1, "server", "x", "  ", "1", "0.4", "a.B")));
        assertThrows(IOException.class,
                () -> parse(valid(1, "server", "x", "x", "", "0.4", "a.B")));
        assertThrows(IOException.class,
                () -> parse(valid(1, "server", "x", "x", "1", "", "a.B")));
        assertThrows(IOException.class,
                () -> parse(valid(1, "server", "x", "x", "1", "0.4", "")));
        assertThrows(IOException.class, () -> parse("""
                {"formatVersion":1,"host":"server","id":"x","name":"x","version":"1",
                 "apiVersion":"0.4"}
                """));
    }
}
