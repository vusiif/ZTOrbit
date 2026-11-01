package vusiif.ztohub.desktopshare.room;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;
import vusiif.ztohub.desktopshare.protocol.MediaProtocol;
import vusiif.ztohub.desktopshare.protocol.SessionGate;
import vusiif.ztohub.api.Cancellable;

/**
 * Phase-2 room controller. It owns every mutable connection, room, membership and UDP
 * endpoint entry and is confined to the host event thread; the only entry points are
 * the {@code ServerPlugin} callbacks plus {@link #maintenance()}.
 *
 * <p>The controller never trusts a request session ID or role: the connection that
 * receives the request supplies the role, and UDP registration must match that
 * connection's peer address, room, token and session before a port is bound.
 */
public final class RoomController {
    private static final int ROOM_CODE_ATTEMPTS = 32;
    private static final List<String> PROFILES = List.of("720p30", "1080p30");
    private static final SecureRandom RANDOM = new SecureRandom();

    private final HostGateway host;
    private final RoomCodeGenerator roomCodes;
    private final MonotonicClock clock;
    private final Tuning tuning;
    private final JoinFailureLimiter joinFailures;
    private final ByteRateLimiter egress;
    private final EgressFairness egressFairness = new EgressFairness();

    private final Map<Long, Session> sessions = new LinkedHashMap<>();
    private final Map<Long, Room> rooms = new LinkedHashMap<>();
    private final Map<String, Room> roomsByCode = new LinkedHashMap<>();
    private final Set<Long> pendingDisconnects = new LinkedHashSet<>();

    private long nextRoomId = 1;
    private int memberCount;
    private long reservedEgressBits;
    private boolean running;
    private boolean stopped;
    private Cancellable maintenanceTask;

    private long rejectedRequests;
    private long droppedUdpPackets;
    private long acceptedVideoPackets;
    private long closedRooms;
    private long forwardedFragments;
    private long forwardedDropped;
    private long staleSequenceFragments;
    private long notReadyFragments;
    private long ingressLimitedFragments;
    private long egressLimitedFragments;
    private long fairYieldFragments;
    private long controlBacklogDisconnects;
    private boolean flushingDisconnects;
    private long lastStatsLogNanos;

    public RoomController(HostGateway host, RoomCodeGenerator roomCodes, MonotonicClock clock,
            Tuning tuning) {
        this.host = host;
        this.roomCodes = roomCodes;
        this.clock = clock;
        this.tuning = tuning;
        this.joinFailures = new JoinFailureLimiter(tuning.joinFailureWindowNanos(),
                tuning.joinFailureLimit(), tuning.joinFailureIdleNanos(),
                tuning.joinFailureSources());
        this.egress = new ByteRateLimiter(tuning.egressBudgetBitsPerSecond() / 8L,
                tuning.egressBurstBytes(), clock.nanoTime());
    }

    /** Schedules maintenance. Call once when the plugin becomes ready. */
    public void start() {
        if (stopped) {
            throw new IllegalStateException("room controller already stopped");
        }
        if (running) {
            return;
        }
        running = true;
        try {
            maintenanceTask = host.scheduleAtFixedRate(
                    this::maintenance, tuning.maintenanceIntervalMillis());
        } catch (RuntimeException failure) {
            running = false;
            maintenanceTask = null;
            throw failure;
        }
        host.info("desktop-share 房间控制已启动（route=desktop-share）");
    }

    /**
     * Cancels the maintenance timer, closes every room with {@code server_shutdown}
     * and invalidates all credentials. Idempotent.
     */
    public void stop() {
        if (stopped) {
            return;
        }
        stopped = true;
        running = false;
        Cancellable task = maintenanceTask;
        maintenanceTask = null;
        if (task != null) {
            task.cancel();
        }
        for (Room room : new ArrayList<>(rooms.values())) {
            closeRoom(room, ControlProtocol.CloseReason.SERVER_SHUTDOWN);
        }
        rooms.clear();
        roomsByCode.clear();
        sessions.clear();
        memberCount = 0;
        reservedEgressBits = 0;
        egressFairness.clear();
        flushDisconnects();
        host.info("desktop-share 房间控制已停止");
    }

    /** Handles one decoded ZTS1 TCP payload. */
    public void onTcpPacket(long connectionId, byte[] payload) {
        try {
            handleTcpPacket(connectionId, payload);
        } finally {
            // A control response that crossed the 256 KiB backlog is disconnected
            // only after the handler finished mutating room/member state.
            flushDisconnects();
        }
    }

    private void handleTcpPacket(long connectionId, byte[] payload) {
        if (!running) {
            return;
        }
        Session session = session(connectionId);
        ControlProtocol.Request request;
        try {
            request = ControlProtocol.decodeRequest(payload);
        } catch (ControlProtocol.ProtocolException error) {
            long id = ControlProtocol.peekRequestId(payload);
            if (id != 0) {
                rejectedRequests++;
                send(connectionId, ControlProtocol.encodeError(id, error.code(),
                        error.getMessage()));
            }
            return;
        }
        long now = clock.nanoTime();
        if (!session.requests.take(now)) {
            fail(session, request, ControlProtocol.ErrorCode.RATE_LIMITED,
                    "控制请求过于频繁");
            return;
        }
        ControlProtocol.ErrorCode gate =
                SessionGate.gateError(session.phase(), request.op());
        if (gate != ControlProtocol.ErrorCode.NONE) {
            fail(session, request, gate, session.handshaken
                    ? "当前操作需要房间上下文" : "请先完成 hello 握手");
            return;
        }
        // Only a request that passes the phase gate counts as valid control activity.
        // Malformed frames, rate-limited requests and pre-handshake operations must not
        // keep a session alive.
        session.lastActivityNanos = now;
        switch (request.op()) {
            case HELLO -> handleHello(session, request);
            case CREATE -> handleCreate(session, request);
            case JOIN -> handleJoin(session, request, now);
            case LEAVE -> handleLeave(session, request);
            case STOP -> handleStop(session, request);
            case PING -> send(session, ControlProtocol.Response.success(request.id(),
                    ControlProtocol.Operation.PING));
            case STREAM_CONFIGURE -> handleConfigure(session, request);
            case STREAM_READY -> handleReady(session, request);
            case STREAM_KEYFRAME -> handleKeyframe(session, request, now);
            case STREAM_FEEDBACK -> handleFeedback(session, request, now);
        }
    }

    /** Handles one decoded ZTS1 UDP payload. Invalid media is silently discarded. */
    public void onUdpPacket(InetSocketAddress sender, byte[] payload) {
        try {
            handleUdpPacket(sender, payload);
        } finally {
            flushDisconnects();
        }
    }

    private void handleUdpPacket(InetSocketAddress sender, byte[] payload) {
        if (!running || sender == null || sender.getAddress() == null) {
            return;
        }
        MediaProtocol.Decoded decoded;
        try {
            decoded = MediaProtocol.decode(payload);
        } catch (MediaProtocol.MediaException error) {
            droppedUdpPackets++;
            return;
        }
        MediaProtocol.Header header = decoded.header();
        Session session = sessions.get(header.sessionId());
        if (session == null || !session.handshaken || session.roomId == null
                || session.roomId.longValue() != header.roomId()
                || session.token == null
                || !Arrays.equals(session.token, header.token())) {
            droppedUdpPackets++;
            return;
        }
        Room room = rooms.get(session.roomId);
        if (room == null || room.closed) {
            droppedUdpPackets++;
            return;
        }
        if (header.type() == MediaProtocol.Type.ACK) {
            droppedUdpPackets++;
            return;
        }
        if (session.udpEndpoint == null) {
            if (header.type() != MediaProtocol.Type.REGISTER
                    || session.peer == null
                    || !session.peer.equals(sender.getAddress())) {
                droppedUdpPackets++;
                return;
            }
            session.udpEndpoint = new InetSocketAddress(sender.getAddress(), sender.getPort());
            session.udpActivityNanos = clock.nanoTime();
            session.udpRegisterDeadlineNanos = 0;
        } else {
            if (!session.udpEndpoint.equals(sender)) {
                droppedUdpPackets++;
                return;
            }
        }

        if (header.type() == MediaProtocol.Type.REGISTER) {
            session.udpActivityNanos = clock.nanoTime();
            try {
                byte[] ack = MediaProtocol.encodeAck(session.token, session.connectionId,
                        room.id);
                host.sendUdp(session.udpEndpoint, ack);
            } catch (MediaProtocol.MediaException failure) {
                host.warn("desktop-share 注册确认编码失败：" + failure.getMessage());
            }
            return;
        }

        if (session.role != ControlProtocol.Role.PUBLISHER) {
            droppedUdpPackets++;
            return;
        }
        if (room.streamEpoch == 0 || header.streamEpoch() != room.streamEpoch) {
            droppedUdpPackets++;
            return;
        }
        long now = clock.nanoTime();
        session.udpActivityNanos = now;
        // Per-room publisher ingress limit: profile bitrate x2 with a 1 MiB burst,
        // counted as complete ZTS1 datagram bytes. Excess input is dropped here and
        // never reaches the fan-out loop, so one publisher cannot occupy the event
        // thread indefinitely with a flood.
        if (!room.ingress.take((long) payload.length + MediaProtocol.ZTS1_OUTER_BYTES, now)) {
            ingressLimitedFragments++;
            droppedUdpPackets++;
            return;
        }
        // Sequence window: a fragment far behind the newest sequence is stale and must
        // not be forwarded to viewers as if it were current video.
        long sequence = header.sequence();
        if (sequence < 0 || (room.highestSequence - sequence >= Tuning.SEQUENCE_WINDOW
                && room.highestSequence >= Tuning.SEQUENCE_WINDOW)) {
            staleSequenceFragments++;
            droppedUdpPackets++;
            return;
        }
        room.highestSequence = Math.max(room.highestSequence, sequence);
        acceptedVideoPackets++;
        forwardFragment(room, header, decoded.fragment(), now);
    }

    /**
     * Fans one validated publisher fragment out to the room's registered, ready viewers.
     * Only the target token/session are replaced; the encoded payload is copied verbatim.
     * The server keeps no video history, key-frame cache or reassembly state.
     */
    private void forwardFragment(Room room, MediaProtocol.Header header, byte[] fragment,
            long now) {
        int viewers = room.viewers.size();
        if (viewers == 0) {
            egressFairness.release(room.id);
            return;
        }
        List<Session> members = new ArrayList<>(room.viewers.values());
        int start = (int) (room.fanoutCursor++ % viewers);
        if (start < 0) {
            start = 0;
        }
        // Cross-room fairness first: the shared bucket is drained by whichever room
        // the host happens to process first, so a room that cannot get any copy
        // keeps the egress turn until a refill serves it. The unit cost is one
        // retargeted datagram; retarget() preserves the payload length. The whole
        // fan-out cost decides whether both rooms can be served together.
        long unitCost = (long) MediaProtocol.HEADER_BYTES + fragment.length
                + MediaProtocol.ZTS1_OUTER_BYTES;
        int readyViewers = 0;
        for (Session viewer : members) {
            if (viewer.udpEndpoint != null && viewer.token != null
                    && viewer.readyEpoch == room.streamEpoch) {
                readyViewers++;
            }
        }
        long requestCost = unitCost * readyViewers;
        if (readyViewers > 0
                && !egressFairness.admit(room.id, unitCost, requestCost, egress.available(now),
                        now)) {
            room.fairYieldFragments++;
            fairYieldFragments++;
            return;
        }
        int sentCopies = 0;
        int tokenDeniedCopies = 0;
        for (int index = 0; index < viewers; index++) {
            Session viewer = members.get((start + index) % viewers);
            if (viewer.udpEndpoint == null || viewer.token == null
                    || viewer.readyEpoch != room.streamEpoch) {
                viewer.notReadyFragments++;
                notReadyFragments++;
                continue;
            }
            byte[] forwarded;
            try {
                forwarded = MediaProtocol.retarget(header, viewer.token,
                        viewer.connectionId, fragment);
            } catch (MediaProtocol.MediaException failure) {
                viewer.sendDroppedFragments++;
                forwardedDropped++;
                continue;
            }
            // The server-wide egress token bucket counts complete ZTS1 datagram bytes
            // and allows a 1 MiB burst. Insufficient tokens drop this viewer's copy
            // immediately; the remaining viewers are still attempted and the rotating
            // start position keeps the loss from always hitting the same members.
            long cost = (long) forwarded.length + MediaProtocol.ZTS1_OUTER_BYTES;
            if (!egress.take(cost, now)) {
                viewer.egressLimitedFragments++;
                egressLimitedFragments++;
                forwardedDropped++;
                tokenDeniedCopies++;
                continue;
            }
            try {
                if (host.sendUdp(viewer.udpEndpoint, forwarded)) {
                    viewer.forwardedFragments++;
                    forwardedFragments++;
                    sentCopies++;
                } else {
                    egress.refund(cost, now);
                    viewer.sendDroppedFragments++;
                    forwardedDropped++;
                }
            } catch (RuntimeException failure) {
                egress.refund(cost, now);
                viewer.sendDroppedFragments++;
                forwardedDropped++;
            }
        }
        if (readyViewers > 0) {
            egressFairness.conclude(room.id, sentCopies, tokenDeniedCopies, requestCost, now);
        }
    }

    /** Handles a closed TCP connection; the host calls this exactly once per ID. */
    public void onConnectionClosed(long connectionId) {
        try {
            handleConnectionClosed(connectionId);
        } finally {
            flushDisconnects();
        }
    }

    private void handleConnectionClosed(long connectionId) {
        if (!running) {
            return;
        }
        Session session = sessions.remove(connectionId);
        if (session == null || session.roomId == null) {
            return;
        }
        Room room = rooms.get(session.roomId);
        if (room == null) {
            session.clearMembership();
            return;
        }
        if (session.role == ControlProtocol.Role.PUBLISHER) {
            closeRoom(room, ControlProtocol.CloseReason.OWNER_DISCONNECTED);
        } else {
            removeViewer(room, session, false);
            notifyRoomState(room);
        }
    }

    /** Periodic expiry pass. Runs on the host event thread through the scheduler. */
    public void maintenance() {
        try {
            handleMaintenance();
        } finally {
            flushDisconnects();
        }
    }

    private void handleMaintenance() {
        if (!running) {
            return;
        }
        long now = clock.nanoTime();
        for (Session session : new ArrayList<>(sessions.values())) {
            if (sessions.get(session.connectionId) != session) {
                continue;
            }
            if (now - session.lastActivityNanos >= tuning.controlIdleNanos()) {
                expireSession(session);
            }
        }
        for (Session session : new ArrayList<>(sessions.values())) {
            if (sessions.get(session.connectionId) != session
                    || session.roomId == null
                    || session.udpEndpoint != null
                    || session.udpRegisterDeadlineNanos == 0
                    || now < session.udpRegisterDeadlineNanos) {
                continue;
            }
            expireRegistration(session);
        }
        for (Room room : new ArrayList<>(rooms.values())) {
            if (room.closed || rooms.get(room.id) != room) {
                continue;
            }
            if (room.state == ControlProtocol.RoomState.PREPARING
                    && now >= room.prepareDeadlineNanos) {
                closeRoom(room, ControlProtocol.CloseReason.CONFIG_TIMEOUT);
            }
        }
        for (Session session : new ArrayList<>(sessions.values())) {
            if (sessions.get(session.connectionId) != session
                    || session.udpEndpoint == null) {
                continue;
            }
            if (now - session.udpActivityNanos >= tuning.udpIdleNanos()) {
                expireEndpoint(session);
            }
        }
        joinFailures.maintenance(now);
        egressFairness.maintenance(now);
        // Aggregated forwarding counters only; no tokens, room codes or payloads.
        if (now - lastStatsLogNanos >= tuning.statsLogIntervalNanos()) {
            lastStatsLogNanos = now;
            host.info("desktop-share 转发统计 rooms=" + rooms.size()
                    + " sessions=" + sessions.size()
                    + " accepted=" + acceptedVideoPackets
                    + " forwarded=" + forwardedFragments
                    + " notReady=" + notReadyFragments
                    + " stale=" + staleSequenceFragments
                    + " sendDropped=" + forwardedDropped
                    + " ingressLimited=" + ingressLimitedFragments
                    + " egressLimited=" + egressLimitedFragments
                    + " fairYield=" + fairYieldFragments
                    + " controlBacklog=" + controlBacklogDisconnects
                    + " udpDropped=" + droppedUdpPackets);
        }
    }

    public Stats stats() {
        int endpoints = 0;
        for (Session session : sessions.values()) {
            if (session.udpEndpoint != null) {
                endpoints++;
            }
        }
        List<ViewerStats> viewers = new ArrayList<>();
        List<RoomFairnessStats> roomFairness = new ArrayList<>();
        for (Room room : rooms.values()) {
            long roomForwarded = 0;
            long roomEgressLimited = 0;
            for (Session viewer : room.viewers.values()) {
                viewers.add(new ViewerStats(viewer.connectionId, room.id,
                        egressFor(room.profile), viewer.readyEpoch == room.streamEpoch,
                        viewer.forwardedFragments, viewer.sendDroppedFragments,
                        viewer.egressLimitedFragments, viewer.notReadyFragments));
                roomForwarded += viewer.forwardedFragments;
                roomEgressLimited += viewer.egressLimitedFragments;
            }
            roomFairness.add(new RoomFairnessStats(room.id, room.fairYieldFragments,
                    roomForwarded, roomEgressLimited,
                    egressFairness.waitingRoom() == room.id,
                    egressFairness.turnHolderRoom() == room.id));
        }
        return new Stats(sessions.size(), rooms.size(), memberCount, endpoints,
                reservedEgressBits, joinFailures.size(), droppedUdpPackets, rejectedRequests,
                closedRooms, acceptedVideoPackets, forwardedFragments, forwardedDropped,
                staleSequenceFragments, notReadyFragments, ingressLimitedFragments,
                egressLimitedFragments, fairYieldFragments, controlBacklogDisconnects,
                List.copyOf(viewers), List.copyOf(roomFairness));
    }

    /** Observable counters; no tokens, room codes or video content. */
    public record Stats(int sessions, int rooms, int members, int registeredEndpoints,
                        long reservedEgressBitsPerSecond, int joinFailureSources,
                        long droppedUdpPackets, long rejectedRequests, long closedRooms,
                        long acceptedVideoPackets, long forwardedFragments,
                        long forwardedDropped, long staleSequenceFragments,
                        long notReadyFragments, long ingressLimitedFragments,
                        long egressLimitedFragments, long fairYieldFragments,
                        long controlBacklogDisconnects, List<ViewerStats> viewers,
                        List<RoomFairnessStats> roomFairness) {}

    /** Per-viewer forwarding outcome; no token, room code, endpoint or payload. */
    public record ViewerStats(long connectionId, long roomId, long reservedBitsPerSecond,
                        boolean ready, long forwardedFragments, long sendDroppedFragments,
                        long egressLimitedFragments, long notReadyFragments) {}

    /**
     * Per-room egress outcome: how many fragments the room withheld so another room
     * could progress, and whether it currently owns the fairness turn or waits for
     * the next refill. Counters only, never payload.
     */
    public record RoomFairnessStats(long roomId, long fairYieldFragments,
                        long forwardedFragments, long egressLimitedFragments,
                        boolean waitingForEgress, boolean holdingEgressTurn) {}

    private void handleHello(Session session, ControlProtocol.Request request) {
        session.handshaken = true;
        if (session.peer == null) {
            session.peer = host.peerAddress(session.connectionId);
        }
        ControlProtocol.HelloInfo info = new ControlProtocol.HelloInfo(
                session.connectionId, host.port(), PROFILES, tuning.maxViewersPerRoom());
        send(session, ControlProtocol.Response.hello(request.id(), info));
    }

    private void handleCreate(Session session, ControlProtocol.Request request) {
        if (session.roomId != null) {
            Room current = rooms.get(session.roomId);
            if (current != null && session.role == ControlProtocol.Role.PUBLISHER) {
                respondRoom(session, request, current);
            } else {
                fail(session, request, ControlProtocol.ErrorCode.ALREADY_IN_ROOM,
                        "当前已在其他房间");
            }
            return;
        }
        if (rooms.size() >= tuning.maxRooms()
                || memberCount + 1 > tuning.maxMembersPerServer()) {
            fail(session, request, ControlProtocol.ErrorCode.CAPACITY,
                    "服务端房间或会话容量不足");
            return;
        }
        String code = allocateRoomCode();
        if (code == null) {
            fail(session, request, ControlProtocol.ErrorCode.INTERNAL,
                    "房号生成失败");
            return;
        }
        long roomId = nextRoomId++;
        if (roomId <= 0) {
            roomId = 1;
            nextRoomId = 2;
        }
        byte[] token = newToken();
        Room room = new Room(roomId, code, ControlProtocol.profile(request.profile()),
                session, clock.nanoTime(), tuning);
        room.prepareDeadlineNanos = clock.nanoTime() + tuning.prepareTimeoutNanos();
        rooms.put(roomId, room);
        roomsByCode.put(code, room);
        memberCount++;
        session.roomId = roomId;
        session.role = ControlProtocol.Role.PUBLISHER;
        session.token = token;
        session.readyEpoch = 0;
        session.udpRegisterDeadlineNanos =
                clock.nanoTime() + tuning.udpRegisterTimeoutNanos();
        host.info("room " + roomId + " 创建 profile=" + room.profile.name());
        respondRoom(session, request, room);
    }

    private void handleJoin(Session session, ControlProtocol.Request request, long now) {
        String code = request.roomCode();
        if (session.roomId != null) {
            Room current = rooms.get(session.roomId);
            if (current != null && session.role == ControlProtocol.Role.VIEWER
                    && current.code.equals(code)) {
                respondRoom(session, request, current);
            } else {
                fail(session, request, ControlProtocol.ErrorCode.ALREADY_IN_ROOM,
                        "当前已在其他房间");
            }
            return;
        }
        JoinFailureLimiter.Decision decision = joinFailures.check(session.peer, now);
        if (decision == JoinFailureLimiter.Decision.BLOCKED) {
            fail(session, request, ControlProtocol.ErrorCode.RATE_LIMITED,
                    "入房尝试过于频繁");
            return;
        }
        if (decision == JoinFailureLimiter.Decision.TABLE_FULL) {
            fail(session, request, ControlProtocol.ErrorCode.CAPACITY,
                    "服务端入房限流表已满");
            return;
        }
        Room room = roomsByCode.get(code);
        if (room == null) {
            joinFailures.recordFailure(session.peer, now);
            fail(session, request, ControlProtocol.ErrorCode.ROOM_NOT_FOUND,
                    "房间不存在或已结束");
            return;
        }
        if (room.viewers.size() >= tuning.maxViewersPerRoom()) {
            joinFailures.recordFailure(session.peer, now);
            fail(session, request, ControlProtocol.ErrorCode.ROOM_FULL,
                    "房间观看人数已满");
            return;
        }
        if (memberCount + 1 > tuning.maxMembersPerServer()) {
            joinFailures.recordFailure(session.peer, now);
            fail(session, request, ControlProtocol.ErrorCode.CAPACITY,
                    "服务端房间或会话容量不足");
            return;
        }
        long reserve = egressFor(room.profile);
        if (reservedEgressBits + reserve > tuning.egressBudgetBitsPerSecond()) {
            joinFailures.recordFailure(session.peer, now);
            fail(session, request, ControlProtocol.ErrorCode.CAPACITY,
                    "服务端视频出口容量不足");
            return;
        }
        session.roomId = room.id;
        session.role = ControlProtocol.Role.VIEWER;
        session.token = newToken();
        session.readyEpoch = 0;
        session.feedback = null;
        session.lastFeedbackNanos = 0;
        session.udpRegisterDeadlineNanos = now + tuning.udpRegisterTimeoutNanos();
        room.viewers.put(session.connectionId, session);
        memberCount++;
        reservedEgressBits += reserve;
        // A successful join or a TCP reconnect never clears the source failure
        // window; STEP.md requires failures to accumulate per source IP for the
        // whole minute even when the same address later joins a valid room.
        respondRoom(session, request, room);
        notifyRoomState(room);
    }

    private void handleLeave(Session session, ControlProtocol.Request request) {
        if (session.roomId == null) {
            send(session, ControlProtocol.Response.success(request.id(),
                    ControlProtocol.Operation.LEAVE));
            return;
        }
        if (session.role == ControlProtocol.Role.PUBLISHER) {
            fail(session, request, ControlProtocol.ErrorCode.FORBIDDEN,
                    "共享者请使用 stop 结束共享");
            return;
        }
        if (session.roomId.longValue() != request.roomId()) {
            fail(session, request, ControlProtocol.ErrorCode.STALE_STREAM,
                    "请求房间与当前观看房间不一致");
            return;
        }
        Room room = rooms.get(session.roomId);
        send(session, ControlProtocol.Response.success(request.id(),
                ControlProtocol.Operation.LEAVE));
        if (room != null) {
            removeViewer(room, session, false);
            notifyRoomState(room);
        } else {
            session.clearMembership();
        }
    }

    private void handleStop(Session session, ControlProtocol.Request request) {
        if (session.roomId == null) {
            send(session, ControlProtocol.Response.success(request.id(),
                    ControlProtocol.Operation.STOP));
            return;
        }
        if (session.role != ControlProtocol.Role.PUBLISHER) {
            fail(session, request, ControlProtocol.ErrorCode.FORBIDDEN,
                    "观看者不能关闭房间");
            return;
        }
        if (session.roomId.longValue() != request.roomId()) {
            fail(session, request, ControlProtocol.ErrorCode.STALE_STREAM,
                    "请求房间与当前共享房间不一致");
            return;
        }
        Room room = rooms.get(session.roomId);
        send(session, ControlProtocol.Response.success(request.id(),
                ControlProtocol.Operation.STOP));
        if (room != null) {
            closeRoom(room, ControlProtocol.CloseReason.OWNER_STOPPED);
        } else {
            session.clearMembership();
        }
    }

    private void handleConfigure(Session session, ControlProtocol.Request request) {
        Room room = requireRoom(session, request);
        if (room == null) {
            return;
        }
        if (session.role != ControlProtocol.Role.PUBLISHER) {
            fail(session, request, ControlProtocol.ErrorCode.FORBIDDEN,
                    "只有共享者可以配置视频流");
            return;
        }
        ControlProtocol.StreamFormat requested = request.format();
        if (requested == null) {
            fail(session, request, ControlProtocol.ErrorCode.INVALID_REQUEST,
                    "缺少视频流格式");
            return;
        }
        ControlProtocol.ProfileInfo profile = ControlProtocol.profileForDimensions(
                requested.width(), requested.height(), requested.fps(), requested.bitrate());
        if (profile == null) {
            fail(session, request, ControlProtocol.ErrorCode.INVALID_REQUEST,
                    "视频流格式不在已定义档位中");
            return;
        }
        boolean firstConfiguration = room.state == ControlProtocol.RoomState.PREPARING;
        if (firstConfiguration && !profile.name().equals(room.profile.name())) {
            fail(session, request, ControlProtocol.ErrorCode.INVALID_REQUEST,
                    "首次视频流格式必须匹配建房档位");
            return;
        }
        if (!firstConfiguration) {
            long oldReserve = egressFor(room.profile);
            long newReserve = egressFor(profile);
            long totalDelta = (newReserve - oldReserve) * room.viewers.size();
            long required = reservedEgressBits + totalDelta;
            if (required > tuning.egressBudgetBitsPerSecond()) {
                fail(session, request, ControlProtocol.ErrorCode.CAPACITY,
                        "服务端视频出口容量不足，无法切换档位");
                return;
            }
            room.profile = profile;
            room.ingress.reset(ingressBytesPerSecond(profile), tuning.ingressBurstBytes(),
                    clock.nanoTime());
            reservedEgressBits = Math.max(0, required);
        }
        long epoch = room.streamEpoch + 1;
        if (epoch <= 0) {
            epoch = 1;
        }
        room.streamEpoch = epoch;
        room.format = requested.withStreamEpoch(epoch);
        room.state = ControlProtocol.RoomState.RUNNING;
        // A new epoch restarts the wire sequence and its validation window.
        room.highestSequence = 0;
        room.fanoutCursor = 0;
        // Every viewer must ready again, so the fairness turn of the previous
        // epoch cannot be used to hold other rooms back.
        egressFairness.release(room.id);
        for (Session viewer : room.viewers.values()) {
            viewer.readyEpoch = 0;
        }
        send(session, ControlProtocol.Response.stream(request.id(),
                ControlProtocol.Operation.STREAM_CONFIGURE, room.id, epoch));
        notifyStreamConfig(room);
        if (firstConfiguration) {
            notifyRoomState(room);
        }
    }

    private void handleReady(Session session, ControlProtocol.Request request) {
        Room room = requireRoom(session, request);
        if (room == null) {
            return;
        }
        if (session.role != ControlProtocol.Role.VIEWER) {
            fail(session, request, ControlProtocol.ErrorCode.FORBIDDEN,
                    "只有观看者可以确认接收轨道");
            return;
        }
        if (room.format == null || room.streamEpoch == 0) {
            fail(session, request, ControlProtocol.ErrorCode.NOT_READY,
                    "尚无有效流配置");
            return;
        }
        if (request.streamEpoch() != room.streamEpoch) {
            fail(session, request, ControlProtocol.ErrorCode.STALE_STREAM,
                    "流代次已失效");
            return;
        }
        session.readyEpoch = room.streamEpoch;
        send(session, ControlProtocol.Response.stream(request.id(),
                ControlProtocol.Operation.STREAM_READY, room.id, room.streamEpoch));
    }

    private void handleKeyframe(Session session, ControlProtocol.Request request, long now) {
        Room room = requireRoom(session, request);
        if (room == null) {
            return;
        }
        if (session.role != ControlProtocol.Role.VIEWER) {
            fail(session, request, ControlProtocol.ErrorCode.FORBIDDEN,
                    "只有观看者可以请求关键帧");
            return;
        }
        if (room.format == null || room.streamEpoch == 0) {
            fail(session, request, ControlProtocol.ErrorCode.NOT_READY,
                    "尚无有效流配置");
            return;
        }
        if (request.streamEpoch() != room.streamEpoch) {
            fail(session, request, ControlProtocol.ErrorCode.STALE_STREAM,
                    "流代次已失效");
            return;
        }
        send(session, ControlProtocol.Response.stream(request.id(),
                ControlProtocol.Operation.STREAM_KEYFRAME, room.id, room.streamEpoch));
        if (now - room.lastKeyframeNanos >= tuning.keyframeIntervalNanos()) {
            room.lastKeyframeNanos = now;
            sendEvent(room.owner.connectionId,
                    ControlProtocol.Event.streamKeyframe(room.id, room.streamEpoch));
        }
    }

    private void handleFeedback(Session session, ControlProtocol.Request request, long now) {
        Room room = requireRoom(session, request);
        if (room == null) {
            return;
        }
        if (session.role != ControlProtocol.Role.VIEWER) {
            fail(session, request, ControlProtocol.ErrorCode.FORBIDDEN,
                    "只有观看者可以上报接收反馈");
            return;
        }
        if (room.format == null || room.streamEpoch == 0) {
            fail(session, request, ControlProtocol.ErrorCode.NOT_READY,
                    "尚无有效流配置");
            return;
        }
        if (request.streamEpoch() != room.streamEpoch) {
            fail(session, request, ControlProtocol.ErrorCode.STALE_STREAM,
                    "流代次已失效");
            return;
        }
        if (now - session.lastFeedbackNanos < tuning.feedbackIntervalNanos()) {
            // Keep the window unchanged and report the interval actually measured
            // at this server, so a diagnostic client can tell a genuinely too
            // frequent reporter from a network trip that compressed two
            // one-second reports. This is display text only, never a credential.
            final long observedMs = (now - session.lastFeedbackNanos) / 1_000_000L;
            final long minimumMs = tuning.feedbackIntervalNanos() / 1_000_000L;
            fail(session, request, ControlProtocol.ErrorCode.RATE_LIMITED,
                    "接收反馈上报过于频繁（间隔 " + observedMs + " ms，最小 " + minimumMs + " ms）");
            return;
        }
        session.lastFeedbackNanos = now;
        session.feedback = request.feedback();
        send(session, ControlProtocol.Response.success(request.id(),
                ControlProtocol.Operation.STREAM_FEEDBACK));
    }

    private Room requireRoom(Session session, ControlProtocol.Request request) {
        if (session.roomId == null) {
            fail(session, request, ControlProtocol.ErrorCode.NOT_READY,
                    "当前不在房间中");
            return null;
        }
        if (session.roomId.longValue() != request.roomId()) {
            fail(session, request, ControlProtocol.ErrorCode.STALE_STREAM,
                    "请求房间与当前房间不一致");
            return null;
        }
        Room room = rooms.get(session.roomId);
        if (room == null || room.closed) {
            fail(session, request, ControlProtocol.ErrorCode.STALE_STREAM,
                    "房间已关闭");
            return null;
        }
        return room;
    }

    private void respondRoom(Session session, ControlProtocol.Request request, Room room) {
        ControlProtocol.StreamFormat format = session.role == ControlProtocol.Role.VIEWER
                ? room.format : null;
        ControlProtocol.RoomInfo info = new ControlProtocol.RoomInfo(room.id, room.code,
                room.profile.name(), session.role, session.token, format);
        send(session, ControlProtocol.Response.room(request.id(), request.op(), info));
    }

    private void notifyRoomState(Room room) {
        if (room.closed) {
            return;
        }
        byte[] event = ControlProtocol.encodeEvent(ControlProtocol.Event.roomState(
                room.id, room.state, room.viewers.size()));
        if (event.length == 0) {
            return;
        }
        send(room.owner.connectionId, event);
        for (Session viewer : room.viewers.values()) {
            send(viewer.connectionId, event);
        }
    }

    private void notifyStreamConfig(Room room) {
        if (room.closed || room.format == null) {
            return;
        }
        byte[] event = ControlProtocol.encodeEvent(
                ControlProtocol.Event.streamConfig(room.id, room.format));
        if (event.length == 0) {
            return;
        }
        for (Session viewer : room.viewers.values()) {
            send(viewer.connectionId, event);
        }
    }

    private void sendEvent(long connectionId, ControlProtocol.Event event) {
        byte[] bytes = ControlProtocol.encodeEvent(event);
        if (bytes.length > 0) {
            send(connectionId, bytes);
        }
    }

    private void send(Session session, ControlProtocol.Response response) {
        send(session.connectionId, ControlProtocol.encodeResponse(response));
    }

    private void send(long connectionId, byte[] bytes) {
        if (bytes != null && bytes.length > 0) {
            if (host.sendTcp(connectionId, bytes)) {
                long queued = host.queuedTcpBytes(connectionId);
                if (queued > tuning.slowConnectionBytes()) {
                    // Do not block the event thread on a peer that cannot drain its
                    // control socket. The disconnect is queued and flushed after the
                    // current handler so room/member state is already consistent.
                    requestDisconnect(connectionId);
                }
            }
        }
    }

    private void requestDisconnect(long connectionId) {
        pendingDisconnects.add(connectionId);
    }

    private void flushDisconnects() {
        if (flushingDisconnects) {
            return;
        }
        flushingDisconnects = true;
        try {
            while (!pendingDisconnects.isEmpty()) {
                long connectionId = pendingDisconnects.iterator().next();
                pendingDisconnects.remove(connectionId);
                controlBacklogDisconnects++;
                host.disconnect(connectionId);
            }
        } finally {
            flushingDisconnects = false;
        }
    }

    private void fail(Session session, ControlProtocol.Request request,
            ControlProtocol.ErrorCode code, String message) {
        rejectedRequests++;
        send(session.connectionId, ControlProtocol.encodeResponse(
                ControlProtocol.Response.failure(request.id(), request.op(), code, message)));
    }

    private void removeViewer(Room room, Session session, boolean notifyViewer) {
        if (!room.viewers.containsKey(session.connectionId)) {
            return;
        }
        if (notifyViewer) {
            // A server-initiated viewer removal is a membership end, not a room
            // close. Reuse room.closed with stream_failed for this viewer only so
            // the client cannot keep showing a session the server already dropped;
            // the publisher and remaining viewers are unaffected.
            sendEvent(session.connectionId, ControlProtocol.Event.roomClosed(room.id,
                    ControlProtocol.CloseReason.STREAM_FAILED));
        }
        room.viewers.remove(session.connectionId);
        memberCount = Math.max(0, memberCount - 1);
        reservedEgressBits = Math.max(0, reservedEgressBits - egressFor(room.profile));
        if (room.viewers.isEmpty()) {
            // No viewer can consume egress any more; do not keep a turn or a
            // pending demand that would hold other rooms back.
            egressFairness.release(room.id);
        }
        host.info("room " + room.id + " viewer conn=" + session.connectionId
                + " forwarded=" + session.forwardedFragments
                + " sendDropped=" + session.sendDroppedFragments
                + " egressLimited=" + session.egressLimitedFragments
                + " notReady=" + session.notReadyFragments
                + " ready=" + (session.readyEpoch == room.streamEpoch));
        session.clearMembership();
    }

    private void closeRoom(Room room, ControlProtocol.CloseReason reason) {
        if (room.closed) {
            return;
        }
        room.closed = true;
        rooms.remove(room.id);
        roomsByCode.remove(room.code);
        egressFairness.release(room.id);
        int members = 1 + room.viewers.size();
        memberCount = Math.max(0, memberCount - members);
        reservedEgressBits = Math.max(0,
                reservedEgressBits - room.viewers.size() * egressFor(room.profile));
        List<Session> affected = new ArrayList<>(1 + room.viewers.size());
        // Per-viewer outcome before membership is cleared; only internal IDs and counts.
        for (Session viewer : room.viewers.values()) {
            host.info("room " + room.id + " viewer conn=" + viewer.connectionId
                    + " forwarded=" + viewer.forwardedFragments
                    + " sendDropped=" + viewer.sendDroppedFragments
                    + " egressLimited=" + viewer.egressLimitedFragments
                    + " notReady=" + viewer.notReadyFragments
                    + " ready=" + (viewer.readyEpoch == room.streamEpoch));
        }
        affected.add(room.owner);
        affected.addAll(room.viewers.values());
        room.viewers.clear();
        for (Session session : affected) {
            if (session.roomId != null && session.roomId.longValue() == room.id) {
                session.clearMembership();
            }
        }
        closedRooms++;
        host.info("room " + room.id + " 关闭 reason=" + reason.wireName()
                + " members=" + members);
        byte[] closed = ControlProtocol.encodeEvent(
                ControlProtocol.Event.roomClosed(room.id, reason));
        if (closed.length > 0) {
            for (Session session : affected) {
                if (!host.sendTcp(session.connectionId, closed)
                        || host.queuedTcpBytes(session.connectionId)
                                > tuning.slowConnectionBytes()) {
                    requestDisconnect(session.connectionId);
                }
            }
        }
        // The caller's finally block flushes; disconnecting here would re-enter
        // onConnectionClosed while this room is still being finished.
    }

    private void expireSession(Session session) {
        Room room = session.roomId == null ? null : rooms.get(session.roomId);
        if (room != null && session.role == ControlProtocol.Role.PUBLISHER) {
            closeRoom(room, ControlProtocol.CloseReason.OWNER_LOST);
        } else if (room != null && session.role == ControlProtocol.Role.VIEWER) {
            removeViewer(room, session, true);
            notifyRoomState(room);
        }
        sessions.remove(session.connectionId);
        host.info("session " + session.connectionId + " 控制活动超时已清理");
    }

    private void expireRegistration(Session session) {
        session.udpRegisterDeadlineNanos = 0;
        Room room = session.roomId == null ? null : rooms.get(session.roomId);
        if (room != null && session.role == ControlProtocol.Role.PUBLISHER) {
            closeRoom(room, ControlProtocol.CloseReason.STREAM_FAILED);
        } else if (room != null && session.role == ControlProtocol.Role.VIEWER) {
            removeViewer(room, session, true);
            notifyRoomState(room);
        }
        host.info("session " + session.connectionId + " UDP 注册超时");
    }

    private void expireEndpoint(Session session) {
        session.udpEndpoint = null;
        session.udpActivityNanos = 0;
        Room room = session.roomId == null ? null : rooms.get(session.roomId);
        if (room != null && session.role == ControlProtocol.Role.PUBLISHER) {
            closeRoom(room, ControlProtocol.CloseReason.STREAM_FAILED);
        } else if (room != null && session.role == ControlProtocol.Role.VIEWER) {
            removeViewer(room, session, true);
            notifyRoomState(room);
        }
        host.info("session " + session.connectionId + " 媒体端点超时");
    }

    private Session session(long connectionId) {
        Session session = sessions.get(connectionId);
        if (session == null) {
            session = new Session(connectionId, host.peerAddress(connectionId),
                    clock.nanoTime(), tuning);
            sessions.put(connectionId, session);
        }
        return session;
    }

    private String allocateRoomCode() {
        for (int attempt = 0; attempt < ROOM_CODE_ATTEMPTS; attempt++) {
            String code = roomCodes.nextCode();
            if (ControlProtocol.isValidRoomCode(code) && !roomsByCode.containsKey(code)) {
                return code;
            }
        }
        return null;
    }

    private static long egressFor(ControlProtocol.ProfileInfo profile) {
        return profile.bitrate() + profile.bitrate() / 4L;
    }

    /** Publisher ingress allowance: profile bitrate x2, expressed in bytes per second. */
    private static long ingressBytesPerSecond(ControlProtocol.ProfileInfo profile) {
        return Math.max(1L, profile.bitrate() / 4L);
    }

    private static byte[] newToken() {
        byte[] token = new byte[16];
        do {
            RANDOM.nextBytes(token);
        } while (allZero(token));
        return token;
    }

    private static boolean allZero(byte[] bytes) {
        for (byte value : bytes) {
            if (value != 0) {
                return false;
            }
        }
        return true;
    }

    private static final class Session {
        final long connectionId;
        final TokenBucket requests;
        InetAddress peer;
        long lastActivityNanos;
        boolean handshaken;
        Long roomId;
        ControlProtocol.Role role;
        byte[] token;
        InetSocketAddress udpEndpoint;
        long udpActivityNanos;
        long udpRegisterDeadlineNanos;
        long readyEpoch;
        long lastFeedbackNanos;
        ControlProtocol.Feedback feedback;
        long forwardedFragments;
        long sendDroppedFragments;
        long egressLimitedFragments;
        long notReadyFragments;

        Session(long connectionId, InetAddress peer, long nowNanos, Tuning tuning) {
            this.connectionId = connectionId;
            this.peer = peer;
            this.lastActivityNanos = nowNanos;
            this.requests = new TokenBucket(tuning.requestsPerSecond(),
                    tuning.requestBurst(), nowNanos);
        }

        SessionGate.SessionPhase phase() {
            if (!handshaken) {
                return SessionGate.SessionPhase.UNHANDSHAKEN;
            }
            return roomId == null
                    ? SessionGate.SessionPhase.IDLE
                    : SessionGate.SessionPhase.IN_ROOM;
        }

        void clearMembership() {
            roomId = null;
            role = null;
            token = null;
            udpEndpoint = null;
            udpActivityNanos = 0;
            udpRegisterDeadlineNanos = 0;
            readyEpoch = 0;
            lastFeedbackNanos = 0;
            feedback = null;
            forwardedFragments = 0;
            sendDroppedFragments = 0;
            egressLimitedFragments = 0;
            notReadyFragments = 0;
        }
    }

    private static final class Room {
        final long id;
        final String code;
        ControlProtocol.ProfileInfo profile;
        final Session owner;
        final Map<Long, Session> viewers = new LinkedHashMap<>();
        final ByteRateLimiter ingress;
        ControlProtocol.RoomState state = ControlProtocol.RoomState.PREPARING;
        long prepareDeadlineNanos;
        ControlProtocol.StreamFormat format;
        long streamEpoch;
        long lastKeyframeNanos;
        long highestSequence;
        long fanoutCursor;
        long fairYieldFragments;
        boolean closed;

        Room(long id, String code, ControlProtocol.ProfileInfo profile, Session owner,
                long nowNanos, Tuning tuning) {
            this.id = id;
            this.code = code;
            this.profile = profile;
            this.owner = owner;
            this.ingress = new ByteRateLimiter(ingressBytesPerSecond(profile),
                    tuning.ingressBurstBytes(), nowNanos);
        }
    }
}
