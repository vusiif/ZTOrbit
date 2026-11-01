package vusiif.ztohub.main;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.io.IOException;

/** Shared strict JSON mapper. */
public final class JsonSupport {
    private static final ObjectMapper MAPPER = JsonMapper.builder()
            .configure(DeserializationFeature.FAIL_ON_TRAILING_TOKENS, true)
            .build();

    private JsonSupport() {}

    public static ObjectMapper mapper() {
        return MAPPER;
    }

    public static JsonNode parse(byte[] bytes) throws IOException {
        return MAPPER.readTree(bytes);
    }

    public static JsonNode parse(String text) throws JsonProcessingException {
        return MAPPER.readTree(text);
    }

    public static byte[] compact(JsonNode node) {
        try {
            return MAPPER.writeValueAsBytes(node);
        } catch (JsonProcessingException impossible) {
            throw new IllegalStateException("Cannot serialize JSON node", impossible);
        }
    }

    public static ObjectNode object() {
        return MAPPER.createObjectNode();
    }
}
