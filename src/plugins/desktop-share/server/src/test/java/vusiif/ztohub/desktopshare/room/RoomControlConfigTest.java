package vusiif.ztohub.desktopshare.room;

import java.nio.charset.StandardCharsets;
import java.util.List;
import org.junit.jupiter.api.Test;
import vusiif.ztohub.desktopshare.protocol.ControlProtocol;
import vusiif.ztohub.api.FileResult;
import vusiif.ztohub.api.PluginFiles;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class RoomControlConfigTest {
    @Test
    void missingFileUsesDocumentedDefaults() {
        RoomControlConfig.LoadResult result = RoomControlConfig.read(
                new FakeFiles(FileResult.failure("not found")));
        assertTrue(result.ok());
        assertFalse(result.present());
        assertEquals(RoomControlConfig.defaults(), result.config());
        assertEquals(8, result.config().maxRooms());
        assertEquals(8, result.config().maxViewersPerRoom());
        assertEquals(72, result.config().maxMembersPerServer());
        assertEquals(160, result.config().egressBudgetMbps());
        assertEquals(20, result.config().controlRequestsPerSecond());
        assertEquals(20, result.config().controlRequestBurst());
        assertEquals(10, result.config().joinFailureLimitPerMinute());
        assertEquals(4096, result.config().joinFailureSources());
    }

    @Test
    void emptyObjectUsesDefaultsAndPartialConfigKeepsOtherDefaults() {
        RoomControlConfig empty = parse("{}");
        assertEquals(RoomControlConfig.defaults(), empty);

        RoomControlConfig partial = parse("{\"maxViewersPerRoom\":2}");
        assertEquals(2, partial.maxViewersPerRoom());
        assertEquals(RoomControlConfig.defaults().maxRooms(), partial.maxRooms());
        assertEquals(RoomControlConfig.defaults().egressBudgetMbps(),
                partial.egressBudgetMbps());
    }

    @Test
    void validFullConfigIsAppliedToTuning() {
        RoomControlConfig config = parse("""
                {
                  "maxRooms": 2,
                  "maxViewersPerRoom": 3,
                  "maxMembersPerServer": 7,
                  "egressBudgetMbps": 25,
                  "controlRequestsPerSecond": 4,
                  "controlRequestBurst": 6,
                  "joinFailureLimitPerMinute": 5,
                  "joinFailureSources": 11
                }
                """);
        assertEquals(2, config.maxRooms());
        assertEquals(3, config.maxViewersPerRoom());
        assertEquals(7, config.maxMembersPerServer());
        assertEquals(25, config.egressBudgetMbps());
        assertEquals(4, config.controlRequestsPerSecond());
        assertEquals(6, config.controlRequestBurst());
        assertEquals(5, config.joinFailureLimitPerMinute());
        assertEquals(11, config.joinFailureSources());

        Tuning tuning = config.toTuning();
        assertEquals(2, tuning.maxRooms());
        assertEquals(3, tuning.maxViewersPerRoom());
        assertEquals(7, tuning.maxMembersPerServer());
        assertEquals(25_000_000L, tuning.egressBudgetBitsPerSecond());
        assertEquals(4, tuning.requestsPerSecond());
        assertEquals(6, tuning.requestBurst());
        assertEquals(5, tuning.joinFailureLimit());
        assertEquals(11, tuning.joinFailureSources());
        Tuning defaults = Tuning.defaults();
        assertEquals(defaults.controlIdleNanos(), tuning.controlIdleNanos());
        assertEquals(defaults.udpIdleNanos(), tuning.udpIdleNanos());
        assertEquals(defaults.udpRegisterTimeoutNanos(), tuning.udpRegisterTimeoutNanos());
        assertEquals(defaults.prepareTimeoutNanos(), tuning.prepareTimeoutNanos());
    }

    @Test
    void validConfiguredCapacityIsEnforcedByTheController() throws Exception {
        RoomControlConfig config = parse("""
                {
                  "maxRooms": 1,
                  "maxViewersPerRoom": 1,
                  "maxMembersPerServer": 2,
                  "egressBudgetMbps": 5
                }
                """);
        Harness harness = new Harness(config.toTuning());
        long publisher = harness.connect();
        ControlProtocol.HelloInfo hello = harness.hello(publisher).hello();
        assertEquals(1, hello.maxViewers());
        ControlProtocol.RoomInfo room = harness.create(publisher, "720p30").room();

        long viewer = harness.connect();
        harness.hello(viewer);
        assertTrue(harness.join(viewer, room.roomCode()).ok());

        long secondViewer = harness.connect();
        harness.hello(secondViewer);
        ControlProtocol.Response full = harness.join(secondViewer, room.roomCode());
        assertFalse(full.ok());
        assertEquals(ControlProtocol.ErrorCode.ROOM_FULL, full.error());

        long secondPublisher = harness.connect();
        harness.hello(secondPublisher);
        ControlProtocol.Response capacity = harness.create(secondPublisher, "720p30");
        assertFalse(capacity.ok());
        assertEquals(ControlProtocol.ErrorCode.CAPACITY, capacity.error());
        assertEquals(1, harness.stats().rooms());
        assertEquals(2, harness.stats().members());
    }

    @Test
    void invalidInputsReportFieldAndReason() {
        record Invalid(String json, String expected) {}
        List<Invalid> cases = List.of(
                new Invalid("{", "JSON 解析失败"),
                new Invalid("[]", "JSON 对象"),
                new Invalid("{\"unknown\":1}", "未知字段 unknown"),
                new Invalid("{\"maxRooms\":\"2\"}", "maxRooms 必须是 1..8 的整数"),
                new Invalid("{\"maxRooms\":1.5}", "maxRooms 必须是 1..8 的整数"),
                new Invalid("{\"maxRooms\":null}", "maxRooms 必须是 1..8 的整数"),
                new Invalid("{\"maxRooms\":0}", "maxRooms 必须在 1..8 之间"),
                new Invalid("{\"maxRooms\":9}", "maxRooms 必须在 1..8 之间"),
                new Invalid("{\"maxViewersPerRoom\":9}", "maxViewersPerRoom 必须在 1..8 之间"),
                new Invalid("{\"maxMembersPerServer\":73}", "maxMembersPerServer 必须在 1..72 之间"),
                new Invalid("{\"egressBudgetMbps\":161}", "egressBudgetMbps 必须在 1..160 之间"),
                new Invalid("{\"controlRequestsPerSecond\":21}",
                        "controlRequestsPerSecond 必须在 1..20 之间"),
                new Invalid("{\"controlRequestBurst\":21}",
                        "controlRequestBurst 必须在 1..20 之间"),
                new Invalid("{\"joinFailureLimitPerMinute\":11}",
                        "joinFailureLimitPerMinute 必须在 1..10 之间"),
                new Invalid("{\"joinFailureSources\":4097}",
                        "joinFailureSources 必须在 1..4096 之间"));
        for (Invalid item : cases) {
            RoomControlConfig.ConfigException error = assertThrows(
                    RoomControlConfig.ConfigException.class,
                    () -> RoomControlConfig.fromJson(bytes(item.json)));
            assertTrue(error.getMessage().contains(item.expected),
                    item.json + " -> " + error.getMessage());
        }
    }

    @Test
    void duplicateKeysAndOversizedInputAreRejected() {
        assertThrows(RoomControlConfig.ConfigException.class,
                () -> RoomControlConfig.fromJson(bytes(
                        "{\"maxRooms\":1,\"maxRooms\":2}")));
        RoomControlConfig.ConfigException oversized = assertThrows(
                RoomControlConfig.ConfigException.class,
                () -> RoomControlConfig.fromJson(new byte[RoomControlConfig.MAX_CONFIG_BYTES + 1]));
        assertTrue(oversized.getMessage().contains("字节上限"));
        RoomControlConfig.ConfigException empty = assertThrows(
                RoomControlConfig.ConfigException.class,
                () -> RoomControlConfig.fromJson(new byte[0]));
        assertTrue(empty.getMessage().contains("不能为空"));
    }

    @Test
    void hardMaximumValuesAreAccepted() {
        RoomControlConfig config = parse("""
                {
                  "maxRooms": 8,
                  "maxViewersPerRoom": 8,
                  "maxMembersPerServer": 72,
                  "egressBudgetMbps": 160,
                  "controlRequestsPerSecond": 20,
                  "controlRequestBurst": 20,
                  "joinFailureLimitPerMinute": 10,
                  "joinFailureSources": 4096
                }
                """);
        assertEquals(RoomControlConfig.defaults(), config);
    }

    @Test
    void readFailureIsDiagnosticButMissingFileIsNot() {
        RoomControlConfig.LoadResult missing = RoomControlConfig.read(
                new FakeFiles(FileResult.failure("not found")));
        assertTrue(missing.ok());
        assertFalse(missing.present());
        assertEquals("", missing.error());

        RoomControlConfig.LoadResult failure = RoomControlConfig.read(
                new FakeFiles(FileResult.failure("permission denied")));
        assertFalse(failure.ok());
        assertNull(failure.config());
        assertTrue(failure.error().contains("读取 config.json 失败"));
        assertTrue(failure.error().contains("permission denied"));

        RoomControlConfig.LoadResult invalid = RoomControlConfig.read(
                new FakeFiles(FileResult.success(bytes("{\"maxRooms\":9}"))));
        assertFalse(invalid.ok());
        assertTrue(invalid.present());
        assertTrue(invalid.error().contains("config.json 无效"));
        assertTrue(invalid.error().contains("maxRooms"));
        assertNotNull(invalid.error());
    }

    private static RoomControlConfig parse(String json) {
        try {
            return RoomControlConfig.fromJson(bytes(json));
        } catch (RoomControlConfig.ConfigException error) {
            throw new AssertionError(error.getMessage(), error);
        }
    }

    private static byte[] bytes(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }

    private static final class FakeFiles implements PluginFiles {
        private final FileResult read;

        FakeFiles(FileResult read) {
            this.read = read;
        }

        @Override
        public FileResult readFile(String relativePath) {
            assertEquals(RoomControlConfig.CONFIG_FILE, relativePath);
            return read;
        }

        @Override
        public FileResult writeFile(String relativePath, byte[] data) {
            throw new UnsupportedOperationException("read-only test double");
        }
    }
}
