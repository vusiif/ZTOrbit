package vusiif.ztgui.server.main.plugin;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import vusiif.ztgui.server.api.FileResult;

class PluginFilesImplTest {
    @TempDir
    Path root;

    private PluginFilesImpl files(String name) {
        return new PluginFilesImpl(root, name);
    }

    @Test
    void writesAtomicallyAndReadsBack() {
        PluginFilesImpl files = files("语音聊天");
        FileResult written = files.writeFile("settings.json",
                "{\"maxRooms\":2}".getBytes(StandardCharsets.UTF_8));
        assertTrue(written.ok(), written.error());
        assertTrue(Files.isRegularFile(root.resolve("plugins-data/语音聊天/settings.json")));
        FileResult read = files.readFile("settings.json");
        assertTrue(read.ok());
        assertArrayEquals("{\"maxRooms\":2}".getBytes(StandardCharsets.UTF_8), read.data());
        assertTrue(files.writeFile("nested/dir/data.bin", new byte[] {1, 2}).ok());
        assertArrayEquals(new byte[] {1, 2}, files.readFile("nested/dir/data.bin").data());
    }

    @Test
    void rejectsInvalidNamesAndPaths() {
        assertFalse(files("").writeFile("a", new byte[0]).ok());
        assertFalse(files("..").writeFile("a", new byte[0]).ok());
        assertFalse(files("with/slash").writeFile("a", new byte[0]).ok());
        PluginFilesImpl files = files("test");
        for (String path : new String[] {"", "/absolute", "C:/x", "a/../b", "a/./b",
                "a//b", "a/", "trailing.", "trailing ", "nul\0byte", "a/b/../../c"}) {
            assertFalse(files.writeFile(path, new byte[0]).ok(), "path should be rejected: " + path);
        }
        assertFalse(files.readFile("missing").ok());
    }

    @Test
    void rejectsSymlinkedDirectoriesWhenSupported() throws Exception {
        Path shared = root.resolve("plugins-data");
        Files.createDirectories(shared);
        Path target = root.resolve("outside");
        Files.createDirectories(target);
        Path link = shared.resolve("linked");
        try {
            Files.createSymbolicLink(link, target);
        } catch (UnsupportedOperationException | java.nio.file.FileSystemException unsupported) {
            return; // Windows without symlink privilege
        } catch (java.io.IOException unsupported) {
            return;
        }
        PluginFilesImpl linked = files("linked");
        assertFalse(linked.writeFile("file", new byte[0]).ok());
        assertFalse(linked.readFile("file").ok());
    }

    @Test
    void rejectsWhenPluginsDataIsAFile() throws Exception {
        Files.write(root.resolve("plugins-data"), new byte[] {1});
        assertFalse(files("test").writeFile("a", new byte[0]).ok());
        assertTrue(files("test").readFile("a").error().contains("plugins-data"));
    }

    @Test
    void pluginNameValidationMatchesHostRules() {
        assertTrue(PluginFilesImpl.validPluginName("语音聊天"));
        assertTrue(PluginFilesImpl.validPluginName("test-echo"));
        assertFalse(PluginFilesImpl.validPluginName(""));
        assertFalse(PluginFilesImpl.validPluginName("."));
        assertFalse(PluginFilesImpl.validPluginName(".."));
        assertFalse(PluginFilesImpl.validPluginName("name."));
        assertFalse(PluginFilesImpl.validPluginName(" name"));
        assertFalse(PluginFilesImpl.validPluginName("a/b"));
        assertFalse(PluginFilesImpl.validPluginName("CON"));
        assertFalse(PluginFilesImpl.validPluginName("nul.txt"));
        assertFalse(PluginFilesImpl.validPluginName("lpt9"));
        assertTrue(PluginFilesImpl.validPluginName("console"));
    }
}
