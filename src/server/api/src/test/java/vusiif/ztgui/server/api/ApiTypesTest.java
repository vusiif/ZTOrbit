package vusiif.ztgui.server.api;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.net.InetSocketAddress;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;

class ApiTypesTest {
    private static final ObjectMapper JSON = new ObjectMapper();

    @Test
    void queryResultRequiresDocumentAndNoError() throws Exception {
        JsonNode document = JSON.readTree("{\"address\":\"0123456789\"}");
        assertTrue(new QueryResult(document, null).ok());
        assertTrue(QueryResult.success(document).ok());
        assertFalse(QueryResult.success(null).ok());
        assertFalse(new QueryResult(document, "failed").ok());
        assertFalse(QueryResult.failure("failed").ok());
    }

    @Test
    void fileResultCopiesDataAndReportsErrors() {
        byte[] source = {1, 2, 3};
        FileResult result = FileResult.success(source);
        source[0] = 9;
        assertArrayEquals(new byte[] {1, 2, 3}, result.data());
        assertTrue(result.ok());
        assertTrue(result.error().isEmpty());
        result.data()[1] = 8;
        assertEquals(2, result.data()[1]);
        assertNull(FileResult.failure("bad").data());
        assertFalse(FileResult.failure("bad").ok());
        assertTrue(FileResult.success(new byte[0]).ok());
    }

    @Test
    void logResultReportsEachDestination() {
        assertTrue(LogResult.success().ok());
        assertTrue(new LogResult("", "").ok());
        assertFalse(new LogResult("host", "").ok());
        assertFalse(new LogResult("", "plugin").ok());
        assertFalse(new LogResult(null, "plugin").ok());
    }

    @Test
    void sendResultCopiesMembersAndExposesOk() {
        SendResult result = SendResult.failure("partial", List.of("a", "b"));
        assertFalse(result.ok());
        assertEquals(List.of("a", "b"), result.failedMembers());
        assertTrue(SendResult.success(3).ok());
        assertTrue(SendResult.nothing().ok());
        assertTrue(new SendResult(0, null, null).failedMembers().isEmpty());
    }

    @Test
    void relayResultCopiesList() {
        RelayInfo info = new RelayInfo(
                "0123456789", "0000000123456789", "10.0.0.2", 18100, 5.0, 4.0, true, 1, 2048, 0);
        RelayResult result = RelayResult.success(List.of(info));
        assertTrue(result.ok());
        assertEquals(List.of(info), result.relays());
        assertFalse(RelayResult.failure("no").ok());
        assertEquals(List.of(), RelayResult.failure("no").relays());
    }

    @Test
    void recordsAndCallbacksBehave() {
        PluginInfo info = new PluginInfo("test", "Test", "1.0.0");
        assertEquals("test", info.id());
        NetworkMessage message = new NetworkMessage("network", "sender", new byte[] {4});
        assertEquals("network", message.networkId());
        assertTrue(message.data().length == 1);
        AtomicBoolean cancelled = new AtomicBoolean();
        Cancellable cancellable = () -> cancelled.set(true);
        cancellable.cancel();
        assertTrue(cancelled.get());
        InetSocketAddress endpoint = new InetSocketAddress("127.0.0.1", 18100);
        assertEquals(18100, endpoint.getPort());
    }
}
