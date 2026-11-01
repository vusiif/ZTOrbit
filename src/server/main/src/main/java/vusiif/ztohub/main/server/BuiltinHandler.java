package vusiif.ztohub.main.server;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;
import vusiif.ztohub.api.Cancellable;
import vusiif.ztohub.main.JsonSupport;
import vusiif.ztohub.main.ServerHost;

/** Handles the {@code builtin} route: read-only ZeroTier queries and network transport ops. */
public final class BuiltinHandler {
    private final ServerHost host;
    private final Map<Long, Set<Cancellable>> pending = new HashMap<>();

    public BuiltinHandler(ServerHost host) {
        this.host = host;
    }

    public void handle(Connection connection, byte[] payload) {
        JsonNode request;
        try {
            request = JsonSupport.parse(payload);
        } catch (IOException invalid) {
            host.disconnect(connection.id());
            return;
        }
        if (request == null || !request.isObject()
                || !request.path("v").isIntegralNumber() || request.path("v").asInt() != 1
                || !request.has("id")) {
            host.disconnect(connection.id());
            return;
        }
        JsonNode serial = request.get("id");
        String operation = request.path("op").isTextual() ? request.path("op").asText() : "";
        if (operation.startsWith("network.")) {
            dispatchNetwork(connection, request, serial);
            return;
        }
        JsonNode data = request.path("data");
        JsonNode parameters = data.isObject() ? data : JsonSupport.object();
        Cancellable[] holder = new Cancellable[1];
        holder[0] = host.zeroTier().query(operation, parameters, result -> {
            untrack(connection.id(), holder[0]);
            ObjectNode response = JsonSupport.object();
            response.put("v", 1);
            response.set("id", serial.deepCopy());
            if (result.ok()) {
                response.put("ok", true);
                response.set("data", result.document());
            } else {
                response.put("ok", false);
                response.put("error", result.error());
            }
            host.sendTcp(Wire.BUILTIN, connection.id(), JsonSupport.compact(response));
        });
        track(connection.id(), holder[0]);
    }

    private void dispatchNetwork(Connection connection, JsonNode request, JsonNode serial) {
        InetSocketAddress remote = connection.channel().remoteAddress() instanceof InetSocketAddress socket
                ? socket : null;
        InetAddress source = remote == null ? null : remote.getAddress();
        if (source == null) {
            host.disconnect(connection.id());
            return;
        }
        Cancellable[] holder = new Cancellable[1];
        holder[0] = host.transport().builtin(source, request, response -> {
            untrack(connection.id(), holder[0]);
            ObjectNode output = response.deepCopy();
            output.put("v", 1);
            output.set("id", serial.deepCopy());
            host.sendTcp(Wire.BUILTIN, connection.id(), JsonSupport.compact(output));
        });
        track(connection.id(), holder[0]);
    }

    private void track(long connectionId, Cancellable job) {
        if (job == null) {
            return;
        }
        pending.computeIfAbsent(connectionId, ignored -> new LinkedHashSet<>()).add(job);
    }

    private void untrack(long connectionId, Cancellable job) {
        Set<Cancellable> jobs = pending.get(connectionId);
        if (jobs == null) {
            return;
        }
        jobs.remove(job);
        if (jobs.isEmpty()) {
            pending.remove(connectionId);
        }
    }

    /** Cancels every pending query/transport request for a closed connection. */
    public void cancelConnection(long connectionId) {
        Set<Cancellable> jobs = pending.remove(connectionId);
        if (jobs == null) {
            return;
        }
        for (Cancellable job : new ArrayList<>(jobs)) {
            job.cancel();
        }
    }

    public int pendingCount(long connectionId) {
        Set<Cancellable> jobs = pending.get(connectionId);
        return jobs == null ? 0 : jobs.size();
    }
}
