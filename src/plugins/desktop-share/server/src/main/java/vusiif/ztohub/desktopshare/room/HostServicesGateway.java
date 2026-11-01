package vusiif.ztohub.desktopshare.room;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import vusiif.ztohub.api.Cancellable;
import vusiif.ztohub.api.HostServices;
import vusiif.ztohub.api.LogLevel;

/** {@link HostGateway} backed by the public {@link HostServices}. */
public final class HostServicesGateway implements HostGateway {
    private final HostServices host;

    public HostServicesGateway(HostServices host) {
        this.host = host;
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
        return host.sendTcp(connectionId, payload);
    }

    @Override
    public boolean sendUdp(InetSocketAddress endpoint, byte[] payload) {
        return host.sendUdp(endpoint, payload);
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
    public Cancellable scheduleAtFixedRate(Runnable task, long periodMillis) {
        ScheduledFuture<?> future = host.scheduler().scheduleAtFixedRate(
                task, periodMillis, periodMillis, TimeUnit.MILLISECONDS);
        return () -> future.cancel(false);
    }

    @Override
    public void info(String message) {
        host.logs().write(LogLevel.INFO, message);
    }

    @Override
    public void warn(String message) {
        host.logs().write(LogLevel.WARNING, message);
    }
}
