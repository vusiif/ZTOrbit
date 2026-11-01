package vusiif.ztohub.main.plugin;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import vusiif.ztohub.api.ApiVersion;
import vusiif.ztohub.api.PluginInfo;
import vusiif.ztohub.api.ServerPlugin;
import vusiif.ztohub.api.StartResult;
import vusiif.ztohub.api.StartTask;
import vusiif.ztohub.main.ServerHost;

/**
 * Scans {@code plugins/*.jar} once at startup, validates manifests, loads plugins with an isolated
 * {@link URLClassLoader} and manages their readiness and shutdown.
 */
public final class PluginManager {
    private static final long MAX_JAR_BYTES = 256L * 1024 * 1024;
    private static final int MAX_MANIFEST_BYTES = 64 * 1024;
    private static final String MANIFEST_ENTRY = "META-INF/ztohub-plugin.json";

    private final ServerHost host;
    private final Path pluginsDirectory;
    private final List<PluginContainer> plugins = new ArrayList<>();
    private final ExecutorService startupExecutor =
            Executors.newVirtualThreadPerTaskExecutor();
    private boolean stopping;

    public PluginManager(ServerHost host) {
        this.host = host;
        this.pluginsDirectory = host.workingDirectory().resolve("plugins");
    }

    /** Scans and starts every plugin. Must run on the host event thread. */
    public void scanAndStart() {
        try {
            Files.createDirectories(pluginsDirectory);
        } catch (IOException failure) {
            host.log().warn("Cannot create plugins directory: " + failure.getMessage());
            return;
        }
        List<Path> entries = new ArrayList<>();
        try (var stream = Files.list(pluginsDirectory)) {
            stream.forEach(entries::add);
        } catch (IOException failure) {
            host.log().warn("Cannot scan plugins directory: " + failure.getMessage());
            return;
        }
        entries.sort(Comparator.comparing(path -> path.getFileName().toString()));
        for (Path entry : entries) {
            String name = entry.getFileName().toString();
            if (Files.isSymbolicLink(entry)) {
                if (name.endsWith(".jar") || name.endsWith(".z2plugin")) {
                    host.log().warn("Rejected package " + name + ": symbolic links are not supported");
                }
                continue;
            }
            if (Files.isDirectory(entry, LinkOption.NOFOLLOW_LINKS)) {
                continue;
            }
            if (name.endsWith(".z2plugin")) {
                host.log().warn("Rejected package " + name + ": unsupported package, use .jar");
                continue;
            }
            if (!name.endsWith(".jar")) {
                continue;
            }
            if (!Files.isRegularFile(entry, LinkOption.NOFOLLOW_LINKS)) {
                host.log().warn("Rejected package " + name + ": not a regular file");
                continue;
            }
            load(entry);
        }
    }

    private void load(Path jar) {
        String fileName = jar.getFileName().toString();
        URLClassLoader loader = null;
        ServerPlugin plugin = null;
        PluginServices services = null;
        try {
            long size = Files.size(jar);
            if (size > MAX_JAR_BYTES) {
                throw new IOException("plugin jar exceeds 256 MiB");
            }
            PluginManifest manifest = readManifest(jar);
            if (!ApiVersion.matches(manifest.apiVersion())) {
                throw new IOException("plugin API version " + manifest.apiVersion()
                        + " does not match host API " + ApiVersion.value());
            }
            if (findById(manifest.id()) != null) {
                throw new IOException("Duplicate plugin ID");
            }
            if (findByName(manifest.name()) != null) {
                throw new IOException("Duplicate plugin data directory/name");
            }
            loader = new URLClassLoader(new URL[] {jar.toUri().toURL()},
                    PluginManager.class.getClassLoader());
            Class<?> type = Class.forName(manifest.mainClass(), true, loader);
            Object instance = type.getDeclaredConstructor().newInstance();
            if (!(instance instanceof ServerPlugin serverPlugin)) {
                throw new IOException("plugin mainClass does not implement ServerPlugin");
            }
            plugin = serverPlugin;
            PluginInfo info = plugin.info();
            if (info == null
                    || !manifest.id().equals(info.id())
                    || !manifest.name().equals(info.name())
                    || !manifest.version().equals(info.version())) {
                throw new IOException("plugin info does not match manifest");
            }
            services = new PluginServices(host, manifest.id(), manifest.name());
            PluginContainer container = new PluginContainer(manifest, plugin, loader, services);
            plugins.add(container);
            start(container);
            host.log().info("Loaded plugin " + manifest.id());
        } catch (Throwable failure) {
            if (services != null) {
                services.stopRequests();
            }
            if (plugin != null) {
                try {
                    plugin.onStop();
                } catch (RuntimeException stopFailure) {
                    host.log().warn("Plugin stop failed for " + fileName + ": " + stopFailure.getMessage());
                }
            }
            if (loader != null) {
                try {
                    loader.close();
                } catch (IOException ignored) {
                    // The loader is being discarded anyway.
                }
            }
            String message = failure.getMessage() == null ? failure.toString() : failure.getMessage();
            host.log().warn("Plugin rejected " + fileName + ": " + message);
        }
    }

    private void start(PluginContainer container) {
        StartTask task;
        try {
            task = container.plugin().onStart(container.services(), container.cancellation());
        } catch (RuntimeException failure) {
            container.setReady(false);
            host.log().warn("Plugin initialization failed " + container.manifest().id()
                    + ": " + (failure.getMessage() == null ? failure.toString() : failure.getMessage()));
            return;
        }
        if (task == null) {
            container.setReady(true);
            return;
        }
        CompletableFuture<StartResult> startup = CompletableFuture.supplyAsync(() -> {
            try {
                StartResult result = task.start();
                return result == null ? StartResult.failure("Plugin startup returned no result") : result;
            } catch (Exception failure) {
                String message = failure.getMessage() == null ? failure.toString() : failure.getMessage();
                return StartResult.failure("Plugin startup exception: " + message);
            }
        }, startupExecutor);
        container.setStartup(startup);
        startup.whenComplete((result, failure) -> {
            StartResult value = result != null
                    ? result
                    : StartResult.failure(failure == null ? "Plugin startup failed" : failure.toString());
            host.eventLoop().execute(() -> {
                if (stopping || !plugins.contains(container)) {
                    return;
                }
                container.setReady(value.success());
                if (!value.success()) {
                    host.log().warn("Plugin initialization failed " + container.manifest().id()
                            + " " + value.message());
                } else {
                    host.log().info("Plugin ready " + container.manifest().id());
                }
            });
        });
    }

    private PluginManifest readManifest(Path jar) throws IOException {
        try (ZipFile zip = new ZipFile(jar.toFile())) {
            ZipEntry entry = zip.getEntry(MANIFEST_ENTRY);
            if (entry == null) {
                throw new IOException("missing " + MANIFEST_ENTRY);
            }
            if (entry.getSize() > MAX_MANIFEST_BYTES) {
                throw new IOException("plugin manifest exceeds 64 KiB");
            }
            try (InputStream input = zip.getInputStream(entry)) {
                ByteArrayOutputStream output = new ByteArrayOutputStream();
                byte[] buffer = new byte[8192];
                int read;
                while ((read = input.read(buffer)) >= 0) {
                    if (output.size() + read > MAX_MANIFEST_BYTES) {
                        throw new IOException("plugin manifest exceeds 64 KiB");
                    }
                    output.write(buffer, 0, read);
                }
                return PluginManifest.parse(output.toByteArray());
            }
        } catch (IOException failure) {
            throw failure;
        } catch (RuntimeException failure) {
            throw new IOException(failure.getMessage() == null ? failure.toString() : failure.getMessage(), failure);
        }
    }

    public PluginContainer findById(String id) {
        if (id == null) {
            return null;
        }
        for (PluginContainer container : plugins) {
            if (container.manifest().id().equalsIgnoreCase(id)) {
                return container;
            }
        }
        return null;
    }

    public PluginContainer findByName(String name) {
        if (name == null) {
            return null;
        }
        for (PluginContainer container : plugins) {
            if (container.manifest().name().equalsIgnoreCase(name)) {
                return container;
            }
        }
        return null;
    }

    /** Returns a ready plugin for a route, or {@code null} when unknown or not ready. */
    public ServerPlugin readyPlugin(String route) {
        PluginContainer container = findById(route);
        return container != null && container.isReady() ? container.plugin() : null;
    }

    public boolean isReady(String route) {
        PluginContainer container = findById(route);
        return container != null && container.isReady();
    }

    public List<PluginContainer> containers() {
        return List.copyOf(plugins);
    }

    /** Stops every plugin and closes its class loader. Must run on the host event thread. */
    public void stopAll() {
        beginStop();
        finishStop();
    }

    /** Marks all plugins not ready and cancels their initialization and pending work. */
    public void beginStop() {
        stopping = true;
        for (PluginContainer container : plugins) {
            container.setReady(false);
            container.cancellationSource().cancel();
            container.services().stopRequests();
        }
    }

    /** Waits for startup tasks, calls onStop and closes every class loader. */
    public void finishStop() {
        for (PluginContainer container : plugins) {
            CompletableFuture<StartResult> startup = container.startup();
            if (startup != null) {
                try {
                    startup.get(30, TimeUnit.SECONDS);
                } catch (TimeoutException timeout) {
                    host.log().warn("Plugin initialization did not finish " + container.manifest().id());
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                } catch (Exception ignored) {
                    // The plugin already failed to start; onStop still runs below.
                }
            }
        }
        for (PluginContainer container : plugins) {
            try {
                container.plugin().onStop();
            } catch (RuntimeException failure) {
                host.log().warn("Plugin stop failed " + container.manifest().id() + ": " + failure.getMessage());
            }
            try {
                container.loader().close();
            } catch (IOException failure) {
                host.log().warn("Plugin class loader close failed " + container.manifest().id());
            }
        }
        plugins.clear();
        startupExecutor.shutdownNow();
    }
}
