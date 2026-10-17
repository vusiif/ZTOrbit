package vusiif.ztgui.server.api;

import java.util.Objects;

/**
 * Data received from an authorized network member.
 *
 * @param networkId 16-character lowercase hex network ID
 * @param senderId 10-character lowercase hex sender member ID
 * @param data application payload
 */
public record NetworkMessage(String networkId, String senderId, byte[] data) {
    public NetworkMessage {
        Objects.requireNonNull(networkId, "networkId");
        Objects.requireNonNull(senderId, "senderId");
        Objects.requireNonNull(data, "data");
    }
}
