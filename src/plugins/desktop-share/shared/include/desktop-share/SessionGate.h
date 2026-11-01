#pragma once

#include <desktop-share/ControlProtocol.h>

namespace vusiif::zto::desktop_share {

// Protocol-level session gate. It has no room or role context, so it only
// enforces the handshake and whether a stream operation has a room. Repeated
// create/join and no-room leave/stop are allowed to reach the phase-2 room
// logic, which decides whether to return the current state or ALREADY_IN_ROOM.
// Role and permission checks also belong to phase 2.
enum class SessionPhase : quint8 {
    Unhandshaken = 0,
    Idle,
    InRoom,
};

inline bool operationAllowed(SessionPhase phase, Operation operation)
{
    switch (phase) {
    case SessionPhase::Unhandshaken:
        return operation == Operation::Hello;
    case SessionPhase::Idle:
        // Idempotent leave/stop and repeated create/join have no room context
        // here; let the room logic answer them. Stream operations still need a
        // room and are rejected below.
        return operation == Operation::Hello || operation == Operation::Create
            || operation == Operation::Join || operation == Operation::Leave
            || operation == Operation::Stop || operation == Operation::Ping;
    case SessionPhase::InRoom:
        // The room controller decides same-room idempotency, different-room
        // ALREADY_IN_ROOM, and publisher/viewer permissions.
        return operation == Operation::Hello || operation == Operation::Create
            || operation == Operation::Join || operation == Operation::Leave
            || operation == Operation::Stop || operation == Operation::Ping
            || operation == Operation::StreamConfigure || operation == Operation::StreamReady
            || operation == Operation::StreamKeyframe || operation == Operation::StreamFeedback;
    }
    return false;
}

inline ErrorCode operationGateError(SessionPhase phase, Operation operation)
{
    if (operationAllowed(phase, operation)) return ErrorCode::None;
    // Only unhandshaken non-hello operations and idle stream operations are
    // rejected here. The gate never returns ALREADY_IN_ROOM: that requires
    // comparing the requested room with the current room, which phase 2 owns.
    return ErrorCode::NotReady;
}

} // namespace vusiif::zto::desktop_share
