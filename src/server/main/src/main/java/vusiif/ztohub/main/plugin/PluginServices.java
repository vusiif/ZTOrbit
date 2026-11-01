package vusiif.ztohub.main.plugin;

import com.fasterxml.jackson.databind.JsonNode;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.atomic.AtomicReference;
import vusiif.ztohub.api.Cancellable;
import vusiif.ztohub.api.HostServices;
import vusiif.ztohub.api.NetworkCallback;
import vusiif.ztohub.api.NetworkSubscription;
import vusiif.ztohub.api.PluginFiles;
import vusiif.ztohub.api.PluginLogger;
import vusiif.ztohub.api.RelayCallback;
import vusiif.ztohub.api.RelayResult;
import vusiif.ztohub.api.SendCallback;
import vusiif.ztohub.api.SendResult;
import vusiif.ztohub.api.ZeroTierQueries;
import vusiif.ztohub.main.ServerHost;

/** Per-plugin {@link HostServices} implementation. */
public final class PluginServices implements HostServices {
    private static final Cancellable NOOP = () -> {};

    private final ServerHost host;
    private final String pluginId;
    private final PluginFilesImpl files;
    private final PluginLogWriter logs;
    private final ZeroTierQueries zeroTier;
    private final Set<Tracked> requests = ConcurrentHashMap.newKeySet();
    private volatile boolean stopped;

    PluginServices(ServerHost host, String pluginId, String pluginName) {
        this.host = host;
        this.pluginId = pluginId;
        this.files = new PluginFilesImpl(host.workingDirectory(), pluginName);
        this.logs = new PluginLogWriter(host.log(), pluginId, files);
        this.zeroTier = (operation, parameters, completed) ->
                query(operation, parameters, completed);
    }

    @Override
    public PluginFiles files() {
        return files;
    }

    @Override
    public PluginLogger logs() {
        return logs;
    }

    @Override
    public ZeroTierQueries zeroTier() {
        return zeroTier;
    }

    @Override
    public ScheduledExecutorService scheduler() {
        return host.eventLoop();
    }

    @Override
    public int port() {
        return host.port();
    }

    @Override
    public InetAddress peerAddress(long connectionId) {
        return host.peerAddress(connectionId);
    }

    @Override
    public boolean sendTcp(long connectionId, byte[] payload) {
        return host.sendTcp(pluginId, connectionId, payload);
    }

    @Override
    public boolean sendUdp(InetSocketAddress endpoint, byte[] payload) {
        return host.sendUdp(pluginId, endpoint, payload);
    }

    @Override
    public void disconnect(long connectionId) {
        host.disconnect(connectionId);
    }

    @Override
    public long queuedTcpBytes(long connectionId) {
        return host.queuedTcpBytes(connectionId);
    }

    @Override
    public Cancellable sendDataTo(
            String networkId, String memberId, byte[] data, SendCallback completed) {
        return broadcastDataTo(networkId, List.of(memberId), data, completed);
    }

    @Override
    public Cancellable broadcastDataTo(
            String networkId, List<String> memberIds, byte[] data, SendCallback completed) {
        return send(networkId, memberIds, data, false, completed);
    }

    @Override
    public Cancellable broadcastDataBalancedTo(
            String networkId, List<String> memberIds, byte[] data, SendCallback completed) {
        return send(networkId, memberIds, data, true, completed);
    }

    private Cancellable send(
            String networkId, List<String> memberIds, byte[] data, boolean balanced, SendCallback completed) {
        if (stopped) {
            return NOOP;
        }
        Tracked tracked = new Tracked();
        Cancellable delegate = host.transport().send(
                pluginId, networkId, memberIds, data, balanced, result -> {
                    tracked.finish();
                    if (completed != null) {
                        try {
                            completed.completed(result == null ? SendResult.nothing() : result);
                        } catch (RuntimeException failure) {
                            callbackFailed("send", failure);
                        }
                    }
                });
        return tracked.attach(delegate);
    }

    @Override
    public Cancellable getNetworkRelays(String networkId, RelayCallback completed) {
        if (stopped) {
            return NOOP;
        }
        Tracked tracked = new Tracked();
        Cancellable delegate = host.transport().relays(networkId, result -> {
            tracked.finish();
            if (completed != null) {
                try {
                    completed.completed(result == null
                            ? RelayResult.failure("network_unavailable") : result);
                } catch (RuntimeException failure) {
                    callbackFailed("relay", failure);
                }
            }
        });
        return tracked.attach(delegate);
    }

    @Override
    public NetworkSubscription subscribeNetworkData(String networkId, NetworkCallback received) {
        if (stopped) {
            return () -> {};
        }
        NetworkSubscription subscription = host.transport().subscribe(pluginId, networkId, received);
        Tracked tracked = new Tracked();
        tracked.attach(subscription);
        return tracked;
    }

    private Cancellable query(String operation, JsonNode parameters, vusiif.ztohub.api.QueryCallback completed) {
        if (stopped) {
            return NOOP;
        }
        Tracked tracked = new Tracked();
        Cancellable delegate = host.zeroTier().query(operation, parameters, result -> {
            tracked.finish();
            if (completed != null) {
                try {
                    completed.completed(result);
                } catch (RuntimeException failure) {
                    callbackFailed("query", failure);
                }
            }
        });
        return tracked.attach(delegate);
    }

    private void callbackFailed(String kind, RuntimeException failure) {
        String message = failure.getMessage() == null ? failure.toString() : failure.getMessage();
        host.log().warn("Plugin " + kind + " callback failed for " + pluginId + ": " + message);
    }

    /** Cancels every pending query, subscription and send callback owned by the plugin. */
    public void stopRequests() {
        stopped = true;
        for (Tracked tracked : new ArrayList<>(requests)) {
            tracked.cancel();
        }
        requests.clear();
    }

    String pluginId() {
        return pluginId;
    }

    /** Cancellable handle that also removes itself from the owning plugin request set. */
    private final class Tracked implements Cancellable, NetworkSubscription {
        private final AtomicReference<Cancellable> delegate = new AtomicReference<>();
        private volatile boolean finished;

        Cancellable attach(Cancellable target) {
            delegate.set(target);
            if (finished) {
                target.cancel();
                return this;
            }
            requests.add(this);
            if (stopped) {
                cancel();
            }
            return this;
        }

        void finish() {
            finished = true;
            requests.remove(this);
        }

        @Override
        public void cancel() {
            finished = true;
            requests.remove(this);
            Cancellable target = delegate.get();
            if (target != null) {
                target.cancel();
            }
        }
    }

    /** Exposed for tests that need to assert cleanup of pending work. */
    public int pendingRequestCount() {
        return requests.size();
    }
}
