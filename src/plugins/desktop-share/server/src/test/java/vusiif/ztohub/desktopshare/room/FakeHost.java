package vusiif.ztohub.desktopshare.room;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import vusiif.ztohub.api.Cancellable;

/** In-memory host gateway; records every outbound frame and UDP datagram. */
final class FakeHost implements HostGateway {
    record UdpSend(InetSocketAddress endpoint, byte[] payload) {}

    private final Map<Long, InetAddress> peers = new LinkedHashMap<>();
    private final Map<Long, List<byte[]>> tcp = new LinkedHashMap<>();
    private final Map<Long, Long> queued = new HashMap<>();
    private final Set<Long> failing = new HashSet<>();
    private final Set<InetSocketAddress> failingUdp = new HashSet<>();
    private final List<UdpSend> udp = new ArrayList<>();
    private final List<Long> disconnected = new ArrayList<>();
    final List<String> logs = new ArrayList<>();

    private Runnable scheduled;
    private boolean scheduledCanceled;
    private long scheduledPeriodMillis;
    private int scheduleCalls;

    void addConnection(long id, String address) {
        try {
            peers.put(id, InetAddress.getByName(address));
        } catch (Exception error) {
            throw new IllegalArgumentException("invalid test address " + address, error);
        }
        tcp.put(id, new ArrayList<>());
    }

    void removeConnection(long id) {
        peers.remove(id);
    }

    void failSends(long id) {
        failing.add(id);
    }

    void failUdpTo(InetSocketAddress endpoint) {
        failingUdp.add(endpoint);
    }

    void allowUdpTo(InetSocketAddress endpoint) {
        failingUdp.remove(endpoint);
    }

    void setQueuedBytes(long id, long bytes) {
        queued.put(id, bytes);
    }

    List<byte[]> messages(long id) {
        return tcp.getOrDefault(id, List.of());
    }

    List<UdpSend> udpSends() {
        return udp;
    }

    List<Long> disconnected() {
        return disconnected;
    }

    boolean maintenanceScheduled() {
        return scheduled != null;
    }

    boolean maintenanceCanceled() {
        return scheduledCanceled;
    }

    long scheduledPeriodMillis() {
        return scheduledPeriodMillis;
    }

    int scheduleCalls() {
        return scheduleCalls;
    }

    void runMaintenance() {
        if (scheduled != null && !scheduledCanceled) {
            scheduled.run();
        }
    }

    @Override
    public int port() {
        return 18100;
    }

    @Override
    public InetAddress peerAddress(long connectionId) {
        return peers.get(connectionId);
    }

    @Override
    public boolean sendTcp(long connectionId, byte[] payload) {
        if (payload == null || !tcp.containsKey(connectionId) || failing.contains(connectionId)) {
            return false;
        }
        tcp.get(connectionId).add(payload.clone());
        return true;
    }

    @Override
    public boolean sendUdp(InetSocketAddress endpoint, byte[] payload) {
        if (endpoint == null || payload == null || failingUdp.contains(endpoint)) {
            return false;
        }
        udp.add(new UdpSend(endpoint, payload.clone()));
        return true;
    }

    @Override
    public void disconnect(long connectionId) {
        disconnected.add(connectionId);
    }

    @Override
    public long queuedTcpBytes(long connectionId) {
        return queued.getOrDefault(connectionId, 0L);
    }

    @Override
    public Cancellable scheduleAtFixedRate(Runnable task, long periodMillis) {
        scheduleCalls++;
        scheduled = task;
        scheduledPeriodMillis = periodMillis;
        return () -> scheduledCanceled = true;
    }

    @Override
    public void info(String message) {
        logs.add("INFO " + message);
    }

    @Override
    public void warn(String message) {
        logs.add("WARN " + message);
    }
}
