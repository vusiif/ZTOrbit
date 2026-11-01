package vusiif.ztohub.desktopshare.protocol;

/**
 * Protocol-level session gate. It has no room or role context, so it only
 * enforces the handshake and whether a stream operation has a room. Repeated
 * create/join and no-room leave/stop are allowed to reach the phase-2 room
 * logic, which decides whether to return the current state or ALREADY_IN_ROOM.
 * Role and permission checks also belong to phase 2.
 */
public final class SessionGate {
    public enum SessionPhase {
        UNHANDSHAKEN,
        IDLE,
        IN_ROOM,
    }

    private SessionGate() {}

    public static boolean operationAllowed(SessionPhase phase,
            ControlProtocol.Operation operation) {
        return switch (phase) {
            case UNHANDSHAKEN -> operation == ControlProtocol.Operation.HELLO;
            case IDLE -> operation == ControlProtocol.Operation.HELLO
                    // Idempotent leave/stop and repeated create/join have no
                    // room context here; let the room logic answer them.
                    || operation == ControlProtocol.Operation.CREATE
                    || operation == ControlProtocol.Operation.JOIN
                    || operation == ControlProtocol.Operation.LEAVE
                    || operation == ControlProtocol.Operation.STOP
                    || operation == ControlProtocol.Operation.PING;
            case IN_ROOM -> operation == ControlProtocol.Operation.HELLO
                    // The room controller decides same-room idempotency,
                    // different-room ALREADY_IN_ROOM, and roles.
                    || operation == ControlProtocol.Operation.CREATE
                    || operation == ControlProtocol.Operation.JOIN
                    || operation == ControlProtocol.Operation.LEAVE
                    || operation == ControlProtocol.Operation.STOP
                    || operation == ControlProtocol.Operation.PING
                    || operation == ControlProtocol.Operation.STREAM_CONFIGURE
                    || operation == ControlProtocol.Operation.STREAM_READY
                    || operation == ControlProtocol.Operation.STREAM_KEYFRAME
                    || operation == ControlProtocol.Operation.STREAM_FEEDBACK;
        };
    }

    public static ControlProtocol.ErrorCode gateError(SessionPhase phase,
            ControlProtocol.Operation operation) {
        if (operationAllowed(phase, operation)) return ControlProtocol.ErrorCode.NONE;
        // Only unhandshaken non-hello operations and idle stream operations are
        // rejected here. The gate never returns ALREADY_IN_ROOM: that requires
        // comparing the requested room with the current room, which phase 2 owns.
        return ControlProtocol.ErrorCode.NOT_READY;
    }
}
