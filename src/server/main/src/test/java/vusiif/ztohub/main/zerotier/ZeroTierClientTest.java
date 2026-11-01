package vusiif.ztohub.main.zerotier;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpHandler;
import com.sun.net.httpserver.HttpServer;
import io.netty.channel.EventLoop;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import vusiif.ztohub.api.Cancellable;
import vusiif.ztohub.api.QueryResult;
import vusiif.ztohub.main.HostLog;

class ZeroTierClientTest {
    private static final ObjectMapper JSON = new ObjectMapper();
    @TempDir
    Path root;

    private EventLoopGroup group;
    private EventLoop loop;
    private HostLog log;
    private final List<HttpServer> servers = new ArrayList<>();
    private ZeroTierClient client;

    @BeforeEach
    void setUp() {
        group = new NioEventLoopGroup(1,
                (java.util.concurrent.ThreadFactory) runnable -> new Thread(runnable, "zt-test"));
        loop = group.next();
        log = new HostLog(root.resolve("host"));
    }

    @AfterEach
    void tearDown() {
        if (client != null) {
            client.close();
        }
        for (HttpServer server : servers) {
            server.stop(0);
        }
        log.close();
        group.shutdownGracefully(0, 5, TimeUnit.SECONDS).syncUninterruptibly();
    }

    private HttpServer serve(HttpHandler handler) throws IOException {
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        server.createContext("/", handler);
        server.setExecutor(java.util.concurrent.Executors.newCachedThreadPool(runnable -> {
            Thread thread = new Thread(runnable, "zt-mock-http");
            thread.setDaemon(true);
            return thread;
        }));
        server.start();
        servers.add(server);
        return server;
    }

    private ZeroTierClient client(HttpServer server, String token) {
        client = new ZeroTierClient(loop, log,
                java.net.URI.create("http://127.0.0.1:" + server.getAddress().getPort()),
                token, root.resolve("missing-token"));
        return client;
    }

    private static void respond(HttpExchange exchange, int status, byte[] body) throws IOException {
        exchange.getResponseHeaders().set("Content-Type", "application/json");
        exchange.sendResponseHeaders(status, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }

    private QueryResult query(ZeroTierClient client, String operation, String parameters)
            throws Exception {
        CompletableFuture<QueryResult> future = new CompletableFuture<>();
        loop.submit(() -> client.query(operation,
                parameters == null ? JSON.createObjectNode() : JSON.readTree(parameters),
                future::complete)).get(5, TimeUnit.SECONDS);
        return future.get(15, TimeUnit.SECONDS);
    }

    @Test
    void sendsTokenAndParsesInfo() throws Exception {
        AtomicReference<String> seen = new AtomicReference<>();
        HttpServer server = serve(exchange -> {
            seen.set(exchange.getRequestHeaders().getFirst("X-ZT1-Auth"));
            respond(exchange, 200, "{\"address\":\"0123456789\"}".getBytes(StandardCharsets.UTF_8));
        });
        QueryResult result = query(client(server, "secret-token"), "info", null);
        assertTrue(result.ok(), result.error());
        assertEquals("0123456789", result.document().path("address").asText());
        assertEquals("secret-token", seen.get());
    }

    @Test
    void reportsHttpErrorsWithoutLeakingToken() throws Exception {
        HttpServer server = serve(exchange ->
                respond(exchange, 500, "server error".getBytes(StandardCharsets.UTF_8)));
        QueryResult result = query(client(server, "secret-token"), "peers", null);
        assertFalse(result.ok());
        assertTrue(result.error().contains("HTTP 500"), result.error());
        assertFalse(result.error().contains("secret-token"));
    }

    @Test
    void rejectsInvalidJsonAndWrongShape() throws Exception {
        HttpServer invalid = serve(exchange ->
                respond(exchange, 200, "not json".getBytes(StandardCharsets.UTF_8)));
        assertTrue(query(client(invalid, "token"), "info", null).error().contains("Invalid ZeroTier JSON"));

        HttpServer wrongShape = serve(exchange ->
                respond(exchange, 200, "{}".getBytes(StandardCharsets.UTF_8)));
        assertTrue(query(client(wrongShape, "token"), "peers", null)
                .error().contains("Unexpected ZeroTier JSON type"));
    }

    @Test
    void capsResponsesAtFourMiB() throws Exception {
        byte[] huge = new byte[4 * 1024 * 1024 + 1];
        java.util.Arrays.fill(huge, (byte) ' ');
        HttpServer server = serve(exchange -> respond(exchange, 200, huge));
        QueryResult result = query(client(server, "token"), "info", null);
        assertFalse(result.ok());
        assertTrue(result.error().contains("too large"), result.error());
    }

    @Test
    void rejectsMissingAndInvalidTokenFiles() throws Exception {
        HttpServer server = serve(exchange -> respond(exchange, 200,
                "{}".getBytes(StandardCharsets.UTF_8)));
        client = new ZeroTierClient(loop, log,
                java.net.URI.create("http://127.0.0.1:" + server.getAddress().getPort()),
                "", root.resolve("missing-token"));
        assertEquals("ZeroTier authentication file unavailable",
                query(client, "info", null).error());
        Path tokenFile = root.resolve("token");
        Files.writeString(tokenFile, "bad\ntoken");
        client.close();
        client = new ZeroTierClient(loop, log,
                java.net.URI.create("http://127.0.0.1:" + server.getAddress().getPort()),
                "", tokenFile);
        assertEquals("Invalid ZeroTier authentication file", query(client, "info", null).error());
    }

    @Test
    void invalidNetworkIdFailsBeforeHttp() throws Exception {
        HttpServer server = serve(exchange -> {
            respond(exchange, 500, new byte[0]);
            return;
        });
        QueryResult result = query(client(server, "token"), "controller.members",
                "{\"networkId\":\"bad\"}");
        assertEquals("Invalid network ID", result.error());
        result = query(client, "controller.members", "{\"networkId\":401}");
        assertEquals("Invalid network ID", result.error());
        result = query(client, "controller.members", "{\"networkId\":\"0123456789ABCDEF\"}");
        assertEquals("Invalid network ID", result.error());
        result = query(client, "unknown-operation", null);
        assertEquals("Unknown query operation", result.error());
    }

    @Test
    void aggregatesControllerMembers() throws Exception {
        HttpServer server = serve(exchange -> {
            String path = exchange.getRequestURI().getPath();
            if (path.equals("/controller/network/0123456789abcdef/member")) {
                respond(exchange, 200,
                        "{\"0123456789\":\"\",\"abcdef0123\":\"\"}".getBytes(StandardCharsets.UTF_8));
                return;
            }
            String node = path.substring(path.lastIndexOf('/') + 1);
            respond(exchange, 200, ("{\"address\":\"" + node
                    + "\",\"name\":\"member-" + node
                    + "\",\"authorized\":true,\"ipAssignments\":[\"10.0.0.2/24\"]}")
                    .getBytes(StandardCharsets.UTF_8));
        });
        QueryResult result = query(client(server, "token"), "controller.members",
                "{\"networkId\":\"0123456789abcdef\"}");
        assertTrue(result.ok(), result.error());
        JsonNode rows = result.document();
        assertEquals(2, rows.size());
        assertEquals("0123456789", rows.get(0).path("nodeId").asText());
        assertEquals("member-0123456789", rows.get(0).path("name").asText());
        assertTrue(rows.get(0).path("authorized").asBoolean());
        assertEquals("10.0.0.2/24", rows.get(0).path("ipAssignments").get(0).asText());
    }

    @Test
    void rejectsTooManyOrInvalidMembers() throws Exception {
        HttpServer invalidMember = serve(exchange -> {
            String path = exchange.getRequestURI().getPath();
            if (path.endsWith("/member")) {
                respond(exchange, 200, "{\"not-a-node\":\"\"}".getBytes(StandardCharsets.UTF_8));
            } else {
                respond(exchange, 200, "{}".getBytes(StandardCharsets.UTF_8));
            }
        });
        assertEquals("Invalid controller member ID", query(client(invalidMember, "token"),
                "controller.members", "{\"networkId\":\"0123456789abcdef\"}").error());
    }

    @Test
    void cancellationSuppressesCallback() throws Exception {
        HttpServer server = serve(exchange -> {
            try {
                Thread.sleep(400);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
            respond(exchange, 200, "{}".getBytes(StandardCharsets.UTF_8));
        });
        CompletableFuture<QueryResult> future = new CompletableFuture<>();
        AtomicReference<Cancellable> handle = new AtomicReference<>();
        loop.submit(() -> handle.set(client(server, "token").query("info",
                JSON.createObjectNode(), future::complete))).get(5, TimeUnit.SECONDS);
        assertNotNull(handle.get());
        handle.get().cancel();
        Thread.sleep(700);
        assertFalse(future.isDone());
    }

    @Test
    void limitsPendingQueries() throws Exception {
        HttpServer server = serve(exchange -> {
            try {
                Thread.sleep(300);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
            respond(exchange, 200, "{}".getBytes(StandardCharsets.UTF_8));
        });
        ZeroTierClient client = client(server, "token");
        List<CompletableFuture<QueryResult>> futures = new ArrayList<>();
        CountDownLatch submitted = new CountDownLatch(65);
        for (int index = 0; index < 65; index++) {
            CompletableFuture<QueryResult> future = new CompletableFuture<>();
            futures.add(future);
            loop.execute(() -> {
                client.query("info", JSON.createObjectNode(), future::complete);
                submitted.countDown();
            });
        }
        assertTrue(submitted.await(5, TimeUnit.SECONDS));
        int tooMany = 0;
        for (CompletableFuture<QueryResult> future : futures) {
            QueryResult result = future.get(20, TimeUnit.SECONDS);
            if ("Too many pending queries".equals(result.error())) {
                tooMany++;
            }
        }
        assertEquals(1, tooMany);
    }

    @Test
    void emptyMemberListReturnsEmptyArray() throws Exception {
        HttpServer server = serve(exchange -> {
            if (exchange.getRequestURI().getPath().endsWith("/member")) {
                respond(exchange, 200, "{}".getBytes(StandardCharsets.UTF_8));
            } else {
                respond(exchange, 200, "[]".getBytes(StandardCharsets.UTF_8));
            }
        });
        QueryResult result = query(client(server, "token"), "controller.members",
                "{\"networkId\":\"0123456789abcdef\"}");
        assertTrue(result.ok(), result.error());
        assertTrue(result.document().isArray());
        assertEquals(0, result.document().size());
        assertNull(QueryResult.failure("x").document());
    }
}
