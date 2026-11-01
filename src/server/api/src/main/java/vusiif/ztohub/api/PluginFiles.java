package vusiif.ztohub.api;

/**
 * Binary file access scoped to {@code plugins-data/<PluginInfo.name>/}. Paths are relative to the
 * plugin data directory. Thread-safe.
 */
public interface PluginFiles {
    FileResult readFile(String relativePath);

    /**
     * Atomically replaces a file, creating missing parent directories. Relative path rules reject
     * absolute paths, drive/colon prefixes, NUL, empty segments, {@code .}/{@code ..}, trailing
     * dots/spaces, symbolic links and non-regular files or directories.
     */
    FileResult writeFile(String relativePath, byte[] data);
}
