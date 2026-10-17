package fixture;

import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import vusiif.ztgui.server.api.CancellationToken;
import vusiif.ztgui.server.api.HostServices;
import vusiif.ztgui.server.api.LogLevel;
import vusiif.ztgui.server.api.PluginInfo;
import vusiif.ztgui.server.api.ServerPlugin;
import vusiif.ztgui.server.api.StartResult;
import vusiif.ztgui.server.api.StartTask;

/**
 * Black-box fixture plugin. Behaviour is selected by the ID declared in its own
 * {@code META-INF/ztserver-plugin.json}, so a single class backs every fixture jar:
 *
 * <ul>
 *   <li>{@code test-echo}, {@code test-other}: echo TCP and UDP payloads</li>
 *   <li>{@code test-failed}: startup task fails, so the plugin never becomes ready</li>
 *   <li>{@code test-cancellable}: startup waits for cancellation and records markers</li>
 *   <li>{@code test-mismatch}: reports a different info ID than its manifest</li>
 * </ul>
 */
public final class FixturePlugin implements ServerPlugin {
    private final String id;
    private final PluginInfo info;
    private HostServices services;

    public FixturePlugin() {
        String manifestText = readManifest();
        id = field(manifestText, "id");
        String reportedId = "test-mismatch".equals(id) ? "test-echo" : id;
        info = new PluginInfo(reportedId,
                field(manifestText, "name"), field(manifestText, "version"));
    }

    private static String readManifest() {
        try (InputStream input = FixturePlugin.class.getClassLoader()
                .getResourceAsStream("META-INF/ztserver-plugin.json")) {
            if (input == null) {
                return "";
            }
            return new String(input.readAllBytes(), StandardCharsets.UTF_8);
        } catch (Exception failure) {
            return "";
        }
    }

    private static String field(String text, String name) {
        Pattern pattern = Pattern.compile("\"" + Pattern.quote(name) + "\"\\s*:\\s*\"([^\"]*)\"");
        Matcher match = pattern.matcher(text);
        return match.find() ? match.group(1) : "";
    }

    @Override
    public PluginInfo info() {
        return info;
    }

    @Override
    public StartTask onStart(HostServices host, CancellationToken cancellation) {
        services = host;
        host.logs().write(LogLevel.INFO, "fixture-start");
        if ("test-failed".equals(id)) {
            return () -> StartResult.failure("fixture failure");
        }
        if ("test-cancellable".equals(id)) {
            write("started", "yes");
            return () -> {
                while (!cancellation.isCancellationRequested()) {
                    try {
                        Thread.sleep(20);
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        break;
                    }
                }
                write("cancelled", "yes");
                return StartResult.failure("cancelled");
            };
        }
        return null;
    }

    @Override
    public void onTcpPacket(long connection, byte[] payload) {
        if (services != null && !"test-failed".equals(id)) {
            services.sendTcp(connection, payload);
        }
    }

    @Override
    public void onUdpPacket(java.net.InetSocketAddress endpoint, byte[] payload) {
        if (services != null && !"test-failed".equals(id)) {
            services.sendUdp(endpoint, payload);
        }
    }

    @Override
    public void onConnectionClosed(long connection) {
        write("closed", "yes");
    }

    @Override
    public void onStop() {
        if (services != null) {
            services.logs().write(LogLevel.INFO, "fixture-stop");
        }
        if ("test-cancellable".equals(id)) {
            write("capture-destroyed", "yes");
        }
        write("stopped", "yes");
        services = null;
    }

    private void write(String name, String value) {
        if (services == null) {
            return;
        }
        services.files().writeFile(name, value.getBytes(StandardCharsets.UTF_8));
    }
}
