package vusiif.ztohub.desktopshare.room;

import java.time.Duration;

/**
 * Room-controller limits and timers. Defaults are the phase-2 planning values from
 * {@code STEP.md}; tests build smaller variants through {@link Builder}.
 *
 * <p>The controller reads a single immutable instance, so capacity changes require a
 * server restart, never a client request.
 */
public record Tuning(
        int maxRooms,
        int maxMembersPerServer,
        int maxViewersPerRoom,
        long egressBudgetBitsPerSecond,
        long controlIdleNanos,
        long udpIdleNanos,
        long udpRegisterTimeoutNanos,
        long prepareTimeoutNanos,
        int requestsPerSecond,
        int requestBurst,
        long joinFailureWindowNanos,
        int joinFailureLimit,
        long joinFailureIdleNanos,
        int joinFailureSources,
        long keyframeIntervalNanos,
        long feedbackIntervalNanos,
        long maintenanceIntervalMillis,
        long slowConnectionBytes,
        long statsLogIntervalNanos,
        long ingressBurstBytes,
        long egressBurstBytes) {
    /** Fragments further than this behind the newest accepted sequence are stale. */
    public static final int SEQUENCE_WINDOW = 1024;

    public Tuning {
        requirePositive(maxRooms, "maxRooms");
        requirePositive(maxMembersPerServer, "maxMembersPerServer");
        requirePositive(maxViewersPerRoom, "maxViewersPerRoom");
        requirePositive(egressBudgetBitsPerSecond, "egressBudgetBitsPerSecond");
        requirePositive(controlIdleNanos, "controlIdleNanos");
        requirePositive(udpIdleNanos, "udpIdleNanos");
        requirePositive(udpRegisterTimeoutNanos, "udpRegisterTimeoutNanos");
        requirePositive(prepareTimeoutNanos, "prepareTimeoutNanos");
        requirePositive(requestsPerSecond, "requestsPerSecond");
        requirePositive(requestBurst, "requestBurst");
        requirePositive(joinFailureWindowNanos, "joinFailureWindowNanos");
        requirePositive(joinFailureLimit, "joinFailureLimit");
        requirePositive(joinFailureIdleNanos, "joinFailureIdleNanos");
        requirePositive(joinFailureSources, "joinFailureSources");
        requirePositive(keyframeIntervalNanos, "keyframeIntervalNanos");
        requirePositive(feedbackIntervalNanos, "feedbackIntervalNanos");
        requirePositive(maintenanceIntervalMillis, "maintenanceIntervalMillis");
        requirePositive(slowConnectionBytes, "slowConnectionBytes");
        requirePositive(statsLogIntervalNanos, "statsLogIntervalNanos");
        requirePositive(ingressBurstBytes, "ingressBurstBytes");
        requirePositive(egressBurstBytes, "egressBurstBytes");
        if (joinFailureIdleNanos < joinFailureWindowNanos) {
            throw new IllegalArgumentException("joinFailureIdleNanos must cover joinFailureWindowNanos");
        }
    }

    public static Tuning defaults() {
        return builder().build();
    }

    public static Builder builder() {
        return new Builder();
    }

    private static void requirePositive(long value, String name) {
        if (value <= 0) {
            throw new IllegalArgumentException(name + " must be positive");
        }
    }

    /** Mutable builder with the documented phase-2 defaults. */
    public static final class Builder {
        private int maxRooms = 8;
        private int maxMembersPerServer = 72;
        private int maxViewersPerRoom = 8;
        private long egressBudgetBitsPerSecond = 160_000_000L;
        private long controlIdleNanos = Duration.ofSeconds(15).toNanos();
        private long udpIdleNanos = Duration.ofSeconds(15).toNanos();
        private long udpRegisterTimeoutNanos = Duration.ofSeconds(5).toNanos();
        private long prepareTimeoutNanos = Duration.ofSeconds(10).toNanos();
        private int requestsPerSecond = 20;
        private int requestBurst = 20;
        private long joinFailureWindowNanos = Duration.ofMinutes(1).toNanos();
        private int joinFailureLimit = 10;
        private long joinFailureIdleNanos = Duration.ofMinutes(2).toNanos();
        private int joinFailureSources = 4096;
        private long keyframeIntervalNanos = Duration.ofSeconds(1).toNanos();
        private long feedbackIntervalNanos = Duration.ofSeconds(1).toNanos();
        private long maintenanceIntervalMillis = 1000;
        private long slowConnectionBytes = 256L * 1024;
        private long statsLogIntervalNanos = Duration.ofSeconds(10).toNanos();
        private long ingressBurstBytes = 1024L * 1024;
        private long egressBurstBytes = 1024L * 1024;

        private Builder() {}

        public Builder maxRooms(int value) {
            maxRooms = value;
            return this;
        }

        public Builder maxMembersPerServer(int value) {
            maxMembersPerServer = value;
            return this;
        }

        public Builder maxViewersPerRoom(int value) {
            maxViewersPerRoom = value;
            return this;
        }

        public Builder egressBudgetBitsPerSecond(long value) {
            egressBudgetBitsPerSecond = value;
            return this;
        }

        public Builder controlIdleMillis(long value) {
            controlIdleNanos = Duration.ofMillis(value).toNanos();
            return this;
        }

        public Builder udpIdleMillis(long value) {
            udpIdleNanos = Duration.ofMillis(value).toNanos();
            return this;
        }

        public Builder udpRegisterTimeoutMillis(long value) {
            udpRegisterTimeoutNanos = Duration.ofMillis(value).toNanos();
            return this;
        }

        public Builder prepareTimeoutMillis(long value) {
            prepareTimeoutNanos = Duration.ofMillis(value).toNanos();
            return this;
        }

        public Builder requestsPerSecond(int value) {
            requestsPerSecond = value;
            return this;
        }

        public Builder requestBurst(int value) {
            requestBurst = value;
            return this;
        }

        public Builder joinFailureWindowMillis(long value) {
            joinFailureWindowNanos = Duration.ofMillis(value).toNanos();
            return this;
        }

        public Builder joinFailureLimit(int value) {
            joinFailureLimit = value;
            return this;
        }

        public Builder joinFailureIdleMillis(long value) {
            joinFailureIdleNanos = Duration.ofMillis(value).toNanos();
            return this;
        }

        public Builder joinFailureSources(int value) {
            joinFailureSources = value;
            return this;
        }

        public Builder keyframeIntervalMillis(long value) {
            keyframeIntervalNanos = Duration.ofMillis(value).toNanos();
            return this;
        }

        public Builder feedbackIntervalMillis(long value) {
            feedbackIntervalNanos = Duration.ofMillis(value).toNanos();
            return this;
        }

        public Builder maintenanceIntervalMillis(long value) {
            maintenanceIntervalMillis = value;
            return this;
        }

        public Builder slowConnectionBytes(long value) {
            slowConnectionBytes = value;
            return this;
        }

        public Builder statsLogIntervalMillis(long value) {
            statsLogIntervalNanos = Duration.ofMillis(value).toNanos();
            return this;
        }

        /** Publisher ingress burst; the default is the 1 MiB documented in STEP.md. */
        public Builder ingressBurstBytes(long value) {
            ingressBurstBytes = value;
            return this;
        }

        /** Server-wide egress burst; the default is the 1 MiB documented in STEP.md. */
        public Builder egressBurstBytes(long value) {
            egressBurstBytes = value;
            return this;
        }

        public Tuning build() {
            return new Tuning(maxRooms, maxMembersPerServer, maxViewersPerRoom,
                    egressBudgetBitsPerSecond, controlIdleNanos, udpIdleNanos,
                    udpRegisterTimeoutNanos, prepareTimeoutNanos, requestsPerSecond, requestBurst,
                    joinFailureWindowNanos, joinFailureLimit, joinFailureIdleNanos,
                    joinFailureSources, keyframeIntervalNanos, feedbackIntervalNanos,
                    maintenanceIntervalMillis, slowConnectionBytes, statsLogIntervalNanos,
                    ingressBurstBytes, egressBurstBytes);
        }
    }
}
