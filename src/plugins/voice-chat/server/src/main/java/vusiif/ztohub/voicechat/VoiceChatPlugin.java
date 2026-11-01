package vusiif.ztohub.voicechat;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.regex.Pattern;
import vusiif.ztohub.api.CancellationToken;
import vusiif.ztohub.api.HostServices;
import vusiif.ztohub.api.LogLevel;
import vusiif.ztohub.api.PluginInfo;
import vusiif.ztohub.api.ServerPlugin;
import vusiif.ztohub.api.StartTask;

/**
 * Voice chat room plugin. Ports the archived C++ plugin behaviour to the Java host API; the client
 * protocol (control JSON over ZTS1 TCP plus ZTV1 UDP) is unchanged.
 *
 * <p>All callbacks arrive on the host event thread, so the session and room maps need no
 * synchronization. Settings are read once at startup from {@code settings.json} in the plugin
 * data directory.
 */
public final class VoiceChatPlugin implements ServerPlugin {
    private static final ObjectMapper JSON = JsonMapper.builder()
            .configure(DeserializationFeature.FAIL_ON_TRAILING_TOKENS, true)
            .build();
    private static final Pattern IDENTITY = Pattern.compile("^[0-9a-f]{10}$");
    private static final long MAX_UNSIGNED_INT = 0xFFFF_FFFFL;
    private static final int MAX_SEND_BACKLOG = 256 * 1024;
    private static final long SESSION_TIMEOUT_MS = 15_000;

    private final Map<Long, Session> sessions = new LinkedHashMap<>();
    private final Map<Long, String> rooms = new LinkedHashMap<>();
    private final SecureRandom random = new SecureRandom();
    private final PluginInfo info = new PluginInfo("voice-chat", "语音聊天", "0.1.0");
    private HostServices services;
    private ScheduledFuture<?> cleanup;
    private long nextSession = 1;
    private long nextRoom = 1;
    private int maxRooms = 32;
    private int maxMembers = 16;
    private int maxConnections = 256;

    @Override
    public PluginInfo info() {
        return info;
    }

    @Override
    public StartTask onStart(HostServices host, CancellationToken cancellation) {
        this.services = host;
        loadSettings(host);
        cleanup = host.scheduler().scheduleAtFixedRate(
                this::expireSessions, 1, 1, TimeUnit.SECONDS);
        host.logs().write(LogLevel.INFO, "Voice plugin started");
        return null;
    }

    private void loadSettings(HostServices host) {
        var config = host.files().readFile("settings.json");
        if (!config.ok()) {
            return;
        }
        JsonNode settings;
        try {
            settings = JSON.readTree(config.data());
        } catch (Exception invalid) {
            throw new IllegalArgumentException("Invalid voice settings.json");
        }
        if (settings == null || !settings.isObject()) {
            throw new IllegalArgumentException("Invalid voice settings.json");
        }
        maxRooms = settings.path("maxRooms").asInt(32);
        maxMembers = settings.path("maxMembers").asInt(16);
        maxConnections = settings.path("maxConnections").asInt(256);
        if (maxRooms < 1 || maxRooms > 10_000
                || maxMembers < 1 || maxMembers > 256
                || maxConnections < 1 || maxConnections > 10_000) {
            throw new IllegalArgumentException("Invalid voice capacity");
        }
    }

    private void expireSessions() {
        if (services == null) {
            return;
        }
        long now = System.currentTimeMillis();
        for (Long connection : new ArrayList<>(sessions.keySet())) {
            Session session = sessions.get(connection);
            if (session != null && now - session.lastSeen > SESSION_TIMEOUT_MS) {
                services.disconnect(connection);
            }
        }
    }

    @Override
    public void onStop() {
        if (cleanup != null) {
            cleanup.cancel(false);
            cleanup = null;
        }
        sessions.clear();
        rooms.clear();
        if (services != null) {
            services.logs().write(LogLevel.INFO, "Voice plugin stopped");
        }
        services = null;
    }

    @Override
    public void onConnectionClosed(long connection) {
        Session session = sessions.remove(connection);
        if (session != null) {
            leave(session);
        }
    }

    @Override
    public void onTcpPacket(long connection, byte[] payload) {
        if (services == null) {
            return;
        }
        Session session = sessions.get(connection);
        if (session == null) {
            if (sessions.size() >= maxConnections || nextSession > MAX_UNSIGNED_INT) {
                services.disconnect(connection);
                return;
            }
            session = new Session();
            session.connection = connection;
            session.id = nextSession;
            nextSession = (nextSession + 1) & MAX_UNSIGNED_INT;
            if (nextSession == 0) {
                nextSession = MAX_UNSIGNED_INT + 1;
            }
            session.lastSeen = session.rateWindow = System.currentTimeMillis();
            sessions.put(connection, session);
        }
        if (payload.length > VoiceProtocol.MAX_CONTROL_BYTES || !allow(session, false)) {
            services.disconnect(connection);
            return;
        }
        JsonNode request;
        try {
            request = JSON.readTree(payload);
        } catch (Exception invalid) {
            services.disconnect(connection);
            return;
        }
        if (request == null || !request.isObject()) {
            services.disconnect(connection);
            return;
        }
        session.lastSeen = System.currentTimeMillis();
        command(session, request);
    }

    @Override
    public void onUdpPacket(InetSocketAddress endpoint, byte[] payload) {
        if (services == null || endpoint == null || endpoint.getAddress() == null) {
            return;
        }
        VoiceProtocol.Packet packet = VoiceProtocol.decode(payload);
        if (packet == null) {
            return;
        }
        Session sender = null;
        for (Session candidate : sessions.values()) {
            if (candidate.id == packet.sender() && candidate.room != 0
                    && candidate.token.length == 16
                    && Arrays.equals(candidate.token, packet.token())) {
                sender = candidate;
                break;
            }
        }
        if (sender == null || !allow(sender, true)
                || !endpoint.getAddress().equals(services.peerAddress(sender.connection))) {
            return;
        }
        if (packet.type() == VoiceProtocol.REGISTER) {
            if (sender.udpPort != 0
                    && (sender.udpPort != endpoint.getPort()
                    || !endpoint.getAddress().equals(sender.udpAddress))) {
                return;
            }
            sender.udpAddress = endpoint.getAddress();
            sender.udpPort = endpoint.getPort();
            VoiceProtocol.Packet registered = new VoiceProtocol.Packet(
                    VoiceProtocol.REGISTERED, packet.token(), packet.sender(), packet.sequence(), new byte[0]);
            services.sendUdp(new InetSocketAddress(sender.udpAddress, sender.udpPort),
                    VoiceProtocol.encode(registered));
        } else if (packet.type() == VoiceProtocol.AUDIO
                && !sender.muted
                && sender.udpPort == endpoint.getPort()
                && endpoint.getAddress().equals(sender.udpAddress)) {
            for (Session recipient : sessions.values()) {
                if (recipient.id == sender.id || recipient.room != sender.room
                        || recipient.udpPort == 0 || recipient.token.length != 16) {
                    continue;
                }
                VoiceProtocol.Packet audio = new VoiceProtocol.Packet(
                        VoiceProtocol.AUDIO, recipient.token, packet.sender(), packet.sequence(),
                        packet.payload());
                services.sendUdp(new InetSocketAddress(recipient.udpAddress, recipient.udpPort),
                        VoiceProtocol.encode(audio));
            }
        }
    }

    private boolean allow(Session session, boolean audio) {
        long now = System.currentTimeMillis();
        if (now - session.rateWindow >= 1000) {
            session.controls = 0;
            session.packets = 0;
            session.rateWindow = now;
        }
        if (audio) {
            session.packets++;
            return session.packets <= 100;
        }
        session.controls++;
        return session.controls <= 30;
    }

    private void command(Session session, JsonNode request) {
        ObjectNode response = JSON.createObjectNode();
        response.put("v", VoiceProtocol.VERSION);
        if (request.has("id")) {
            response.set("id", request.get("id"));
        }
        response.put("ok", false);
        if (request.path("v").asInt(-1) != VoiceProtocol.VERSION) {
            fail(session, response, "protocol_mismatch");
            return;
        }
        String operation = request.path("op").asText("");
        JsonNode data = request.path("data");
        if (!data.isObject()) {
            data = JSON.createObjectNode();
        }
        JsonNode result;
        if ("hello".equals(operation)) {
            if (session.hello) {
                fail(session, response, "already_connected");
                return;
            }
            String nickname = data.path("nickname").asText("").strip();
            String ztid = data.path("ztid").asText("").strip().toLowerCase();
            if (!ztid.isEmpty() && !IDENTITY.matcher(ztid).matches()) {
                fail(session, response, "invalid_identity");
                return;
            }
            if (nickname.isEmpty() || nickname.length() > 64) {
                fail(session, response, "invalid_nickname");
                return;
            }
            session.nickname = nickname;
            session.ztid = ztid;
            session.hello = true;
            ObjectNode hello = JSON.createObjectNode();
            hello.put("session", session.id);
            hello.put("udpPort", services.port());
            hello.set("rooms", roomList());
            result = hello;
        } else if (!session.hello) {
            fail(session, response, "hello_required");
            return;
        } else if ("ping".equals(operation)) {
            result = JSON.createObjectNode();
        } else if ("rooms".equals(operation)) {
            result = roomList();
        } else if ("leave".equals(operation)) {
            leave(session);
            result = JSON.createObjectNode();
        } else if ("mute".equals(operation)) {
            JsonNode muted = data.get("muted");
            if (session.room == 0 || muted == null || !muted.isBoolean()) {
                fail(session, response, "invalid_state");
                return;
            }
            session.muted = muted.asBoolean();
            changed(session.room);
            result = JSON.createObjectNode();
        } else if ("create".equals(operation) || "join".equals(operation)) {
            if (session.room != 0) {
                fail(session, response, "already_in_room");
                return;
            }
            long roomId;
            if ("create".equals(operation)) {
                String name = data.path("name").asText("").strip();
                if (name.isEmpty() || name.length() > 64) {
                    fail(session, response, "invalid_room_name");
                    return;
                }
                if (rooms.size() >= maxRooms || nextRoom > MAX_UNSIGNED_INT) {
                    fail(session, response, "room_limit");
                    return;
                }
                roomId = nextRoom;
                nextRoom = (nextRoom + 1) & MAX_UNSIGNED_INT;
                if (nextRoom == 0) {
                    nextRoom = MAX_UNSIGNED_INT + 1;
                }
                rooms.put(roomId, name);
            } else {
                JsonNode value = data.get("room");
                double number = value != null && value.isNumber() ? value.asDouble() : -1;
                if (number < 1 || number > MAX_UNSIGNED_INT || number != Math.floor(number)) {
                    fail(session, response, "room_not_found");
                    return;
                }
                roomId = (long) number;
                if (!rooms.containsKey(roomId)) {
                    fail(session, response, "room_not_found");
                    return;
                }
                if (population(roomId) >= maxMembers) {
                    fail(session, response, "room_full");
                    return;
                }
            }
            session.room = roomId;
            session.token = new byte[16];
            random.nextBytes(session.token);
            ObjectNode joined = JSON.createObjectNode();
            joined.set("room", roomState(roomId));
            joined.put("token", hex(session.token));
            result = joined;
            services.logs().write(LogLevel.INFO,
                    "Session " + session.id + " joined room " + roomId);
        } else {
            fail(session, response, "unknown_operation");
            return;
        }
        response.put("ok", true);
        response.set("data", result);
        send(session.connection, response);
        if ("join".equals(operation) || "create".equals(operation)) {
            changed(session.room);
        }
    }

    private void fail(Session session, ObjectNode response, String code) {
        response.put("ok", false);
        response.put("error", code);
        send(session.connection, response);
    }

    private void send(long connection, ObjectNode object) {
        if (services == null) {
            return;
        }
        byte[] payload;
        try {
            payload = JSON.writeValueAsBytes(object);
        } catch (JsonProcessingException impossible) {
            return;
        }
        if (services.queuedTcpBytes(connection) + payload.length > MAX_SEND_BACKLOG) {
            // Defer the close so a room broadcast cannot erase the session mid-iteration.
            services.scheduler().execute(() -> {
                if (services != null) {
                    services.disconnect(connection);
                }
            });
            return;
        }
        services.sendTcp(connection, payload);
    }

    private void changed(long room) {
        ArrayNode list = roomList();
        for (Session session : sessions.values()) {
            if (!session.hello) {
                continue;
            }
            ObjectNode roomsEvent = JSON.createObjectNode();
            roomsEvent.put("v", VoiceProtocol.VERSION);
            roomsEvent.put("event", "rooms");
            roomsEvent.set("data", list);
            send(session.connection, roomsEvent);
            if (room != 0 && session.room == room) {
                ObjectNode membersEvent = JSON.createObjectNode();
                membersEvent.put("v", VoiceProtocol.VERSION);
                membersEvent.put("event", "members");
                membersEvent.set("data", roomState(room));
                send(session.connection, membersEvent);
            }
        }
    }

    private void leave(Session session) {
        long oldRoom = session.room;
        session.room = 0;
        session.token = new byte[0];
        session.udpPort = 0;
        session.udpAddress = null;
        session.muted = false;
        if (oldRoom != 0) {
            if (population(oldRoom) == 0) {
                rooms.remove(oldRoom);
            }
            if (services != null) {
                services.logs().write(LogLevel.INFO,
                        "Session " + session.id + " left room " + oldRoom);
            }
            changed(oldRoom);
        }
    }

    private int population(long room) {
        int count = 0;
        for (Session session : sessions.values()) {
            if (session.room == room) {
                count++;
            }
        }
        return count;
    }

    private ArrayNode roomList() {
        ArrayNode list = JSON.createArrayNode();
        for (Map.Entry<Long, String> entry : rooms.entrySet()) {
            ObjectNode room = list.addObject();
            room.put("id", entry.getKey());
            room.put("name", entry.getValue());
            room.put("members", population(entry.getKey()));
            room.put("capacity", maxMembers);
        }
        return list;
    }

    private ObjectNode roomState(long id) {
        ObjectNode state = JSON.createObjectNode();
        state.put("id", id);
        state.put("name", rooms.getOrDefault(id, ""));
        ArrayNode members = state.putArray("members");
        for (Session session : sessions.values()) {
            if (session.room != id) {
                continue;
            }
            ObjectNode member = members.addObject();
            member.put("id", session.id);
            member.put("nickname", session.nickname);
            member.put("ztid", session.ztid);
            member.put("muted", session.muted);
        }
        return state;
    }

    private static String hex(byte[] bytes) {
        StringBuilder builder = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            builder.append(Character.forDigit((value >>> 4) & 0xf, 16));
            builder.append(Character.forDigit(value & 0xf, 16));
        }
        return builder.toString();
    }

    private static final class Session {
        private long connection;
        private long id;
        private long room;
        private String nickname = "";
        private String ztid = "";
        private byte[] token = new byte[0];
        private InetAddress udpAddress;
        private int udpPort;
        private boolean hello;
        private boolean muted;
        private long lastSeen;
        private long rateWindow;
        private int controls;
        private int packets;
    }
}
