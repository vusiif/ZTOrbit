package vusiif.ztohub.main.zerotier;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import io.netty.channel.EventLoop;
import java.io.IOException;
import java.io.InputStream;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.regex.Pattern;
import vusiif.ztohub.api.Cancellable;
import vusiif.ztohub.api.QueryCallback;
import vusiif.ztohub.api.QueryResult;
import vusiif.ztohub.api.ZeroTierQueries;
import vusiif.ztohub.main.HostLog;
import vusiif.ztohub.main.JsonSupport;

/**
 * Asynchronous read-only ZeroTier service client.
 *
 * <p>HTTP requests go directly to {@code http://127.0.0.1:9993} with no proxy, no redirects, a
 * 10-second timeout and a 4 MiB response cap. The authentication token is read from
 * {@code /var/lib/zerotier-one/authtoken.secret} for every request and never logged. The
 * {@code ZTOHUB_ZT_API} and {@code ZTOHUB_ZT_TOKEN} environment variables exist only for
 * isolated tests and operational debugging.
 */
public final class ZeroTierClient implements ZeroTierQueries, AutoCloseable {
    private static final Pattern NETWORK_ID = Pattern.compile("^[0-9a-f]{16}$");
    private static final Pattern MEMBER_ID = Pattern.compile("^[0-9a-fA-F]{10}$");
    private static final int MAX_PENDING = 64;
    private static final int MAX_MEMBERS = 10000;
    private static final int MAX_RESPONSE_BYTES = 4 * 1024 * 1024;
    private static final int MEMBER_CONCURRENCY = 4;
    private static final long MEMBER_DEADLINE_NANOS = TimeUnit.SECONDS.toNanos(15);
    private static final Path DEFAULT_TOKEN_FILE = Path.of("/var/lib/zerotier-one/authtoken.secret");
    private static final String DEFAULT_BASE = "http://127.0.0.1:9993";

    private final EventLoop eventLoop;
    private final HostLog log;
    private final URI base;
    private final String tokenOverride;
    private final Path tokenFile;
    private final HttpClient http;
    private final ExecutorService workers = Executors.newVirtualThreadPerTaskExecutor();
    private final ExecutorService members = Executors.newFixedThreadPool(
            MEMBER_CONCURRENCY, Thread.ofVirtual().name("zt-member-", 0).factory());
    private final Set<Job> jobs = ConcurrentHashMap.newKeySet();
    private final AtomicInteger pending = new AtomicInteger();
    private volatile boolean stopped;

    public ZeroTierClient(EventLoop eventLoop, HostLog log) {
        this(eventLoop, log, parseBase(System.getenv("ZTOHUB_ZT_API")),
                text(System.getenv("ZTOHUB_ZT_TOKEN")), DEFAULT_TOKEN_FILE);
    }

    ZeroTierClient(EventLoop eventLoop, HostLog log, URI base, String tokenOverride, Path tokenFile) {
        this.eventLoop = eventLoop;
        this.log = log;
        this.base = base;
        this.tokenOverride = tokenOverride == null ? "" : tokenOverride;
        this.tokenFile = tokenFile;
        this.http = HttpClient.newBuilder()
                .proxy(NoProxySelector.INSTANCE)
                .followRedirects(HttpClient.Redirect.NEVER)
                .connectTimeout(Duration.ofSeconds(10))
                .build();
    }

    private static URI parseBase(String value) {
        if (value == null || value.isBlank()) {
            return URI.create(DEFAULT_BASE);
        }
        try {
            URI candidate = URI.create(value.trim());
            String scheme = candidate.getScheme();
            if (candidate.getHost() == null
                    || (!("http".equalsIgnoreCase(scheme)) && !("https".equalsIgnoreCase(scheme)))) {
                return URI.create(DEFAULT_BASE);
            }
            return candidate;
        } catch (IllegalArgumentException invalid) {
            return URI.create(DEFAULT_BASE);
        }
    }

    private static String text(String value) {
        return value == null ? "" : value.trim();
    }

    @Override
    public Cancellable query(String operation, JsonNode parameters, QueryCallback completed) {
        if (!eventLoop.inEventLoop()) {
            throw new IllegalStateException("ZeroTier queries must be issued on the host event thread");
        }
        Job job = new Job(completed);
        if (stopped || completed == null) {
            return job;
        }
        String op = operation == null ? "" : operation;
        String network = "";
        if ("controller.members".equals(op)) {
            JsonNode value = parameters == null ? null : parameters.get("networkId");
            String rawNetwork = value != null && value.isTextual() ? value.asText() : "";
            if (!NETWORK_ID.matcher(rawNetwork).matches()) {
                settle(job, QueryResult.failure("Invalid network ID"));
                return job;
            }
            network = rawNetwork;
        }
        if (pathFor(op, network) == null) {
            settle(job, QueryResult.failure("Unknown query operation"));
            return job;
        }
        if (pending.incrementAndGet() > MAX_PENDING) {
            pending.decrementAndGet();
            settle(job, QueryResult.failure("Too many pending queries"));
            return job;
        }
        jobs.add(job);
        String selectedNetwork = network;
        try {
            workers.execute(() -> execute(job, op, selectedNetwork));
        } catch (RejectedExecutionException rejected) {
            jobs.remove(job);
            pending.decrementAndGet();
            settle(job, QueryResult.failure("ZeroTier queries are stopping"));
        }
        return job;
    }

    private static String pathFor(String operation, String network) {
        return switch (operation) {
            case "info" -> "/status";
            case "peers" -> "/peer";
            case "networks" -> "/network";
            case "moons" -> "/moon";
            case "controller.networks" -> "/controller/network";
            case "controller.members" -> "/controller/network/" + network + "/member";
            default -> null;
        };
    }

    private void execute(Job job, String operation, String network) {
        try {
            if (cancelled(job)) {
                return;
            }
            TokenResult token = readToken();
            if (token.error != null) {
                settle(job, QueryResult.failure(token.error));
                return;
            }
            String path = pathFor(operation, network);
            QueryResult result = get(path, token.token);
            if (!result.ok() || cancelled(job)) {
                settle(job, result);
                return;
            }
            if ("controller.members".equals(operation)) {
                settle(job, queryMembers(job, path, token.token));
                return;
            }
            boolean valid = switch (operation) {
                case "info" -> result.document().isObject();
                case "controller.networks" ->
                        result.document().isArray() || result.document().isObject();
                default -> result.document().isArray();
            };
            settle(job, valid ? result : QueryResult.failure("Unexpected ZeroTier JSON type"));
        } catch (RuntimeException unexpected) {
            log.warn("ZeroTier query failed: " + unexpected.getMessage());
            settle(job, QueryResult.failure("ZeroTier query failed"));
        } finally {
            jobs.remove(job);
            pending.decrementAndGet();
        }
    }

    private QueryResult queryMembers(Job job, String path, String token) {
        QueryResult listing = get(path, token);
        if (!listing.ok()) {
            return listing;
        }
        JsonNode document = listing.document();
        List<String> ids = new ArrayList<>();
        if (document.isObject()) {
            document.fieldNames().forEachRemaining(ids::add);
        } else if (document.isArray()) {
            for (JsonNode value : document) {
                String id = value.isTextual()
                        ? value.asText()
                        : value.path("address").asText("");
                ids.add(id);
            }
        } else {
            return QueryResult.failure("Unexpected ZeroTier JSON type");
        }
        if (ids.size() > MAX_MEMBERS) {
            return QueryResult.failure("Too many controller members");
        }
        for (String id : ids) {
            if (!MEMBER_ID.matcher(id).matches()) {
                return QueryResult.failure("Invalid controller member ID");
            }
        }
        ArrayNode rows = JsonSupport.mapper().createArrayNode();
        if (ids.isEmpty()) {
            return QueryResult.success(rows);
        }
        long deadline = System.nanoTime() + MEMBER_DEADLINE_NANOS;
        for (int start = 0; start < ids.size(); start += MEMBER_CONCURRENCY) {
            if (cancelled(job) || stopped) {
                return null;
            }
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) {
                return QueryResult.failure("Controller members query timed out");
            }
            int end = Math.min(ids.size(), start + MEMBER_CONCURRENCY);
            List<Future<QueryResult>> futures = new ArrayList<>(end - start);
            for (int index = start; index < end; index++) {
                String id = ids.get(index);
                futures.add(members.submit(() -> get(path + "/" + id, token)));
            }
            for (int index = start; index < end; index++) {
                String id = ids.get(index);
                QueryResult member;
                try {
                    long wait = Math.max(1, deadline - System.nanoTime());
                    member = futures.get(index - start).get(wait, TimeUnit.NANOSECONDS);
                } catch (java.util.concurrent.TimeoutException timeout) {
                    return QueryResult.failure("Controller members query timed out");
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return QueryResult.failure("Controller members query interrupted");
                } catch (java.util.concurrent.ExecutionException failure) {
                    return QueryResult.failure("Controller members query failed");
                }
                if (!member.ok()) {
                    return member;
                }
                if (!member.document().isObject()) {
                    return QueryResult.failure("Invalid member response");
                }
                JsonNode object = member.document();
                ObjectNode row = rows.addObject();
                row.put("nodeId", object.path("address").asText(id));
                row.put("name", object.path("name").asText(""));
                row.put("authorized", object.path("authorized").asBoolean(false));
                row.set("ipAssignments", object.path("ipAssignments").isArray()
                        ? object.path("ipAssignments")
                        : JsonSupport.mapper().createArrayNode());
            }
        }
        return QueryResult.success(rows);
    }

    private QueryResult get(String path, String token) {
        URI uri;
        try {
            uri = base.resolve(path);
        } catch (IllegalArgumentException invalid) {
            return QueryResult.failure("Invalid ZeroTier service URL");
        }
        HttpRequest request = HttpRequest.newBuilder(uri)
                .timeout(Duration.ofSeconds(10))
                .header("X-ZT1-Auth", token)
                .GET()
                .build();
        try {
            HttpResponse<InputStream> response =
                    http.send(request, HttpResponse.BodyHandlers.ofInputStream());
            try (InputStream body = response.body()) {
                if (response.statusCode() != 200) {
                    return QueryResult.failure("ZeroTier query failed (HTTP " + response.statusCode() + ")");
                }
                byte[] data = readLimited(body);
                if (data == null) {
                    return QueryResult.failure("ZeroTier response too large");
                }
                JsonNode document;
                try {
                    document = JsonSupport.parse(data);
                } catch (IOException invalid) {
                    return QueryResult.failure("Invalid ZeroTier JSON");
                }
                return QueryResult.success(document);
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return QueryResult.failure("ZeroTier query interrupted");
        } catch (IOException failure) {
            return QueryResult.failure("ZeroTier query failed (network)");
        }
    }

    private static byte[] readLimited(InputStream input) throws IOException {
        byte[] buffer = new byte[65536];
        java.io.ByteArrayOutputStream collected = new java.io.ByteArrayOutputStream();
        int read;
        while ((read = input.read(buffer)) >= 0) {
            if (collected.size() + read > MAX_RESPONSE_BYTES) {
                return null;
            }
            collected.write(buffer, 0, read);
        }
        return collected.toByteArray();
    }

    private record TokenResult(String token, String error) {}

    private TokenResult readToken() {
        if (!tokenOverride.isEmpty()) {
            return new TokenResult(tokenOverride, null);
        }
        byte[] bytes;
        try {
            if (!Files.isRegularFile(tokenFile)) {
                return new TokenResult(null, "ZeroTier authentication file unavailable");
            }
            bytes = Files.readAllBytes(tokenFile);
        } catch (IOException failure) {
            return new TokenResult(null, "ZeroTier authentication file unavailable");
        }
        if (bytes.length > 4096) {
            return new TokenResult(null, "Invalid ZeroTier authentication file");
        }
        String token = new String(bytes, StandardCharsets.UTF_8).strip();
        if (token.isEmpty() || token.indexOf('\n') >= 0 || token.indexOf('\r') >= 0) {
            return new TokenResult(null, "Invalid ZeroTier authentication file");
        }
        return new TokenResult(token, null);
    }

    private boolean cancelled(Job job) {
        return job.cancelled.get() || stopped;
    }

    /** Delivers a result once, on the host event thread. */
    private void settle(Job job, QueryResult result) {
        if (result == null || !job.settled.compareAndSet(false, true)) {
            return;
        }
        Runnable deliver = () -> {
            if (stopped || job.cancelled.get()) {
                return;
            }
            try {
                job.callback.completed(result);
            } catch (RuntimeException failure) {
                log.warn("ZeroTier query callback failed: " + failure.getMessage());
            }
        };
        try {
            eventLoop.execute(deliver);
        } catch (RejectedExecutionException ignored) {
            // The host is shutting down; no callback is expected any more.
        }
    }

    @Override
    public void close() {
        stopped = true;
        for (Job job : jobs) {
            job.cancel();
        }
        jobs.clear();
        workers.shutdownNow();
        members.shutdownNow();
        http.close();
    }

    private final class Job implements Cancellable {
        private final QueryCallback callback;
        private final AtomicBoolean cancelled = new AtomicBoolean();
        private final AtomicBoolean settled = new AtomicBoolean();

        private Job(QueryCallback callback) {
            this.callback = callback;
        }

        @Override
        public void cancel() {
            cancelled.set(true);
        }
    }
}
