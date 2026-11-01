package vusiif.ztohub.desktopshare.room;

import java.security.SecureRandom;

/**
 * Room codes are eight decimal digits including leading zeros. They identify a room
 * only on the server that generated them, so they are not authentication material.
 */
public final class SecureRoomCodeGenerator implements RoomCodeGenerator {
    private static final int SPACE = 100_000_000;

    private final SecureRandom random = new SecureRandom();

    @Override
    public String nextCode() {
        int value = random.nextInt(SPACE);
        String digits = Integer.toString(value);
        return "0".repeat(8 - digits.length()) + digits;
    }
}
