package vusiif.ztohub.desktopshare.room;

import java.util.ArrayDeque;
import java.util.Deque;

/** Controllable room-code source for collision and leading-zero tests. */
final class TestRoomCodes implements RoomCodeGenerator {
    private final Deque<String> queued = new ArrayDeque<>();
    private String constant;
    private int counter;
    int calls;

    TestRoomCodes enqueue(String... values) {
        for (String value : values) {
            queued.addLast(value);
        }
        return this;
    }

    TestRoomCodes always(String value) {
        constant = value;
        return this;
    }

    void clearAlways() {
        constant = null;
    }

    void resetCalls() {
        calls = 0;
    }

    @Override
    public String nextCode() {
        calls++;
        if (constant != null) {
            return constant;
        }
        if (!queued.isEmpty()) {
            return queued.removeFirst();
        }
        counter++;
        String digits = Integer.toString(counter);
        return "0".repeat(Math.max(0, 8 - digits.length())) + digits;
    }
}
