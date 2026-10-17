package vusiif.ztgui.server.main.plugin;

import java.io.IOException;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.regex.Pattern;
import vusiif.ztgui.server.api.FileResult;
import vusiif.ztgui.server.api.PluginFiles;

/**
 * Plugin file service rooted at {@code plugins-data/<PluginInfo.name>/}. Paths are validated
 * segment by segment; symbolic links and non-regular files are rejected and writes are atomic.
 */
public final class PluginFilesImpl implements PluginFiles {
    private static final Pattern DEVICE_NAMES = Pattern.compile(
            "^(?:con|prn|aux|nul|com[1-9\u00b9\u00b2\u00b3]|lpt[1-9\u00b9\u00b2\u00b3])(?:\\.|$)",
            Pattern.CASE_INSENSITIVE);
    private static final String INVALID_NAME = "插件名称不能用作数据目录名";
    private static final String INVALID_PATH = "文件路径必须相对于当前插件的数据目录";
    private static final String INVALID_SEGMENT = "文件路径包含无效分段";
    private static final String INVALID_ROOT = "plugins-data 必须是普通目录";
    private static final String INVALID_DATA_ROOT = "插件数据目录必须是普通目录";
    private static final String INVALID_LINK = "文件路径包含链接或非普通文件/目录";

    private final Path sharedRoot;
    private final Path dataRoot;
    private final String pluginName;
    private final Object lock = new Object();

    public PluginFilesImpl(Path workingDirectory, String pluginName) {
        this.sharedRoot = workingDirectory.resolve("plugins-data");
        this.pluginName = pluginName;
        this.dataRoot = sharedRoot.resolve(pluginName);
    }

    @Override
    public FileResult readFile(String relativePath) {
        synchronized (lock) {
            try {
                Path path = resolve(relativePath, false);
                return FileResult.success(Files.readAllBytes(path));
            } catch (IOException failure) {
                return FileResult.failure(message(failure));
            }
        }
    }

    @Override
    public FileResult writeFile(String relativePath, byte[] data) {
        synchronized (lock) {
            Path target = null;
            try {
                target = resolve(relativePath, true);
                Path parent = target.getParent();
                if (parent == null) {
                    return FileResult.failure(INVALID_PATH);
                }
                Files.createDirectories(parent);
                if (hasLink(parent)) {
                    return FileResult.failure(INVALID_LINK);
                }
                Path temporary = parent.resolve("." + target.getFileName() + ".tmp-" + UUID.randomUUID());
                try {
                    Files.write(temporary, data == null ? new byte[0] : data,
                            StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE);
                    try {
                        Files.move(temporary, target,
                                StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
                    } catch (AtomicMoveNotSupportedException fallback) {
                        Files.move(temporary, target, StandardCopyOption.REPLACE_EXISTING);
                    }
                } finally {
                    Files.deleteIfExists(temporary);
                }
                return FileResult.success(new byte[0]);
            } catch (IOException failure) {
                return FileResult.failure(message(failure));
            }
        }
    }

    /**
     * Resolves and validates a directory inside the plugin data directory, creating it when
     * missing. Used by the plugin log writer.
     */
    Path resolveDirectory(String relativePath) throws IOException {
        synchronized (lock) {
            Path path = resolveSegments(relativePath, true);
            Files.createDirectories(path);
            if (Files.isSymbolicLink(path) || !Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException(INVALID_LINK);
            }
            return path;
        }
    }

    private Path resolve(String relativePath, boolean writing) throws IOException {
        prepareRoots(writing);
        Path path = resolveSegments(relativePath, writing);
        if (Files.exists(path, LinkOption.NOFOLLOW_LINKS)) {
            if (Files.isSymbolicLink(path) || !Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException(INVALID_LINK);
            }
        } else if (!writing) {
            throw new IOException("not found");
        }
        if (writing) {
            Files.createDirectories(path.getParent());
        }
        return path;
    }

    private Path resolveSegments(String relativePath, boolean writing) throws IOException {
        if (relativePath == null || relativePath.isEmpty()) {
            throw new IOException(INVALID_PATH);
        }
        String normalized = relativePath.replace('\\', '/');
        if (normalized.startsWith("/") || normalized.indexOf(':') >= 0
                || normalized.indexOf('\0') >= 0) {
            throw new IOException(INVALID_PATH);
        }
        String[] parts = normalized.split("/", -1);
        List<String> segments = new ArrayList<>();
        for (String part : parts) {
            if (part.isEmpty() || ".".equals(part) || "..".equals(part)
                    || part.endsWith(".") || part.endsWith(" ")) {
                throw new IOException(INVALID_SEGMENT);
            }
            segments.add(part);
        }
        Path path = dataRoot;
        for (int index = 0; index < segments.size(); index++) {
            path = path.resolve(segments.get(index));
            boolean last = index == segments.size() - 1;
            if (Files.isSymbolicLink(path)) {
                throw new IOException(INVALID_LINK);
            }
            if (!last) {
                if (Files.exists(path, LinkOption.NOFOLLOW_LINKS)
                        && !Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)) {
                    throw new IOException(INVALID_LINK);
                }
                if (writing && !Files.exists(path, LinkOption.NOFOLLOW_LINKS)) {
                    Files.createDirectories(path);
                }
            }
        }
        return path;
    }

    private void prepareRoots(boolean writing) throws IOException {
        if (!validPluginName(pluginName)) {
            throw new IOException(INVALID_NAME);
        }
        if (Files.isSymbolicLink(sharedRoot)) {
            throw new IOException(INVALID_ROOT);
        }
        if (Files.exists(sharedRoot, LinkOption.NOFOLLOW_LINKS)
                && !Files.isDirectory(sharedRoot, LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException(INVALID_ROOT);
        }
        if (Files.isSymbolicLink(dataRoot)) {
            throw new IOException(INVALID_DATA_ROOT);
        }
        if (Files.exists(dataRoot, LinkOption.NOFOLLOW_LINKS)
                && !Files.isDirectory(dataRoot, LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException(INVALID_DATA_ROOT);
        }
        if (writing) {
            try {
                Files.createDirectories(dataRoot);
            } catch (IOException failure) {
                throw new IOException("无法创建插件数据目录", failure);
            }
            if (Files.isSymbolicLink(dataRoot) || !Files.isDirectory(dataRoot, LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException(INVALID_DATA_ROOT);
            }
        }
    }

    private static boolean hasLink(Path path) throws IOException {
        Path current = path.getRoot();
        if (current == null) {
            return false;
        }
        for (Path part : path) {
            current = current.resolve(part);
            if (Files.isSymbolicLink(current)) {
                return true;
            }
        }
        return false;
    }

    static boolean validPluginName(String name) {
        if (name == null || name.isEmpty() || !name.equals(name.strip())
                || ".".equals(name) || "..".equals(name) || name.endsWith(".")) {
            return false;
        }
        for (int index = 0; index < name.length(); index++) {
            char character = name.charAt(index);
            if (character < 32 || character == 127
                    || (character >= 0x80 && character <= 0x9f)
                    || "/\\:*?\"<>|".indexOf(character) >= 0) {
                return false;
            }
        }
        return !DEVICE_NAMES.matcher(name).find();
    }

    private static String message(IOException failure) {
        return failure.getMessage() == null ? failure.toString() : failure.getMessage();
    }
}
