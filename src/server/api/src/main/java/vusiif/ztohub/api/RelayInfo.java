package vusiif.ztohub.api;

/**
 * One network relay known to the transport.
 *
 * @param nodeId relay member ID (10 lowercase hex characters)
 * @param moonId Moon ID used by the relay
 * @param virtualIp relay ZeroTier IPv4 address
 * @param port relay transport UDP port
 * @param averageLatency average probe latency in milliseconds, negative when unknown
 * @param lastLatency last probe latency in milliseconds, negative when unknown
 * @param available whether the last probes succeeded
 * @param sampledAt epoch milliseconds of the last successful probe, {@code 0} when never
 * @param bytesPerSecond relay-reported transport rate
 * @param queuedBytes relay-reported queued bytes
 */
public record RelayInfo(
        String nodeId,
        String moonId,
        String virtualIp,
        int port,
        double averageLatency,
        double lastLatency,
        boolean available,
        long sampledAt,
        long bytesPerSecond,
        long queuedBytes) {
}
