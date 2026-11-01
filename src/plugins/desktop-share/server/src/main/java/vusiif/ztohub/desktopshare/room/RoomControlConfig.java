package vusiif.ztohub.desktopshare.room;

import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.StreamReadConstraints;
import com.fasterxml.jackson.core.StreamReadFeature;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Set;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;
import vusiif.ztohub.api.FileResult;
import vusiif.ztohub.api.PluginFiles;

/**
 * Startup capacity configuration for the desktop-share server plugin.
 *
 * <p>The file is {@code config.json} in the plugin data directory and is read through
 * the public {@link PluginFiles} service. It is read once in {@code onStart}; changing
 * any value requires a server restart. Missing files and missing fields use the
 * documented defaults. Invalid input fails startup with a diagnostic instead of
 * silently weakening or raising a limit.
 *
 * <p>Every value has a hard project maximum. A configuration file can lower a limit
 * but cannot raise it above the limits in {@code STEP.md}; clients have no control
 * channel for these values.
 */
public record RoomControlConfig(
        int maxRooms,
        int maxViewersPerRoom,
        int maxMembersPerServer,
        int egressBudgetMbps,
        int controlRequestsPerSecond,
        int controlRequestBurst,
        int joinFailureLimitPerMinute,
        int joinFailureSources) {

    public static final String CONFIG_FILE = "config.json";
    public static final int MAX_CONFIG_BYTES = 64 * 1024;

    public static final int HARD_MAX_ROOMS = ControlProtocol.MAX_ROOMS;
    public static final int HARD_MAX_VIEWERS_PER_ROOM = ControlProtocol.MAX_VIEWERS_PER_ROOM;
    public static final int HARD_MAX_MEMBERS_PER_SERVER = ControlProtocol.MAX_DESKTOP_SESSIONS;
    public static final int HARD_MAX_EGRESS_MBPS = 160;
    public static final int HARD_MAX_CONTROL_REQUESTS_PER_SECOND = 20;
    public static final int HARD_MAX_CONTROL_REQUEST_BURST = 20;
    public static final int HARD_MAX_JOIN_FAILURE_LIMIT = 10;
    public static final int HARD_MAX_JOIN_FAILURE_SOURCES = 4096;

    private static final ObjectMapper MAPPER;

    static {
        JsonFactory factory = JsonFactory.builder()
                .enable(StreamReadFeature.STRICT_DUPLICATE_DETECTION)
                .build();
        factory.setStreamReadConstraints(StreamReadConstraints.builder()
                .maxNestingDepth(16)
                .maxStringLength(1024)
                .build());
        MAPPER = new ObjectMapper(factory);
        MAPPER.enable(DeserializationFeature.FAIL_ON_TRAILING_TOKENS);
    }

    public static RoomControlConfig defaults() {
        return new RoomControlConfig(
                HARD_MAX_ROOMS,
                HARD_MAX_VIEWERS_PER_ROOM,
                HARD_MAX_MEMBERS_PER_SERVER,
                HARD_MAX_EGRESS_MBPS,
                HARD_MAX_CONTROL_REQUESTS_PER_SECOND,
                HARD_MAX_CONTROL_REQUEST_BURST,
                HARD_MAX_JOIN_FAILURE_LIMIT,
                HARD_MAX_JOIN_FAILURE_SOURCES);
    }

    public Tuning toTuning() {
        return Tuning.builder()
                .maxRooms(maxRooms)
                .maxViewersPerRoom(maxViewersPerRoom)
                .maxMembersPerServer(maxMembersPerServer)
                .egressBudgetBitsPerSecond(egressBudgetMbps * 1_000_000L)
                .requestsPerSecond(controlRequestsPerSecond)
                .requestBurst(controlRequestBurst)
                .joinFailureLimit(joinFailureLimitPerMinute)
                .joinFailureSources(joinFailureSources)
                .build();
    }

    /** Reads and validates the optional file, returning a diagnostic instead of throwing. */
    public static LoadResult read(PluginFiles files) {
        FileResult result;
        try {
            result = files.readFile(CONFIG_FILE);
        } catch (RuntimeException failure) {
            return new LoadResult(null, "读取 " + CONFIG_FILE + " 失败：" + message(failure),
                    false);
        }
        if (!result.ok()) {
            if ("not found".equals(result.error())) {
                return new LoadResult(defaults(), "", false);
            }
            return new LoadResult(null, "读取 " + CONFIG_FILE + " 失败：" + result.error(),
                    false);
        }
        try {
            return new LoadResult(fromJson(result.data()), "", true);
        } catch (ConfigException failure) {
            return new LoadResult(null, CONFIG_FILE + " 无效：" + failure.getMessage(), true);
        }
    }

    /** Parses one UTF-8 JSON object. Every field is optional; unknown fields are rejected. */
    public static RoomControlConfig fromJson(byte[] bytes) throws ConfigException {
        if (bytes == null || bytes.length == 0) {
            throw new ConfigException("配置不能为空");
        }
        if (bytes.length > MAX_CONFIG_BYTES) {
            throw new ConfigException("配置超过 " + MAX_CONFIG_BYTES + " 字节上限");
        }
        JsonNode root;
        try {
            root = MAPPER.readTree(bytes);
        } catch (Exception failure) {
            throw new ConfigException("JSON 解析失败：" + message(failure));
        }
        if (root == null || !root.isObject()) {
            throw new ConfigException("配置必须是 JSON 对象");
        }
        Set<String> known = new HashSet<>();
        known.add("maxRooms");
        known.add("maxViewersPerRoom");
        known.add("maxMembersPerServer");
        known.add("egressBudgetMbps");
        known.add("controlRequestsPerSecond");
        known.add("controlRequestBurst");
        known.add("joinFailureLimitPerMinute");
        known.add("joinFailureSources");
        Iterator<String> names = root.fieldNames();
        while (names.hasNext()) {
            String name = names.next();
            if (!known.contains(name)) {
                throw new ConfigException("未知字段 " + name);
            }
        }
        RoomControlConfig defaults = defaults();
        return new RoomControlConfig(
                integer(root, "maxRooms", defaults.maxRooms, 1, HARD_MAX_ROOMS),
                integer(root, "maxViewersPerRoom", defaults.maxViewersPerRoom, 1,
                        HARD_MAX_VIEWERS_PER_ROOM),
                integer(root, "maxMembersPerServer", defaults.maxMembersPerServer, 1,
                        HARD_MAX_MEMBERS_PER_SERVER),
                integer(root, "egressBudgetMbps", defaults.egressBudgetMbps, 1,
                        HARD_MAX_EGRESS_MBPS),
                integer(root, "controlRequestsPerSecond",
                        defaults.controlRequestsPerSecond, 1,
                        HARD_MAX_CONTROL_REQUESTS_PER_SECOND),
                integer(root, "controlRequestBurst", defaults.controlRequestBurst, 1,
                        HARD_MAX_CONTROL_REQUEST_BURST),
                integer(root, "joinFailureLimitPerMinute",
                        defaults.joinFailureLimitPerMinute, 1,
                        HARD_MAX_JOIN_FAILURE_LIMIT),
                integer(root, "joinFailureSources", defaults.joinFailureSources, 1,
                        HARD_MAX_JOIN_FAILURE_SOURCES));
    }

    private static int integer(JsonNode root, String field, int fallback, int minimum,
            int maximum) throws ConfigException {
        JsonNode node = root.get(field);
        if (node == null) {
            return fallback;
        }
        if (!node.isIntegralNumber() || !node.canConvertToInt()) {
            throw new ConfigException(field + " 必须是 " + minimum + ".." + maximum
                    + " 的整数");
        }
        int value = node.intValue();
        if (value < minimum || value > maximum) {
            throw new ConfigException(field + " 必须在 " + minimum + ".." + maximum
                    + " 之间");
        }
        return value;
    }

    private static String message(Throwable failure) {
        String text = failure.getMessage();
        return text == null || text.isBlank() ? failure.getClass().getSimpleName() : text;
    }

    /** Result of reading the optional configuration file. */
    public record LoadResult(RoomControlConfig config, String error, boolean present) {
        public LoadResult {
            error = error == null ? "" : error;
        }

        public boolean ok() {
            return config != null;
        }
    }

    /** Invalid configuration syntax, type, field or range. */
    public static final class ConfigException extends Exception {
        public ConfigException(String message) {
            super(message);
        }
    }
}
