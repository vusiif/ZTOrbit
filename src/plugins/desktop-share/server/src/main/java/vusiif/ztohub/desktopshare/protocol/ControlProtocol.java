package vusiif.ztohub.desktopshare.protocol;

import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.StreamReadConstraints;
import com.fasterxml.jackson.core.StreamReadFeature;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;

/**
 * Control-plane contract shared with the C++ client. Every request, response and
 * event is one compact UTF-8 JSON object inside one ZTS1 TCP frame. Encoding is
 * canonical and byte-identical to the C++ implementation in
 * {@code src/plugins/desktop-share/shared}.
 */
public final class ControlProtocol {
    public static final int VERSION = 1;
    public static final int MAX_CONTROL_BYTES = 64 * 1024;
    public static final int MAX_INITIALIZATION_BYTES = 8 * 1024;
    public static final int DEFAULT_PORT = 18100;
    public static final int MAX_VIEWERS_PER_ROOM = 8;
    public static final int MAX_ROOMS = 8;
    public static final int MAX_DESKTOP_SESSIONS = 72;
    public static final long MAX_REQUEST_ID = 0xFFFF_FFFFL;

    private static final HexFormat HEX = HexFormat.of();

    private static final ObjectMapper MAPPER;

    static {
        JsonFactory factory = JsonFactory.builder()
                .enable(StreamReadFeature.STRICT_DUPLICATE_DETECTION)
                .build();
        factory.setStreamReadConstraints(StreamReadConstraints.builder()
                .maxNestingDepth(64)
                .maxStringLength(MAX_CONTROL_BYTES)
                .build());
        MAPPER = new ObjectMapper(factory);
        MAPPER.enable(DeserializationFeature.FAIL_ON_TRAILING_TOKENS);
    }

    private ControlProtocol() {}

    public enum ErrorCode {
        NONE(""),
        PROTOCOL_VERSION("PROTOCOL_VERSION"),
        INVALID_REQUEST("INVALID_REQUEST"),
        UNKNOWN_OPERATION("UNKNOWN_OPERATION"),
        NOT_READY("NOT_READY"),
        ALREADY_IN_ROOM("ALREADY_IN_ROOM"),
        ROOM_NOT_FOUND("ROOM_NOT_FOUND"),
        ROOM_FULL("ROOM_FULL"),
        CAPACITY("CAPACITY"),
        FORBIDDEN("FORBIDDEN"),
        STALE_STREAM("STALE_STREAM"),
        RATE_LIMITED("RATE_LIMITED"),
        UDP_UNAVAILABLE("UDP_UNAVAILABLE"),
        MEDIA_UNAVAILABLE("MEDIA_UNAVAILABLE"),
        TIMEOUT("TIMEOUT"),
        INTERNAL("INTERNAL");

        private final String wire;

        ErrorCode(String wire) {
            this.wire = wire;
        }

        public String wireName() {
            return wire;
        }

        public static ErrorCode fromWire(String name) {
            for (ErrorCode value : values()) {
                if (!value.wire.isEmpty() && value.wire.equals(name)) return value;
            }
            return null;
        }
    }

    public enum Operation {
        HELLO("hello"),
        CREATE("create"),
        JOIN("join"),
        LEAVE("leave"),
        STOP("stop"),
        PING("ping"),
        STREAM_CONFIGURE("stream.configure"),
        STREAM_READY("stream.ready"),
        STREAM_KEYFRAME("stream.keyframe"),
        STREAM_FEEDBACK("stream.feedback");

        private final String wire;

        Operation(String wire) {
            this.wire = wire;
        }

        public String wireName() {
            return wire;
        }

        static Operation fromWire(String name) {
            for (Operation value : values()) {
                if (value.wire.equals(name)) return value;
            }
            return null;
        }
    }

    public enum Role {
        PUBLISHER("publisher"),
        VIEWER("viewer");

        private final String wire;

        Role(String wire) {
            this.wire = wire;
        }

        public String wireName() {
            return wire;
        }

        static Role fromWire(String name) {
            for (Role value : values()) {
                if (value.wire.equals(name)) return value;
            }
            return null;
        }
    }

    public enum RoomState {
        PREPARING("preparing"),
        RUNNING("running"),
        CLOSED("closed");

        private final String wire;

        RoomState(String wire) {
            this.wire = wire;
        }

        public String wireName() {
            return wire;
        }

        static RoomState fromWire(String name) {
            for (RoomState value : values()) {
                if (value.wire.equals(name)) return value;
            }
            return null;
        }
    }

    public enum CloseReason {
        OWNER_STOPPED("owner_stopped"),
        OWNER_DISCONNECTED("owner_disconnected"),
        OWNER_LOST("owner_lost"),
        STREAM_FAILED("stream_failed"),
        CONFIG_TIMEOUT("config_timeout"),
        SERVER_SHUTDOWN("server_shutdown");

        private final String wire;

        CloseReason(String wire) {
            this.wire = wire;
        }

        public String wireName() {
            return wire;
        }

        static CloseReason fromWire(String name) {
            for (CloseReason value : values()) {
                if (value.wire.equals(name)) return value;
            }
            return null;
        }
    }

    public enum EventKind {
        ROOM_STATE("room.state"),
        STREAM_CONFIG("stream.config"),
        STREAM_KEYFRAME("stream.keyframe"),
        ROOM_CLOSED("room.closed");

        private final String wire;

        EventKind(String wire) {
            this.wire = wire;
        }

        public String wireName() {
            return wire;
        }

        static EventKind fromWire(String name) {
            for (EventKind value : values()) {
                if (value.wire.equals(name)) return value;
            }
            return null;
        }
    }

    public record ProfileInfo(String name, int width, int height, int fps, int bitrate) {}

    private static final List<ProfileInfo> PROFILES = List.of(
            new ProfileInfo("720p30", 1280, 720, 30, 2_000_000),
            new ProfileInfo("1080p30", 1920, 1080, 30, 4_000_000));

    public static ProfileInfo profile(String name) {
        for (ProfileInfo info : PROFILES) {
            if (info.name.equals(name)) return info;
        }
        return null;
    }

    public static ProfileInfo profileForDimensions(int width, int height, int fps, int bitrate) {
        for (ProfileInfo info : PROFILES) {
            if (info.width == width && info.height == height && info.fps == fps
                    && info.bitrate == bitrate) return info;
        }
        return null;
    }

    public record StreamFormat(int width, int height, int fps, int bitrate,
                               byte[] initializationData, long streamEpoch, String codec) {
        public String initializationHex() {
            return HEX.formatHex(initializationData == null ? new byte[0] : initializationData);
        }

        public StreamFormat withStreamEpoch(long epoch) {
            return new StreamFormat(width, height, fps, bitrate, initializationData, epoch, codec);
        }
    }

    public record Feedback(long completePackets, long missingFragments, long decodedFrames,
                           long displayedFrames, boolean waiting, long receivedBytes) {}

    public record Request(long id, Operation op, String profile, String roomCode, long roomId,
                          StreamFormat format, long streamEpoch, Feedback feedback) {
        public static Request hello(long id) {
            return new Request(id, Operation.HELLO, null, null, 0, null, 0, null);
        }

        public static Request ping(long id) {
            return new Request(id, Operation.PING, null, null, 0, null, 0, null);
        }

        public static Request create(long id, String profile) {
            return new Request(id, Operation.CREATE, profile, null, 0, null, 0, null);
        }

        public static Request join(long id, String roomCode) {
            return new Request(id, Operation.JOIN, null, roomCode, 0, null, 0, null);
        }

        public static Request leave(long id, long roomId) {
            return new Request(id, Operation.LEAVE, null, null, roomId, null, 0, null);
        }

        public static Request stop(long id, long roomId) {
            return new Request(id, Operation.STOP, null, null, roomId, null, 0, null);
        }

        public static Request configure(long id, long roomId, StreamFormat format) {
            return new Request(id, Operation.STREAM_CONFIGURE, null, null, roomId, format, 0, null);
        }

        public static Request ready(long id, long roomId, long streamEpoch) {
            return new Request(id, Operation.STREAM_READY, null, null, roomId, null, streamEpoch,
                    null);
        }

        public static Request keyframe(long id, long roomId, long streamEpoch) {
            return new Request(id, Operation.STREAM_KEYFRAME, null, null, roomId, null, streamEpoch,
                    null);
        }

        public static Request feedback(long id, long roomId, long streamEpoch, Feedback feedback) {
            return new Request(id, Operation.STREAM_FEEDBACK, null, null, roomId, null, streamEpoch,
                    feedback);
        }
    }

    public record HelloInfo(long sessionId, int udpPort, List<String> profiles, int maxViewers) {}

    public record RoomInfo(long roomId, String roomCode, String profile, Role role, byte[] token,
                           StreamFormat format) {
        public String tokenHex() {
            return token == null ? "" : HEX.formatHex(token);
        }
    }

    public record Response(long id, boolean ok, Operation op, HelloInfo hello, RoomInfo room,
                           long roomId, long streamEpoch, ErrorCode error, String message) {
        public static Response success(long id, Operation op) {
            return new Response(id, true, op, null, null, 0, 0, ErrorCode.NONE, null);
        }

        public static Response hello(long id, HelloInfo info) {
            return new Response(id, true, Operation.HELLO, info, null, 0, 0, ErrorCode.NONE, null);
        }

        public static Response room(long id, Operation op, RoomInfo room) {
            return new Response(id, true, op, null, room, 0, 0, ErrorCode.NONE, null);
        }

        public static Response stream(long id, Operation op, long roomId, long streamEpoch) {
            return new Response(id, true, op, null, null, roomId, streamEpoch, ErrorCode.NONE,
                    null);
        }

        public static Response failure(long id, Operation op, ErrorCode code, String message) {
            return new Response(id, false, op, null, null, 0, 0, code, message);
        }
    }

    public record Event(EventKind kind, long roomId, RoomState roomState, int viewerCount,
                        StreamFormat format, long streamEpoch, CloseReason reason) {
        public static Event roomState(long roomId, RoomState state, int viewerCount) {
            return new Event(EventKind.ROOM_STATE, roomId, state, viewerCount, null, 0, null);
        }

        public static Event streamConfig(long roomId, StreamFormat format) {
            return new Event(EventKind.STREAM_CONFIG, roomId, null, 0, format, 0, null);
        }

        public static Event streamKeyframe(long roomId, long streamEpoch) {
            return new Event(EventKind.STREAM_KEYFRAME, roomId, null, 0, null, streamEpoch, null);
        }

        public static Event roomClosed(long roomId, CloseReason reason) {
            return new Event(EventKind.ROOM_CLOSED, roomId, null, 0, null, 0, reason);
        }
    }

    public static final class ProtocolException extends Exception {
        private final ErrorCode code;

        public ProtocolException(ErrorCode code, String message) {
            super(message);
            this.code = code;
        }

        public ErrorCode code() {
            return code;
        }
    }

    private static ProtocolException invalid(String message) {
        return new ProtocolException(ErrorCode.INVALID_REQUEST, message);
    }

    private static ProtocolException version(String message) {
        return new ProtocolException(ErrorCode.PROTOCOL_VERSION, message);
    }

    private static ProtocolException unknownOperation(String message) {
        return new ProtocolException(ErrorCode.UNKNOWN_OPERATION, message);
    }

    private enum Kind { REQUEST, RESPONSE, EVENT }

    private static Kind kind(JsonNode root) throws ProtocolException {
        boolean hasId = root.has("id");
        boolean hasOp = root.has("op");
        boolean hasEvent = root.has("event");
        boolean hasOk = root.has("ok");
        boolean hasError = root.has("error");
        boolean request = hasOp && !hasEvent && !hasOk && !hasError;
        boolean event = hasEvent && !hasOp && !hasOk && !hasError && !hasId;
        boolean response = hasOk && !hasOp && !hasEvent;
        if ((request ? 1 : 0) + (event ? 1 : 0) + (response ? 1 : 0) != 1) {
            throw invalid("控制消息必须且只能是一种请求、响应或事件");
        }
        return request ? Kind.REQUEST : event ? Kind.EVENT : Kind.RESPONSE;
    }

    private static JsonNode envelope(byte[] bytes) throws ProtocolException {
        if (bytes == null || bytes.length == 0) throw invalid("控制消息为空");
        if (bytes.length > MAX_CONTROL_BYTES) {
            throw invalid("控制消息超过 " + MAX_CONTROL_BYTES + " 字节上限");
        }
        JsonNode root;
        try {
            root = MAPPER.readTree(bytes);
        } catch (Exception error) {
            throw invalid("JSON 解析失败：" + error.getMessage());
        }
        if (root == null || !root.isObject()) throw invalid("控制消息必须是 JSON 对象");
        JsonNode version = root.get("v");
        if (version == null || !version.isIntegralNumber()) throw version("缺少整数协议版本 v");
        if (version.longValue() != VERSION) throw version("不支持的协议版本 " + version.longValue());
        return root;
    }

    private static long requireId(JsonNode root) throws ProtocolException {
        JsonNode id = root.get("id");
        if (id == null || !id.isIntegralNumber() || !id.canConvertToLong()) {
            throw invalid("请求 ID 必须是 1..4294967295 的整数");
        }
        long value = id.longValue();
        if (value <= 0 || value > MAX_REQUEST_ID) {
            throw invalid("请求 ID 必须是 1..4294967295 的整数");
        }
        return value;
    }

    private static String requireString(JsonNode object, String key) throws ProtocolException {
        JsonNode node = object.get(key);
        if (node == null || !node.isTextual()) throw invalid("缺少字符串字段 " + key);
        String value = node.textValue();
        if (hasUnpairedSurrogate(value)) throw invalid("字段 " + key + " 含无效 UTF-16 代理项");
        return value;
    }

    private static boolean hasUnpairedSurrogate(String value) {
        for (int i = 0; i < value.length(); ++i) {
            char c = value.charAt(i);
            if (Character.isHighSurrogate(c)) {
                if (i + 1 >= value.length() || !Character.isLowSurrogate(value.charAt(i + 1)))
                    return true;
                ++i;
            } else if (Character.isLowSurrogate(c)) {
                return true;
            }
        }
        return false;
    }

    private static boolean requireBool(JsonNode object, String key) throws ProtocolException {
        JsonNode node = object.get(key);
        if (node == null || !node.isBoolean()) throw invalid("字段 " + key + " 必须是布尔值");
        return node.booleanValue();
    }

    private static int requireInt(JsonNode object, String key, int minimum, int maximum)
            throws ProtocolException {
        JsonNode node = object.get(key);
        if (node == null || !node.isIntegralNumber() || !node.canConvertToInt())
            throw invalid("字段 " + key + " 必须是 " + minimum + ".." + maximum + " 的整数");
        int value = node.intValue();
        if (value < minimum || value > maximum)
            throw invalid("字段 " + key + " 必须是 " + minimum + ".." + maximum + " 的整数");
        return value;
    }

    private static JsonNode requireObject(JsonNode object, String key) throws ProtocolException {
        JsonNode node = object.get(key);
        if (node == null || !node.isObject()) throw invalid("字段 " + key + " 必须是对象");
        return node;
    }

    private static long requireU64(JsonNode object, String key, boolean allowZero)
            throws ProtocolException {
        long value = parseU64(requireString(object, key));
        if (!allowZero && value == 0)
            throw invalid("字段 " + key + " 必须是无前导零的十进制无符号整数");
        return value;
    }

    private static void requireEmptyObject(JsonNode data) throws ProtocolException {
        if (data.size() != 0) throw invalid("该操作不接受 data 字段");
    }

    private static byte[] requireHex(JsonNode object, String key) throws ProtocolException {
        String text = requireString(object, key);
        if (!isValidInitializationData(text))
            throw invalid("字段 " + key + " 必须是偶数长度、不超过 8192 字节的十六进制字符串");
        return text.isEmpty() ? new byte[0] : HEX.parseHex(text);
    }

    private static StreamFormat readFormat(JsonNode object, boolean withEpoch)
            throws ProtocolException {
        String codec = requireString(object, "codec");
        if (!"h264".equals(codec) && !"bgra-tiles".equals(codec)) throw invalid("codec 只接受 h264 或 bgra-tiles");
        int width = requireInt(object, "width", 2, 8192);
        int height = requireInt(object, "height", 2, 8192);
        int fps = requireInt(object, "fps", 1, 120);
        int bitrate = requireInt(object, "bitrate", 1, 100_000_000);
        byte[] initialization = requireHex(object, "initializationData");
        long epoch = withEpoch ? requireU64(object, "streamEpoch", false) : 0;
        if (profileForDimensions(width, height, fps, bitrate) == null)
            throw invalid("格式尺寸、帧率与码率必须匹配已定义档位");
        if ("bgra-tiles".equals(codec) && initialization.length != 0) throw invalid("bgra-tiles 初始化数据必须为空");
        return new StreamFormat(width, height, fps, bitrate, initialization, epoch, codec);
    }

    private static HelloInfo readHello(JsonNode data) throws ProtocolException {
        long sessionId = requireU64(data, "sessionId", false);
        int udpPort = requireInt(data, "udpPort", 1, 65_535);
        JsonNode capabilities = requireObject(data, "capabilities");
        JsonNode profiles = capabilities.get("profiles");
        if (profiles == null || !profiles.isArray() || profiles.size() == 0 || profiles.size() > 2)
            throw invalid("capabilities.profiles 必须包含 1~2 项");
        List<String> names = new ArrayList<>();
        for (JsonNode entry : profiles) {
            if (!entry.isTextual() || profile(entry.textValue()) == null)
                throw invalid("capabilities.profiles 含未知档位");
            if (names.contains(entry.textValue()))
                throw invalid("capabilities.profiles 含重复档位");
            names.add(entry.textValue());
        }
        int maxViewers = requireInt(capabilities, "maxViewers", 1, MAX_VIEWERS_PER_ROOM);
        return new HelloInfo(sessionId, udpPort, names, maxViewers);
    }

    private static RoomInfo readRoom(JsonNode data, Role expectedRole) throws ProtocolException {
        long roomId = requireU64(data, "roomId", false);
        String roomCode = requireString(data, "roomCode");
        if (!isValidRoomCode(roomCode)) throw invalid("roomCode 必须是八位十进制字符串");
        String profileName = requireString(data, "profile");
        ProfileInfo profile = profile(profileName);
        if (profile == null) throw invalid("profile 不是已定义档位");
        String roleText = requireString(data, "role");
        Role role = Role.fromWire(roleText);
        if (role != expectedRole) throw invalid("响应角色与请求不匹配");
        String tokenText = requireString(data, "token");
        byte[] token = tokenFromHex(tokenText);
        if (token == null) throw invalid("token 必须是 32 字符十六进制字符串");
        StreamFormat format = null;
        JsonNode formatNode = data.get("format");
        if (formatNode != null) {
            if (expectedRole != Role.VIEWER) throw invalid("create 响应不能包含 format");
            if (!formatNode.isObject()) throw invalid("format 必须是对象");
            format = readFormat(formatNode, true);
            if (format.width() != profile.width || format.height() != profile.height
                    || format.fps() != profile.fps || format.bitrate() != profile.bitrate)
                throw invalid("format 与房间档位不一致");
        }
        return new RoomInfo(roomId, roomCode, profileName, role, token, format);
    }

    public static byte[] encodeRequest(Request request) {
        if (request == null || request.id() <= 0 || request.id() > MAX_REQUEST_ID
                || request.op() == null) return new byte[0];
        Json json = new Json();
        json.beginObject();
        json.key("v");
        json.number(VERSION);
        json.key("id");
        json.number(request.id());
        json.key("op");
        json.string(request.op().wireName());
        json.key("data");
        json.beginObject();
        switch (request.op()) {
            case HELLO, PING -> {
            }
            case CREATE -> {
                if (profile(request.profile()) == null) return new byte[0];
                json.key("profile");
                json.string(request.profile());
            }
            case JOIN -> {
                if (!isValidRoomCode(request.roomCode())) return new byte[0];
                json.key("roomCode");
                json.string(request.roomCode());
            }
            case LEAVE, STOP -> {
                if (request.roomId() == 0) return new byte[0];
                writeU64(json, "roomId", request.roomId());
            }
            case STREAM_CONFIGURE -> {
                StreamFormat format = request.format();
                if (request.roomId() == 0 || format == null || !validFormatDimensions(format))
                    return new byte[0];
                writeU64(json, "roomId", request.roomId());
                json.key("codec");
                json.string(format.codec());
                json.key("width");
                json.number(format.width());
                json.key("height");
                json.number(format.height());
                json.key("fps");
                json.number(format.fps());
                json.key("bitrate");
                json.number(format.bitrate());
                json.key("initializationData");
                json.string(HEX.formatHex(format.initializationData() == null
                        ? new byte[0] : format.initializationData()));
            }
            case STREAM_READY, STREAM_KEYFRAME -> {
                if (request.roomId() == 0 || request.streamEpoch() == 0) return new byte[0];
                writeU64(json, "roomId", request.roomId());
                writeU64(json, "streamEpoch", request.streamEpoch());
            }
            case STREAM_FEEDBACK -> {
                if (request.roomId() == 0 || request.streamEpoch() == 0
                        || request.feedback() == null) return new byte[0];
                writeU64(json, "roomId", request.roomId());
                writeU64(json, "streamEpoch", request.streamEpoch());
                Feedback feedback = request.feedback();
                writeU64(json, "completePackets", feedback.completePackets());
                writeU64(json, "missingFragments", feedback.missingFragments());
                writeU64(json, "decodedFrames", feedback.decodedFrames());
                writeU64(json, "displayedFrames", feedback.displayedFrames());
                json.key("waiting");
                json.bool(feedback.waiting());
                writeU64(json, "receivedBytes", feedback.receivedBytes());
            }
        }
        json.endObject();
        json.endObject();
        return json.take();
    }

    public static Request decodeRequest(byte[] bytes) throws ProtocolException {
        JsonNode root = envelope(bytes);
        if (kind(root) != Kind.REQUEST) throw invalid("消息不是请求");
        long id = requireId(root);
        String opName = requireString(root, "op");
        Operation op = Operation.fromWire(opName);
        if (op == null) throw unknownOperation("未知操作：" + opName);
        JsonNode data = requireObject(root, "data");
        String profileName = null;
        String roomCode = null;
        long roomId = 0;
        StreamFormat format = null;
        long streamEpoch = 0;
        Feedback feedback = null;
        switch (op) {
            case HELLO, PING -> requireEmptyObject(data);
            case CREATE -> {
                profileName = requireString(data, "profile");
                if (profile(profileName) == null) throw invalid("profile 不是已定义档位");
            }
            case JOIN -> {
                roomCode = requireString(data, "roomCode");
                if (!isValidRoomCode(roomCode)) throw invalid("roomCode 必须是八位十进制字符串");
            }
            case LEAVE, STOP -> roomId = requireU64(data, "roomId", false);
            case STREAM_CONFIGURE -> {
                roomId = requireU64(data, "roomId", false);
                String codec = requireString(data, "codec");
                if (!"h264".equals(codec) && !"bgra-tiles".equals(codec)) throw invalid("codec 只接受 h264 或 bgra-tiles");
                int width = requireInt(data, "width", 2, 8192);
                int height = requireInt(data, "height", 2, 8192);
                int fps = requireInt(data, "fps", 1, 120);
                int bitrate = requireInt(data, "bitrate", 1, 100_000_000);
                byte[] initialization = requireHex(data, "initializationData");
                if (profileForDimensions(width, height, fps, bitrate) == null)
                    throw invalid("格式尺寸、帧率与码率必须匹配已定义档位");
                if ("bgra-tiles".equals(codec) && initialization.length != 0) throw invalid("bgra-tiles 初始化数据必须为空");
                format = new StreamFormat(width, height, fps, bitrate, initialization, 0, codec);
            }
            case STREAM_READY, STREAM_KEYFRAME -> {
                roomId = requireU64(data, "roomId", false);
                streamEpoch = requireU64(data, "streamEpoch", false);
            }
            case STREAM_FEEDBACK -> {
                roomId = requireU64(data, "roomId", false);
                streamEpoch = requireU64(data, "streamEpoch", false);
                feedback = new Feedback(
                        requireU64(data, "completePackets", true),
                        requireU64(data, "missingFragments", true),
                        requireU64(data, "decodedFrames", true),
                        requireU64(data, "displayedFrames", true),
                        requireBool(data, "waiting"),
                        requireU64(data, "receivedBytes", true));
            }
        }
        return new Request(id, op, profileName, roomCode, roomId, format, streamEpoch, feedback);
    }

    public static byte[] encodeResponse(Response response) {
        if (response == null || response.id() <= 0 || response.id() > MAX_REQUEST_ID
                || response.op() == null) return new byte[0];
        Json json = new Json();
        json.beginObject();
        json.key("v");
        json.number(VERSION);
        json.key("id");
        json.number(response.id());
        json.key("ok");
        json.bool(response.ok());
        if (!response.ok()) {
            if (response.error() == null || response.error() == ErrorCode.NONE) return new byte[0];
            json.key("error");
            json.beginObject();
            json.key("code");
            json.string(response.error().wireName());
            json.key("message");
            json.string(response.message() == null ? "" : response.message());
            json.endObject();
            json.endObject();
            return json.take();
        }
        json.key("data");
        json.beginObject();
        switch (response.op()) {
            case HELLO -> {
                HelloInfo hello = response.hello();
                if (!validHello(hello)) return new byte[0];
                writeU64(json, "sessionId", hello.sessionId());
                json.key("udpPort");
                json.number(hello.udpPort());
                json.key("capabilities");
                json.beginObject();
                json.key("profiles");
                json.beginArray();
                for (ProfileInfo info : PROFILES) {
                    if (hello.profiles().contains(info.name)) json.string(info.name);
                }
                json.endArray();
                json.key("maxViewers");
                json.number(hello.maxViewers());
                json.endObject();
            }
            case CREATE, JOIN -> {
                RoomInfo room = response.room();
                Role expected = response.op() == Operation.CREATE ? Role.PUBLISHER : Role.VIEWER;
                if (room == null || room.role() != expected || !validRoom(room)) return new byte[0];
                writeU64(json, "roomId", room.roomId());
                json.key("roomCode");
                json.string(room.roomCode());
                json.key("profile");
                json.string(room.profile());
                json.key("role");
                json.string(room.role().wireName());
                json.key("token");
                json.string(HEX.formatHex(room.token()));
                if (room.format() != null) {
                    json.key("format");
                    writeFormat(json, room.format());
                }
            }
            case LEAVE, STOP, PING, STREAM_FEEDBACK -> {
            }
            case STREAM_CONFIGURE, STREAM_READY, STREAM_KEYFRAME -> {
                if (response.roomId() == 0 || response.streamEpoch() == 0) return new byte[0];
                writeU64(json, "roomId", response.roomId());
                writeU64(json, "streamEpoch", response.streamEpoch());
            }
        }
        json.endObject();
        json.endObject();
        return json.take();
    }

    /**
     * Returns the request ID of a payload that looks like a request (has an integral
     * {@code id} and a textual {@code op}, and is not a response or event), or zero when
     * no safe error reply is possible. Used after {@link #decodeRequest} rejects a frame
     * so the server can still answer malformed or unknown operations.
     */
    public static long peekRequestId(byte[] bytes) {
        if (bytes == null || bytes.length == 0 || bytes.length > MAX_CONTROL_BYTES) {
            return 0;
        }
        try {
            JsonNode root = MAPPER.readTree(bytes);
            if (root == null || !root.isObject()) return 0;
            if (!root.has("op") || root.has("event") || root.has("ok")) return 0;
            JsonNode op = root.get("op");
            if (op == null || !op.isTextual()) return 0;
            JsonNode id = root.get("id");
            if (id == null || !id.isIntegralNumber() || !id.canConvertToLong()) return 0;
            long value = id.longValue();
            return value > 0 && value <= MAX_REQUEST_ID ? value : 0;
        } catch (Exception ignored) {
            return 0;
        }
    }

    /**
     * Encodes an error response without an operation. Request frames that failed to
     * decode have no valid {@link Operation}, but the wire shape is identical to a
     * normal failure response.
     */
    public static byte[] encodeError(long id, ErrorCode code, String message) {
        if (id <= 0 || id > MAX_REQUEST_ID || code == null || code == ErrorCode.NONE) {
            return new byte[0];
        }
        Json json = new Json();
        json.beginObject();
        json.key("v");
        json.number(VERSION);
        json.key("id");
        json.number(id);
        json.key("ok");
        json.bool(false);
        json.key("error");
        json.beginObject();
        json.key("code");
        json.string(code.wireName());
        json.key("message");
        json.string(message == null ? "" : message);
        json.endObject();
        json.endObject();
        return json.take();
    }

    public static Response decodeResponse(byte[] bytes, Operation expected)
            throws ProtocolException {
        if (expected == null) throw invalid("响应类型缺失");
        JsonNode root = envelope(bytes);
        if (kind(root) != Kind.RESPONSE) throw invalid("消息不是响应");
        long id = requireId(root);
        JsonNode okNode = root.get("ok");
        if (okNode == null || !okNode.isBoolean()) throw invalid("ok 必须是布尔值");
        boolean ok = okNode.booleanValue();
        JsonNode data = root.get("data");
        JsonNode error = root.get("error");
        if (ok) {
            if (data == null) throw invalid("成功响应缺少 data");
            if (error != null) throw invalid("成功响应不能包含 error");
            if (!data.isObject()) throw invalid("响应 data 必须是对象");
            HelloInfo hello = null;
            RoomInfo room = null;
            long roomId = 0;
            long streamEpoch = 0;
            switch (expected) {
                case HELLO -> hello = readHello(data);
                case CREATE -> room = readRoom(data, Role.PUBLISHER);
                case JOIN -> room = readRoom(data, Role.VIEWER);
                case LEAVE, STOP, PING, STREAM_FEEDBACK -> requireEmptyObject(data);
                case STREAM_CONFIGURE, STREAM_READY, STREAM_KEYFRAME -> {
                    roomId = requireU64(data, "roomId", false);
                    streamEpoch = requireU64(data, "streamEpoch", false);
                }
            }
            return new Response(id, true, expected, hello, room, roomId, streamEpoch,
                    ErrorCode.NONE, null);
        }
        if (data != null) throw invalid("错误响应不能包含 data");
        if (error == null || !error.isObject()) throw invalid("错误响应缺少 error 对象");
        String codeName = requireString(error, "code");
        ErrorCode code = ErrorCode.fromWire(codeName);
        if (code == null || code == ErrorCode.NONE) throw invalid("未知错误码：" + codeName);
        String message = requireString(error, "message");
        return new Response(id, false, expected, null, null, 0, 0, code, message);
    }

    public static byte[] encodeEvent(Event event) {
        if (event == null || event.kind() == null || event.roomId() == 0) return new byte[0];
        Json json = new Json();
        json.beginObject();
        json.key("v");
        json.number(VERSION);
        json.key("event");
        json.string(event.kind().wireName());
        json.key("data");
        json.beginObject();
        writeU64(json, "roomId", event.roomId());
        switch (event.kind()) {
            case ROOM_STATE -> {
                if (event.roomState() == null || event.viewerCount() < 0
                        || event.viewerCount() > MAX_VIEWERS_PER_ROOM) return new byte[0];
                json.key("state");
                json.string(event.roomState().wireName());
                json.key("viewerCount");
                json.number(event.viewerCount());
            }
            case STREAM_CONFIG -> {
                StreamFormat format = event.format();
                if (format == null || !validFormatDimensions(format) || format.streamEpoch() == 0)
                    return new byte[0];
                json.key("format");
                writeFormat(json, format);
            }
            case STREAM_KEYFRAME -> {
                if (event.streamEpoch() == 0) return new byte[0];
                writeU64(json, "streamEpoch", event.streamEpoch());
            }
            case ROOM_CLOSED -> {
                if (event.reason() == null) return new byte[0];
                json.key("reason");
                json.string(event.reason().wireName());
            }
        }
        json.endObject();
        json.endObject();
        return json.take();
    }

    public static Event decodeEvent(byte[] bytes) throws ProtocolException {
        JsonNode root = envelope(bytes);
        if (kind(root) != Kind.EVENT) throw invalid("消息不是事件");
        String name = requireString(root, "event");
        EventKind kindName = EventKind.fromWire(name);
        if (kindName == null) throw invalid("未知事件：" + name);
        JsonNode data = requireObject(root, "data");
        long roomId = requireU64(data, "roomId", false);
        RoomState state = null;
        int viewerCount = 0;
        StreamFormat format = null;
        long streamEpoch = 0;
        CloseReason reason = null;
        switch (kindName) {
            case ROOM_STATE -> {
                String stateName = requireString(data, "state");
                state = RoomState.fromWire(stateName);
                if (state == null) throw invalid("未知房间状态");
                viewerCount = requireInt(data, "viewerCount", 0, MAX_VIEWERS_PER_ROOM);
            }
            case STREAM_CONFIG -> format = readFormat(requireObject(data, "format"), true);
            case STREAM_KEYFRAME -> streamEpoch = requireU64(data, "streamEpoch", false);
            case ROOM_CLOSED -> {
                String reasonName = requireString(data, "reason");
                reason = CloseReason.fromWire(reasonName);
                if (reason == null) throw invalid("未知关闭原因");
            }
        }
        return new Event(kindName, roomId, state, viewerCount, format, streamEpoch, reason);
    }

    private static void writeU64(Json json, String key, long value) {
        json.key(key);
        json.string(Long.toUnsignedString(value));
    }

    private static void writeFormat(Json json, StreamFormat format) {
        json.beginObject();
        json.key("codec");
        json.string(format.codec());
        json.key("width");
        json.number(format.width());
        json.key("height");
        json.number(format.height());
        json.key("fps");
        json.number(format.fps());
        json.key("bitrate");
        json.number(format.bitrate());
        json.key("initializationData");
        json.string(HEX.formatHex(format.initializationData() == null
                ? new byte[0] : format.initializationData()));
        json.key("streamEpoch");
        json.string(Long.toUnsignedString(format.streamEpoch()));
        json.endObject();
    }

    private static boolean validFormatDimensions(StreamFormat format) {
        return format != null && format.initializationData() != null
                && ("h264".equals(format.codec()) || ("bgra-tiles".equals(format.codec()) && format.initializationData().length == 0))
                && format.initializationData().length <= MAX_INITIALIZATION_BYTES
                && profileForDimensions(format.width(), format.height(), format.fps(),
                        format.bitrate()) != null;
    }

    private static boolean validHello(HelloInfo hello) {
        if (hello == null || hello.sessionId() == 0 || hello.udpPort() == 0
                || hello.maxViewers() < 1 || hello.maxViewers() > MAX_VIEWERS_PER_ROOM
                || hello.profiles() == null || hello.profiles().isEmpty()
                || hello.profiles().size() > 2) return false;
        for (String name : hello.profiles()) {
            if (profile(name) == null) return false;
        }
        return hello.profiles().stream().distinct().count() == hello.profiles().size();
    }

    private static boolean validRoom(RoomInfo room) {
        if (room.roomId() == 0 || !isValidRoomCode(room.roomCode()) || profile(room.profile()) == null
                || room.token() == null || room.token().length != 16) return false;
        if (room.format() != null) {
            if (room.role() != Role.VIEWER) return false;
            if (!validFormatDimensions(room.format()) || room.format().streamEpoch() == 0)
                return false;
            ProfileInfo profile = profile(room.profile());
            if (profile.width != room.format().width() || profile.height != room.format().height()
                    || profile.fps != room.format().fps
                    || profile.bitrate != room.format().bitrate) return false;
        }
        return true;
    }

    public static boolean isValidRoomCode(String code) {
        if (code == null || code.length() != 8) return false;
        for (int i = 0; i < code.length(); ++i) {
            char c = code.charAt(i);
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    public static boolean isValidTokenHex(String hex) {
        if (hex == null || hex.length() != 32) return false;
        for (int i = 0; i < hex.length(); ++i) {
            char c = hex.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                    || (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }

    public static byte[] tokenFromHex(String hex) {
        if (!isValidTokenHex(hex)) return null;
        return HEX.parseHex(hex);
    }

    public static String tokenToHex(byte[] raw) {
        if (raw == null || raw.length != 16) return "";
        return HEX.formatHex(raw);
    }

    public static boolean isValidInitializationData(String hex) {
        if (hex == null || hex.length() > MAX_INITIALIZATION_BYTES * 2) return false;
        if ((hex.length() & 1) != 0) return false;
        for (int i = 0; i < hex.length(); ++i) {
            char c = hex.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                    || (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }

    public static boolean isValidCounter(String text) {
        try {
            parseU64(text);
            return true;
        } catch (ProtocolException error) {
            return false;
        }
    }

    private static long parseU64(String text) throws ProtocolException {
        if (text == null || text.isEmpty() || text.length() > 20)
            throw invalid("无符号整数格式无效");
        if (text.equals("0")) return 0;
        char first = text.charAt(0);
        if (first < '1' || first > '9') throw invalid("无符号整数格式无效");
        for (int i = 1; i < text.length(); ++i) {
            char c = text.charAt(i);
            if (c < '0' || c > '9') throw invalid("无符号整数格式无效");
        }
        try {
            return Long.parseUnsignedLong(text);
        } catch (NumberFormatException error) {
            throw invalid("无符号整数超出 64 位范围");
        }
    }

    static byte[] utf8(String text) {
        return text.getBytes(StandardCharsets.UTF_8);
    }
}
