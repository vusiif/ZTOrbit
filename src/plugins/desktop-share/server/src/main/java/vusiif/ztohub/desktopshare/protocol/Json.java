package vusiif.ztohub.desktopshare.protocol;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Deque;

/**
 * Deterministic compact JSON writer. The desktop-share control contract compares
 * encoded messages byte for byte across C++ and Java, so object keys are written
 * in the caller's order and numbers keep their integer lexical form.
 */
final class Json {
    private final StringBuilder output = new StringBuilder();
    private final Deque<Level> levels = new ArrayDeque<>();
    private String pendingKey;

    private static final class Level {
        final boolean object;
        boolean first = true;

        Level(boolean object) {
            this.object = object;
        }
    }

    void beginObject() {
        beforeValue();
        output.append('{');
        levels.push(new Level(true));
    }

    void endObject() {
        if (levels.isEmpty() || !levels.peek().object) throw new IllegalStateException("not in object");
        output.append('}');
        levels.pop();
    }

    void beginArray() {
        beforeValue();
        output.append('[');
        levels.push(new Level(false));
    }

    void endArray() {
        if (levels.isEmpty() || levels.peek().object) throw new IllegalStateException("not in array");
        output.append(']');
        levels.pop();
    }

    void key(String key) {
        if (levels.isEmpty() || !levels.peek().object) throw new IllegalStateException("not in object");
        if (pendingKey != null) throw new IllegalStateException("pending key");
        pendingKey = key;
    }

    void string(String value) {
        beforeValue();
        output.append('"');
        appendEscaped(value);
        output.append('"');
    }

    void number(int value) {
        beforeValue();
        output.append(value);
    }

    void number(long value) {
        beforeValue();
        output.append(value);
    }

    void bool(boolean value) {
        beforeValue();
        output.append(value ? "true" : "false");
    }

    void nul() {
        beforeValue();
        output.append("null");
    }

    byte[] take() {
        if (!levels.isEmpty()) throw new IllegalStateException("unbalanced JSON writer");
        if (pendingKey != null) throw new IllegalStateException("pending key");
        return output.toString().getBytes(StandardCharsets.UTF_8);
    }

    private void beforeValue() {
        if (levels.isEmpty()) return;
        Level level = levels.peek();
        if (!level.first) output.append(',');
        level.first = false;
        if (level.object) {
            if (pendingKey == null) throw new IllegalStateException("missing key");
            output.append('"');
            appendEscaped(pendingKey);
            output.append('"').append(':');
            pendingKey = null;
        }
    }

    private void appendEscaped(String value) {
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            switch (c) {
                case '"' -> output.append("\\\"");
                case '\\' -> output.append("\\\\");
                case '\b' -> output.append("\\b");
                case '\f' -> output.append("\\f");
                case '\n' -> output.append("\\n");
                case '\r' -> output.append("\\r");
                case '\t' -> output.append("\\t");
                default -> {
                    if (c < 0x20) {
                        output.append("\\u00");
                        output.append(Character.forDigit(c >> 4, 16));
                        output.append(Character.forDigit(c & 0x0F, 16));
                    } else {
                        // Emit UTF-16 as UTF-8 at the end. Lone surrogates are
                        // replaced by the JDK encoder, matching the C++ side's
                        // QString-to-UTF-8 conversion.
                        output.append(c);
                    }
                }
            }
        }
    }
}
