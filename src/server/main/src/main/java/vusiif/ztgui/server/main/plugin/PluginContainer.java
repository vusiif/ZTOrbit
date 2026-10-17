package vusiif.ztgui.server.main.plugin;

import java.net.URLClassLoader;
import java.util.concurrent.CompletableFuture;
import vusiif.ztgui.server.api.CancellationToken;
import vusiif.ztgui.server.api.ServerPlugin;
import vusiif.ztgui.server.api.StartResult;

/** Runtime state of one loaded plugin jar. */
public final class PluginContainer {
    private final PluginManifest manifest;
    private final ServerPlugin plugin;
    private final URLClassLoader loader;
    private final PluginServices services;
    private final CancellationSource cancellation = new CancellationSource();
    private volatile boolean ready;
    private volatile CompletableFuture<StartResult> startup;

    PluginContainer(
            PluginManifest manifest,
            ServerPlugin plugin,
            URLClassLoader loader,
            PluginServices services) {
        this.manifest = manifest;
        this.plugin = plugin;
        this.loader = loader;
        this.services = services;
    }

    public PluginManifest manifest() {
        return manifest;
    }

    public ServerPlugin plugin() {
        return plugin;
    }

    public PluginServices services() {
        return services;
    }

    public URLClassLoader loader() {
        return loader;
    }

    public CancellationToken cancellation() {
        return cancellation;
    }

    CancellationSource cancellationSource() {
        return cancellation;
    }

    public boolean isReady() {
        return ready;
    }

    void setReady(boolean value) {
        ready = value;
    }

    CompletableFuture<StartResult> startup() {
        return startup;
    }

    void setStartup(CompletableFuture<StartResult> value) {
        startup = value;
    }

    /** Cancellation signal handed to {@code onStart}. */
    static final class CancellationSource implements CancellationToken {
        private volatile boolean cancelled;

        @Override
        public boolean isCancellationRequested() {
            return cancelled;
        }

        void cancel() {
            cancelled = true;
        }
    }
}
