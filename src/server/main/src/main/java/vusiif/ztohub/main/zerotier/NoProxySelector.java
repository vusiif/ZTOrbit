package vusiif.ztohub.main.zerotier;

import java.io.IOException;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.SocketAddress;
import java.net.URI;
import java.util.List;

/** Disables proxy selection for host-internal ZeroTier HTTP requests. */
final class NoProxySelector extends ProxySelector {
    static final NoProxySelector INSTANCE = new NoProxySelector();

    private NoProxySelector() {}

    @Override
    public List<Proxy> select(URI uri) {
        return List.of(Proxy.NO_PROXY);
    }

    @Override
    public void connectFailed(URI uri, SocketAddress address, IOException failure) {
        // The HttpClient reports the failure to the caller.
    }
}
