package vusiif.ztgui.server.api;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

class ApiVersionTest {
    @Test
    void valueIsApiGenerationWithOptionalQualifier() {
        assertTrue(ApiVersion.value().matches("[0-9]+\\.[0-9]+(?:-[0-9A-Za-z.-]+)?"),
                ApiVersion.value());
    }

    @Test
    void matchesOnlyExactApiVersion() {
        String value = ApiVersion.value();
        assertTrue(ApiVersion.matches(value));
        assertFalse(ApiVersion.matches(null));
        assertFalse(ApiVersion.matches(value + ".1"));
        assertFalse(ApiVersion.matches("v" + value));
        assertFalse(ApiVersion.matches(""));
    }
}
