package vusiif.ztohub.desktopshare.protocol;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

class ContractBehaviorTest {
    @Test
    void pendingRequestTimeoutAndLateResponse() {
        PendingRequests pending = new PendingRequests();
        for (long id = 1; id <= PendingRequests.MAX_OUTSTANDING; ++id) {
            assertTrue(pending.add(id, 1000));
        }
        assertEquals(PendingRequests.MAX_OUTSTANDING, pending.size());
        assertFalse(pending.add(PendingRequests.MAX_OUTSTANDING + 1, 1000));
        assertFalse(pending.add(1, 1000));
        assertTrue(pending.complete(1, 1000 + PendingRequests.TIMEOUT_MS - 1));
        assertFalse(pending.complete(1, 1000 + PendingRequests.TIMEOUT_MS));
        assertFalse(pending.complete(2, 1000 + PendingRequests.TIMEOUT_MS));
        assertEquals(14, pending.expire(1000 + PendingRequests.TIMEOUT_MS));
        assertEquals(0, pending.size());
        // A late response never resurrects state after the deadline.
        assertTrue(pending.add(99, 5000));
        assertFalse(pending.complete(99, 5000 + PendingRequests.TIMEOUT_MS));
        assertFalse(pending.contains(99, 5000 + PendingRequests.TIMEOUT_MS));
    }

    @Test
    void sessionGateAllowsIdempotentRoomOperationsAndRejectsNoRoomStreams() {
        for (ControlProtocol.Operation operation : ControlProtocol.Operation.values()) {
            // Before hello only the handshake is legal.
            if (operation == ControlProtocol.Operation.HELLO) {
                assertTrue(SessionGate.operationAllowed(SessionGate.SessionPhase.UNHANDSHAKEN,
                        operation), operation.name());
                assertEquals(ControlProtocol.ErrorCode.NONE,
                        SessionGate.gateError(SessionGate.SessionPhase.UNHANDSHAKEN, operation),
                        operation.name());
            } else {
                assertFalse(SessionGate.operationAllowed(SessionGate.SessionPhase.UNHANDSHAKEN,
                        operation), operation.name());
                assertEquals(ControlProtocol.ErrorCode.NOT_READY,
                        SessionGate.gateError(SessionGate.SessionPhase.UNHANDSHAKEN, operation),
                        operation.name());
            }
            // Idle: create/join and the idempotent leave/stop reach room logic;
            // a stream operation still requires a room.
            if (isStream(operation)) {
                assertFalse(SessionGate.operationAllowed(SessionGate.SessionPhase.IDLE,
                        operation), operation.name());
                assertEquals(ControlProtocol.ErrorCode.NOT_READY,
                        SessionGate.gateError(SessionGate.SessionPhase.IDLE, operation),
                        operation.name());
            } else {
                assertTrue(SessionGate.operationAllowed(SessionGate.SessionPhase.IDLE,
                        operation), operation.name());
                assertEquals(ControlProtocol.ErrorCode.NONE,
                        SessionGate.gateError(SessionGate.SessionPhase.IDLE, operation),
                        operation.name());
            }
            // In a room the gate has no room or role context: every operation
            // must reach phase-2 room logic.
            assertTrue(SessionGate.operationAllowed(SessionGate.SessionPhase.IN_ROOM, operation),
                    operation.name());
            assertEquals(ControlProtocol.ErrorCode.NONE,
                    SessionGate.gateError(SessionGate.SessionPhase.IN_ROOM, operation),
                    operation.name());
        }
        // Explicit coverage for the regression paths reported in STEP.md.
        assertEquals(ControlProtocol.ErrorCode.NONE, SessionGate.gateError(
                SessionGate.SessionPhase.IDLE, ControlProtocol.Operation.LEAVE));
        assertEquals(ControlProtocol.ErrorCode.NONE, SessionGate.gateError(
                SessionGate.SessionPhase.IDLE, ControlProtocol.Operation.STOP));
        assertEquals(ControlProtocol.ErrorCode.NONE, SessionGate.gateError(
                SessionGate.SessionPhase.IN_ROOM, ControlProtocol.Operation.CREATE));
        assertEquals(ControlProtocol.ErrorCode.NONE, SessionGate.gateError(
                SessionGate.SessionPhase.IN_ROOM, ControlProtocol.Operation.JOIN));
    }

    private static boolean isStream(ControlProtocol.Operation operation) {
        return operation == ControlProtocol.Operation.STREAM_CONFIGURE
                || operation == ControlProtocol.Operation.STREAM_READY
                || operation == ControlProtocol.Operation.STREAM_KEYFRAME
                || operation == ControlProtocol.Operation.STREAM_FEEDBACK;
    }
}
